/*
 * Wayland backend: xdg-shell + wl_shm, keyboard via xkbcommon.
 * The configure/ack sequencing and buffer handling follow foot's
 * wayland.c and shm.c (Copyright (c) 2019 Daniel Eklöf, MIT; see
 * LICENSE).
 */
#include "backend/backend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1.h"
#include "fractional-scale-v1.h"
#include "primary-selection-unstable-v1.h"
#include "viewporter.h"
#include "xdg-decoration-unstable-v1.h"
#include "xdg-shell.h"

#define LOG_MODULE "wayland"
#include "core/loop.h"
#include "core/util.h"

#define MAX_BUFFERS 3

struct wl_buf {
    struct buffer base;
    struct wl_buffer *wl_buffer;
    size_t size;
};

struct backend {
    struct loop *loop;
    const struct backend_listener *listener;
    void *data;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wl_data_device_manager *data_device_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_manager;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame_cb;
    /* Both present (or neither): buffers are `width * scale` pixels and the
     * viewport maps them onto the `width` x `height` logical surface. */
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional_scale;
    double scale;

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t enter_serial; /* of the last wl_pointer.enter; cursor-shape needs it */
    enum cursor_shape shape;

    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_v1 *primary_selection_device;
    uint32_t last_serial; /* from the last key or button event; needed to claim a selection */

    /* Text this window owns per target, offered to a wl_data_source /
     * zwp_primary_selection_source_v1 created fresh on every claim. */
    struct {
        char *text;
        size_t len;
    } sel_owned[2];
    struct wl_data_source *clipboard_source;
    struct zwp_primary_selection_source_v1 *primary_source;

    /* The other side's current offer; replaced or nulled by .selection. */
    struct wl_data_offer *clipboard_offer;
    struct zwp_primary_selection_offer_v1 *primary_offer;

    /* One paste in flight: a pipe registered with the loop, growing buffer. */
    struct {
        bool active;
        enum selection_target target;
        int fd;
        char *data;
        size_t len, cap;
    } paste_recv;

    /* Last known pointer position (wl_pointer.button/axis carry no coordinates
     * of their own); [0]=vertical, [1]=horizontal for the axis accumulators. */
    int ptr_x, ptr_y;
    int32_t axis_v120[2], axis_discrete[2];
    wl_fixed_t axis_cont[2], axis_cont_remainder[2];
    bool axis_touched[2];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;

    /* Key repeat is done client-side on Wayland */
    int repeat_timer;
    int32_t repeat_rate, repeat_delay;
    uint32_t repeat_key; /* xkb keycode; 0 when not repeating */

    int width, height; /* logical (surface) size */
    int pending_width, pending_height;
    bool configured;

    struct wl_buf bufs[MAX_BUFFERS];
};

/* Logical length to buffer pixels. */
static int
to_pixels(const struct backend *b, int logical) {
    return (int)lround(logical * b->scale);
}

/* ---- buffers ---- */

static void
buffer_release(void *data, struct wl_buffer *wl_buffer) {
    struct wl_buf *buf = data;
    buf->base.busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void
buffer_destroy(struct wl_buf *buf) {
    if (buf->base.pix != NULL)
        pixman_image_unref(buf->base.pix);
    if (buf->wl_buffer != NULL)
        wl_buffer_destroy(buf->wl_buffer);
    if (buf->base.data != NULL)
        munmap(buf->base.data, buf->size);
    *buf = (struct wl_buf){0};
}

static bool
buffer_create(struct backend *b, struct wl_buf *buf, int width, int height) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;

    int fd = memfd_create("astralia-wl-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        LOG_ERRNO("memfd_create failed");
        return false;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        LOG_ERRNO("ftruncate failed");
        close(fd);
        return false;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_ERRNO("mmap failed");
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(b->shm, fd, (int32_t)size);
    struct wl_buffer *wl_buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    *buf = (struct wl_buf){
        .base = {
            .width = width,
            .height = height,
            .stride = stride,
            .data = data,
            .pix = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
                                                     data, stride),
        },
        .wl_buffer = wl_buffer,
        .size = size,
    };
    wl_buffer_add_listener(wl_buffer, &buffer_listener, buf);
    return buf->base.pix != NULL;
}

static struct buffer *
wl_get_buffer(struct backend *b, int width, int height) {
    /* Attaching a buffer before the first configure is acked is a
     * protocol error; xdg_surface_configure() wakes the app to draw. */
    if (!b->configured)
        return NULL;

    /* Size change: drop every buffer; a busy one is still valid for the
     * compositor after wl_buffer.destroy, it just won't be released to us. */
    for (int i = 0; i < MAX_BUFFERS; i++) {
        struct wl_buf *buf = &b->bufs[i];
        if (buf->wl_buffer != NULL &&
            (buf->base.width != width || buf->base.height != height))
            buffer_destroy(buf);
    }

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (b->bufs[i].wl_buffer != NULL && !b->bufs[i].base.busy)
            return &b->bufs[i].base;
    }
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (b->bufs[i].wl_buffer == NULL) {
            if (!buffer_create(b, &b->bufs[i], width, height))
                return NULL;
            return &b->bufs[i].base;
        }
    }
    return NULL;
}

static void
frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    struct backend *b = data;
    wl_callback_destroy(cb);
    b->frame_cb = NULL;
    b->listener->frame(b->data);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void
wl_commit(struct backend *b, struct buffer *base, pixman_region32_t *damage) {
    struct wl_buf *buf = (struct wl_buf *)base;

    wl_surface_attach(b->surface, buf->wl_buffer, 0, 0);
    if (b->viewport != NULL)
        wp_viewport_set_destination(b->viewport, b->width, b->height);

    int n;
    pixman_box32_t *boxes = pixman_region32_rectangles(damage, &n);
    for (int i = 0; i < n; i++) {
        wl_surface_damage_buffer(b->surface, boxes[i].x1, boxes[i].y1,
                                 boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
    }

    if (b->frame_cb == NULL) {
        b->frame_cb = wl_surface_frame(b->surface);
        wl_callback_add_listener(b->frame_cb, &frame_listener, b);
    }
    wl_surface_commit(b->surface);
    buf->base.busy = true;
}

/* ---- xdg-shell ---- */

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                   int32_t width, int32_t height, struct wl_array *states) {
    struct backend *b = data;
    b->pending_width = width;
    b->pending_height = height;
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct backend *b = data;
    b->listener->close(b->data);
}

static void
toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                          int32_t width, int32_t height) {
}

static void
toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                         struct wl_array *caps) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

/* xdg_surface.configure ends a configure sequence: ack it, then the next
 * commit (from the app's render hook) carries a buffer of the new size. */
static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct backend *b = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    int width = b->pending_width > 0 ? b->pending_width : b->width;
    int height = b->pending_height > 0 ? b->pending_height : b->height;

    if (!b->configured || width != b->width || height != b->height) {
        b->configured = true;
        b->width = width;
        b->height = height;
        b->listener->configure(b->data, to_pixels(b, width), to_pixels(b, height));
    }

    /* Without a pending frame callback nothing would wake the app to draw
     * the resized frame; the compositor expects one after the ack. */
    if (b->frame_cb == NULL)
        b->listener->frame(b->data);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

/* preferred_scale is in 120ths. A change after the first configure re-sends
 * the buffer size, since the logical size stays the same. */
static void
fractional_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale120) {
    struct backend *b = data;
    double scale = scale120 / 120.0;
    if (scale == b->scale)
        return;
    b->scale = scale;
    b->listener->scale(b->data, scale);
    if (b->configured) {
        b->listener->configure(b->data, to_pixels(b, b->width), to_pixels(b, b->height));
        if (b->frame_cb == NULL)
            b->listener->frame(b->data);
    }
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_preferred_scale,
};

/* ---- keyboard ---- */

/* Current keyboard modifier state, for key events and pointer events alike. */
static unsigned
mods_from_xkb(struct backend *b) {
    struct {
        const char *name;
        unsigned mod;
    } mods[] = {
        {XKB_MOD_NAME_SHIFT, MOD_SHIFT},
        {XKB_MOD_NAME_ALT, MOD_ALT},
        {XKB_MOD_NAME_CTRL, MOD_CTRL},
        {XKB_MOD_NAME_LOGO, MOD_SUPER},
    };
    unsigned result = 0;
    if (b->xkb_state == NULL)
        return result;
    for (size_t i = 0; i < ARRAY_LEN(mods); i++) {
        if (xkb_state_mod_name_is_active(b->xkb_state, mods[i].name,
                                         XKB_STATE_MODS_EFFECTIVE) > 0)
            result |= mods[i].mod;
    }
    return result;
}

static void
emit_key(struct backend *b, xkb_keycode_t code, enum key_action action) {
    struct key_event ev = {.sym = xkb_state_key_get_one_sym(b->xkb_state, code),
                           .action = action,
                           .mods = mods_from_xkb(b)};

    int n = xkb_state_key_get_utf8(b->xkb_state, code, ev.utf8, sizeof(ev.utf8));
    ev.utf8_len = CLAMP(n, 0, (int)sizeof(ev.utf8) - 1);

    const xkb_keysym_t *syms;
    xkb_layout_index_t layout = xkb_state_key_get_layout(b->xkb_state, code);
    if (layout != XKB_LAYOUT_INVALID &&
        xkb_keymap_key_get_syms_by_level(b->keymap, code, layout, 0, &syms) > 0)
        ev.unshifted = syms[0];
    if (xkb_keymap_key_get_syms_by_level(b->keymap, code, 0, 0, &syms) > 0)
        ev.base = syms[0];

    b->listener->key(b->data, &ev);
}

static void
repeat_stop(struct backend *b) {
    b->repeat_key = 0;
    loop_timer_set(b->repeat_timer, 0, 0);
}

static bool
repeat_timer_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct backend *b = data;
    uint64_t count = loop_timer_ack(fd);
    if (b->repeat_key == 0 || b->xkb_state == NULL)
        return true;
    for (uint64_t i = 0; i < MIN(count, 64u); i++)
        emit_key(b, b->repeat_key, KEY_REPEAT);
    return true;
}

static void
keyboard_keymap(void *data, struct wl_keyboard *kbd, uint32_t format,
                int32_t fd, uint32_t size) {
    struct backend *b = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        LOG_ERRNO("failed to map keymap");
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
        b->xkb_ctx, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap == NULL) {
        LOG_ERR("failed to compile keymap");
        return;
    }

    xkb_state_unref(b->xkb_state);
    xkb_keymap_unref(b->keymap);
    b->keymap = keymap;
    b->xkb_state = xkb_state_new(keymap);
}

static void
keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys) {
    struct backend *b = data;
    b->listener->focus(b->data, true);
}

static void
keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
               struct wl_surface *surface) {
    struct backend *b = data;
    repeat_stop(b);
    b->listener->focus(b->data, false);
}

static void
keyboard_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time,
             uint32_t key, uint32_t state) {
    struct backend *b = data;
    b->last_serial = serial;
    if (b->xkb_state == NULL)
        return;

    xkb_keycode_t code = key + 8;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (code == b->repeat_key)
            repeat_stop(b);
        emit_key(b, code, KEY_RELEASE);
        return;
    }

    emit_key(b, code, KEY_PRESS);

    if (b->repeat_rate > 0 && xkb_keymap_key_repeats(b->keymap, code)) {
        b->repeat_key = code;
        loop_timer_set(b->repeat_timer, (uint64_t)b->repeat_delay * 1000000,
                       1000000000ull / (uint64_t)b->repeat_rate);
    }
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                   uint32_t depressed, uint32_t latched, uint32_t locked,
                   uint32_t group) {
    struct backend *b = data;
    if (b->xkb_state != NULL)
        xkb_state_update_mask(b->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay) {
    struct backend *b = data;
    b->repeat_rate = rate;
    b->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ---- pointer ---- */

/* Wheel notches per continuous-axis scroll unit, for compositors that only
 * send wl_pointer.axis (touchpads, or pre-v5 wheel reporting): an
 * approximation, since no discrete "click" size is given for that event. */
#define AXIS_LINE_UNITS (10 * 256) /* wl_fixed_t is 24.8 fixed point */

static void
apply_cursor_shape(struct backend *b) {
    if (b->cursor_shape_device == NULL)
        return;
    wp_cursor_shape_device_v1_set_shape(
        b->cursor_shape_device, b->enter_serial,
        b->shape == CURSOR_SHAPE_POINTER ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                                         : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
}

static void
pointer_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    struct backend *b = data;
    b->ptr_x = (int)lround(wl_fixed_to_double(x) * b->scale);
    b->ptr_y = (int)lround(wl_fixed_to_double(y) * b->scale);
    b->enter_serial = serial;
    apply_cursor_shape(b);
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *su) {}

static void
pointer_motion(void *data, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    struct backend *b = data;
    b->ptr_x = (int)lround(wl_fixed_to_double(x) * b->scale);
    b->ptr_y = (int)lround(wl_fixed_to_double(y) * b->scale);
    b->listener->pointer_motion(b->data, b->ptr_x, b->ptr_y, mods_from_xkb(b));
}

static void
pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
               uint32_t button, uint32_t state) {
    struct backend *b = data;
    b->last_serial = serial;
    int xt_button;
    switch (button) {
    case BTN_LEFT:
        xt_button = 1;
        break;
    case BTN_MIDDLE:
        xt_button = 2;
        break;
    case BTN_RIGHT:
        xt_button = 3;
        break;
    default:
        return; /* side/extra buttons: out of scope */
    }
    b->listener->pointer_button(b->data, xt_button, state == WL_POINTER_BUTTON_STATE_PRESSED,
                                b->ptr_x, b->ptr_y, mods_from_xkb(b));
}

/* axis/axis_discrete/axis_value120 only accumulate; reconciled in frame()
 * once it's known which of the three a compositor is actually sending. */
static void
pointer_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    struct backend *b = data;
    if (axis <= WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        b->axis_cont[axis] += value;
        b->axis_touched[axis] = true;
    }
}

static void
pointer_axis_source(void *d, struct wl_pointer *p, uint32_t source) {}

static void
pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t t, uint32_t axis) {
    struct backend *b = data;
    if (axis <= WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        b->axis_cont_remainder[axis] = 0; /* don't carry a stale remainder into the next scroll */
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    struct backend *b = data;
    if (axis <= WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        b->axis_discrete[axis] += discrete;
        b->axis_touched[axis] = true;
    }
}

static void
pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value120) {
    struct backend *b = data;
    if (axis <= WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        b->axis_v120[axis] += value120;
        b->axis_touched[axis] = true;
    }
}

static void
pointer_frame(void *data, struct wl_pointer *p) {
    struct backend *b = data;
    int steps[2] = {0, 0}; /* [0] vertical (+down), [1] horizontal (+right) */

    for (int a = 0; a < 2; a++) {
        if (!b->axis_touched[a])
            continue;
        if (b->axis_v120[a] != 0)
            steps[a] = b->axis_v120[a] / 120;
        else if (b->axis_discrete[a] != 0)
            steps[a] = b->axis_discrete[a];
        else {
            wl_fixed_t accum = b->axis_cont_remainder[a] + b->axis_cont[a];
            steps[a] = accum / AXIS_LINE_UNITS;
            b->axis_cont_remainder[a] = accum - steps[a] * AXIS_LINE_UNITS;
        }
        b->axis_v120[a] = b->axis_discrete[a] = 0;
        b->axis_cont[a] = 0;
        b->axis_touched[a] = false;
    }

    if (steps[0] != 0 || steps[1] != 0)
        b->listener->scroll(b->data, steps[1], steps[0], b->ptr_x, b->ptr_y, mods_from_xkb(b));
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
};

/* ---- clipboard ---- */

#define CLIPBOARD_MIME "text/plain;charset=utf-8"

static void
paste_recv_finish(struct backend *b) {
    if (b->paste_recv.active)
        loop_del(b->loop, b->paste_recv.fd); /* also closes the fd */
    b->paste_recv.active = false;
    if (b->paste_recv.len > 0)
        b->listener->paste(b->data, b->paste_recv.data, b->paste_recv.len);
    free(b->paste_recv.data);
    b->paste_recv.data = NULL;
    b->paste_recv.len = b->paste_recv.cap = 0;
}

static bool
paste_recv_fd_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct backend *b = data;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (b->paste_recv.len + (size_t)n > b->paste_recv.cap) {
                b->paste_recv.cap = MAX(b->paste_recv.cap * 2, b->paste_recv.len + (size_t)n);
                b->paste_recv.data = xrealloc(b->paste_recv.data, b->paste_recv.cap);
            }
            memcpy(b->paste_recv.data + b->paste_recv.len, buf, (size_t)n);
            b->paste_recv.len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return true; /* more later */
        break;           /* EOF, or a real error: either way the transfer is over */
    }
    paste_recv_finish(b);
    return true;
}

static void
wl_request_paste(struct backend *b, enum selection_target target) {
    if (b->paste_recv.active)
        return; /* one paste in flight at a time */

    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        LOG_ERRNO("pipe2 failed");
        return;
    }

    bool requested = false;
    if (target == SELECTION_CLIPBOARD && b->clipboard_offer != NULL) {
        wl_data_offer_receive(b->clipboard_offer, CLIPBOARD_MIME, fds[1]);
        requested = true;
    } else if (target == SELECTION_PRIMARY && b->primary_offer != NULL) {
        zwp_primary_selection_offer_v1_receive(b->primary_offer, CLIPBOARD_MIME, fds[1]);
        requested = true;
    }
    close(fds[1]);

    if (!requested) {
        close(fds[0]);
        return; /* nothing to paste; listener->paste() simply never fires */
    }

    b->paste_recv.active = true;
    b->paste_recv.target = target;
    b->paste_recv.fd = fds[0];
    b->paste_recv.len = 0;
    loop_add(b->loop, fds[0], EPOLLIN, paste_recv_fd_cb, b);
}

static void data_source_target(void *d, struct wl_data_source *s, const char *m) {}

static void
data_source_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd) {
    struct backend *b = data;
    const char *text = b->sel_owned[SELECTION_CLIPBOARD].text;
    size_t len = b->sel_owned[SELECTION_CLIPBOARD].len;
    for (size_t sent = 0; sent < len;) {
        ssize_t n = write(fd, text + sent, len - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            break;
        sent += (size_t)n;
    }
    close(fd);
}

static void
data_source_cancelled(void *data, struct wl_data_source *source) {
    struct backend *b = data;
    if (b->clipboard_source == source)
        b->clipboard_source = NULL;
    free(b->sel_owned[SELECTION_CLIPBOARD].text);
    b->sel_owned[SELECTION_CLIPBOARD].text = NULL;
    b->sel_owned[SELECTION_CLIPBOARD].len = 0;
    wl_data_source_destroy(source);
}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
};

static void
primary_source_send(void *data, struct zwp_primary_selection_source_v1 *source,
                    const char *mime_type, int32_t fd) {
    struct backend *b = data;
    const char *text = b->sel_owned[SELECTION_PRIMARY].text;
    size_t len = b->sel_owned[SELECTION_PRIMARY].len;
    for (size_t sent = 0; sent < len;) {
        ssize_t n = write(fd, text + sent, len - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            break;
        sent += (size_t)n;
    }
    close(fd);
}

static void
primary_source_cancelled(void *data, struct zwp_primary_selection_source_v1 *source) {
    struct backend *b = data;
    if (b->primary_source == source)
        b->primary_source = NULL;
    free(b->sel_owned[SELECTION_PRIMARY].text);
    b->sel_owned[SELECTION_PRIMARY].text = NULL;
    b->sel_owned[SELECTION_PRIMARY].len = 0;
    zwp_primary_selection_source_v1_destroy(source);
    b->listener->selection_lost(b->data, SELECTION_PRIMARY);
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send = primary_source_send,
    .cancelled = primary_source_cancelled,
};

static void data_offer_offer(void *d, struct wl_data_offer *o, const char *m) {}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = data_offer_offer,
};

static void primary_offer_offer(void *d, struct zwp_primary_selection_offer_v1 *o,
                                const char *m) {
}

static const struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = primary_offer_offer,
};

static void
data_device_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    wl_data_offer_add_listener(offer, &data_offer_listener, data);
}

static void data_device_enter(void *d, struct wl_data_device *dev, uint32_t serial,
                              struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                              struct wl_data_offer *offer) {
}
static void data_device_leave(void *d, struct wl_data_device *dev) {}
static void data_device_motion(void *d, struct wl_data_device *dev, uint32_t time,
                               wl_fixed_t x, wl_fixed_t y) {
}
static void data_device_drop(void *d, struct wl_data_device *dev) {}

static void
data_device_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    struct backend *b = data;
    if (b->clipboard_offer != NULL)
        wl_data_offer_destroy(b->clipboard_offer);
    b->clipboard_offer = offer;
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void
primary_device_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
                          struct zwp_primary_selection_offer_v1 *offer) {
    zwp_primary_selection_offer_v1_add_listener(offer, &primary_selection_offer_listener, data);
}

static void
primary_device_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
                         struct zwp_primary_selection_offer_v1 *offer) {
    struct backend *b = data;
    if (b->primary_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(b->primary_offer);
    b->primary_offer = offer;
}

static const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = primary_device_data_offer,
    .selection = primary_device_selection,
};

static void
setup_data_devices(struct backend *b) {
    if (b->seat == NULL)
        return;
    if (b->data_device_manager != NULL && b->data_device == NULL) {
        b->data_device = wl_data_device_manager_get_data_device(b->data_device_manager, b->seat);
        wl_data_device_add_listener(b->data_device, &data_device_listener, b);
    }
    if (b->primary_selection_manager != NULL && b->primary_selection_device == NULL) {
        b->primary_selection_device = zwp_primary_selection_device_manager_v1_get_device(
            b->primary_selection_manager, b->seat);
        zwp_primary_selection_device_v1_add_listener(b->primary_selection_device,
                                                     &primary_selection_device_listener, b);
    }
}

static void
wl_set_selection(struct backend *b, enum selection_target target, const char *text, size_t len) {
    free(b->sel_owned[target].text);
    b->sel_owned[target].text = xmalloc(len > 0 ? len : 1);
    memcpy(b->sel_owned[target].text, text, len);
    b->sel_owned[target].len = len;

    if (target == SELECTION_CLIPBOARD) {
        if (b->data_device == NULL)
            return;
        struct wl_data_source *source =
            wl_data_device_manager_create_data_source(b->data_device_manager);
        wl_data_source_add_listener(source, &data_source_listener, b);
        wl_data_source_offer(source, CLIPBOARD_MIME);
        wl_data_device_set_selection(b->data_device, source, b->last_serial);
        b->clipboard_source = source;
    } else {
        if (b->primary_selection_device == NULL)
            return;
        struct zwp_primary_selection_source_v1 *source =
            zwp_primary_selection_device_manager_v1_create_source(b->primary_selection_manager);
        zwp_primary_selection_source_v1_add_listener(source, &primary_selection_source_listener, b);
        zwp_primary_selection_source_v1_offer(source, CLIPBOARD_MIME);
        zwp_primary_selection_device_v1_set_selection(b->primary_selection_device, source,
                                                      b->last_serial);
        b->primary_source = source;
    }
}

/* ---- seat and registry ---- */

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct backend *b = data;

    bool has_kbd = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_kbd && b->keyboard == NULL) {
        b->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(b->keyboard, &keyboard_listener, b);
    } else if (!has_kbd && b->keyboard != NULL) {
        repeat_stop(b);
        wl_keyboard_release(b->keyboard);
        b->keyboard = NULL;
    }

    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    if (has_ptr && b->pointer == NULL) {
        b->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(b->pointer, &pointer_listener, b);
        if (b->cursor_shape_manager != NULL)
            b->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
                b->cursor_shape_manager, b->pointer);
    } else if (!has_ptr && b->pointer != NULL) {
        if (b->cursor_shape_device != NULL)
            wp_cursor_shape_device_v1_destroy(b->cursor_shape_device);
        b->cursor_shape_device = NULL;
        wl_pointer_release(b->pointer);
        b->pointer = NULL;
    }
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version) {
    struct backend *b = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (version >= 4)
            b->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        b->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && b->seat == NULL) {
        /* first seat only; v5 for release requests, v8 for wl_pointer.axis_value120 */
        if (version >= 5) {
            b->seat = wl_registry_bind(registry, name, &wl_seat_interface, MIN(version, 8u));
            wl_seat_add_listener(b->seat, &seat_listener, b);
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        b->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                      MIN(version, 5u));
        xdg_wm_base_add_listener(b->wm_base, &wm_base_listener, b);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        b->decoration_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        b->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        b->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        b->fractional_scale_manager = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        /* v1 only: no drag-and-drop support needed, so the v3 source/offer
         * action-negotiation events (out of scope) never get sent to us. */
        b->data_device_manager = wl_registry_bind(
            registry, name, &wl_data_device_manager_interface, 1);
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        b->primary_selection_manager = wl_registry_bind(
            registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- event loop integration ---- */

static bool
display_fd_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct backend *b = data;
    if ((events & (EPOLLHUP | EPOLLERR)) || wl_display_dispatch(b->display) < 0) {
        LOG_ERR("lost connection to the Wayland compositor");
        b->listener->close(b->data);
        return false;
    }
    return true;
}

static void
pre_poll_dispatch(struct loop *loop, void *data) {
    struct backend *b = data;
    wl_display_dispatch_pending(b->display);
}

static void
pre_poll_flush(struct loop *loop, void *data) {
    struct backend *b = data;
    if (wl_display_flush(b->display) < 0 && errno != EAGAIN)
        LOG_ERRNO("failed to flush Wayland requests");
}

/* ---- setup ---- */

static void wl_destroy(struct backend *b);

static struct backend *
wl_create(struct loop *loop, const struct backend_listener *listener, void *data,
          int width, int height, const char *app_id, const char *title) {
    struct backend *b = xcalloc(1, sizeof(*b));
    b->loop = loop;
    b->listener = listener;
    b->data = data;
    b->width = width;
    b->height = height;
    b->repeat_rate = 25;
    b->repeat_delay = 600;
    b->repeat_timer = -1;
    b->scale = 1.0;

    b->display = wl_display_connect(NULL);
    if (b->display == NULL) {
        LOG_ERR("failed to connect to the Wayland compositor");
        free(b);
        return NULL;
    }

    b->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    b->registry = wl_display_get_registry(b->display);
    wl_registry_add_listener(b->registry, &registry_listener, b);
    wl_display_roundtrip(b->display); /* globals */

    if (b->compositor == NULL || b->shm == NULL || b->wm_base == NULL) {
        LOG_ERR("compositor lacks wl_compositor v4, wl_shm or xdg_wm_base");
        wl_destroy(b);
        return NULL;
    }
    if (b->seat == NULL)
        LOG_WARN("no wl_seat v5+; keyboard input unavailable");

    wl_display_roundtrip(b->display); /* seat capabilities, keymap */
    setup_data_devices(b);

    b->surface = wl_compositor_create_surface(b->compositor);
    if (b->viewporter != NULL && b->fractional_scale_manager != NULL) {
        b->viewport = wp_viewporter_get_viewport(b->viewporter, b->surface);
        b->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            b->fractional_scale_manager, b->surface);
        wp_fractional_scale_v1_add_listener(b->fractional_scale, &fractional_scale_listener, b);
    } else
        LOG_WARN("no fractional-scale/viewporter; rendering at scale 1.0");
    b->xdg_surface = xdg_wm_base_get_xdg_surface(b->wm_base, b->surface);
    xdg_surface_add_listener(b->xdg_surface, &xdg_surface_listener, b);
    b->xdg_toplevel = xdg_surface_get_toplevel(b->xdg_surface);
    xdg_toplevel_add_listener(b->xdg_toplevel, &toplevel_listener, b);
    xdg_toplevel_set_app_id(b->xdg_toplevel, app_id);
    xdg_toplevel_set_title(b->xdg_toplevel, title);

    if (b->decoration_manager != NULL) {
        b->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            b->decoration_manager, b->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(
            b->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else
        LOG_WARN("no xdg-decoration; window will have no decorations");

    /* Initial commit without a buffer; the compositor answers with configure */
    wl_surface_commit(b->surface);

    b->repeat_timer = loop_timer_add(loop, repeat_timer_cb, b);
    if (b->repeat_timer < 0 ||
        !loop_add(loop, wl_display_get_fd(b->display), EPOLLIN, display_fd_cb, b) ||
        !loop_hook_add(loop, pre_poll_dispatch, b, LOOP_HOOK_HIGH) ||
        !loop_hook_add(loop, pre_poll_flush, b, LOOP_HOOK_LOW)) {
        wl_destroy(b);
        return NULL;
    }

    LOG_INFO("Wayland backend");
    return b;
}

static void
wl_destroy(struct backend *b) {
    if (b == NULL)
        return;

    loop_hook_del(b->loop, pre_poll_dispatch, LOOP_HOOK_HIGH);
    loop_hook_del(b->loop, pre_poll_flush, LOOP_HOOK_LOW);
    if (b->repeat_timer >= 0) {
        loop_del(b->loop, b->repeat_timer);
        loop_del_no_close(b->loop, wl_display_get_fd(b->display));
    }

    for (int i = 0; i < MAX_BUFFERS; i++)
        buffer_destroy(&b->bufs[i]);

    if (b->frame_cb != NULL)
        wl_callback_destroy(b->frame_cb);
    if (b->decoration != NULL)
        zxdg_toplevel_decoration_v1_destroy(b->decoration);
    if (b->xdg_toplevel != NULL)
        xdg_toplevel_destroy(b->xdg_toplevel);
    if (b->xdg_surface != NULL)
        xdg_surface_destroy(b->xdg_surface);
    if (b->fractional_scale != NULL)
        wp_fractional_scale_v1_destroy(b->fractional_scale);
    if (b->viewport != NULL)
        wp_viewport_destroy(b->viewport);
    if (b->surface != NULL)
        wl_surface_destroy(b->surface);

    if (b->fractional_scale_manager != NULL)
        wp_fractional_scale_manager_v1_destroy(b->fractional_scale_manager);
    if (b->viewporter != NULL)
        wp_viewporter_destroy(b->viewporter);
    if (b->cursor_shape_device != NULL)
        wp_cursor_shape_device_v1_destroy(b->cursor_shape_device);
    if (b->pointer != NULL)
        wl_pointer_release(b->pointer);
    if (b->keyboard != NULL)
        wl_keyboard_release(b->keyboard);
    if (b->seat != NULL)
        wl_seat_release(b->seat);

    if (b->paste_recv.active)
        loop_del(b->loop, b->paste_recv.fd);
    free(b->paste_recv.data);
    free(b->sel_owned[SELECTION_PRIMARY].text);
    free(b->sel_owned[SELECTION_CLIPBOARD].text);
    if (b->clipboard_source != NULL)
        wl_data_source_destroy(b->clipboard_source);
    if (b->primary_source != NULL)
        zwp_primary_selection_source_v1_destroy(b->primary_source);
    if (b->clipboard_offer != NULL)
        wl_data_offer_destroy(b->clipboard_offer);
    if (b->primary_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(b->primary_offer);
    /* wl_data_device has no destructor at v1, the version bound above */
    if (b->primary_selection_device != NULL)
        zwp_primary_selection_device_v1_destroy(b->primary_selection_device);
    if (b->primary_selection_manager != NULL)
        zwp_primary_selection_device_manager_v1_destroy(b->primary_selection_manager);

    if (b->cursor_shape_manager != NULL)
        wp_cursor_shape_manager_v1_destroy(b->cursor_shape_manager);
    if (b->decoration_manager != NULL)
        zxdg_decoration_manager_v1_destroy(b->decoration_manager);
    if (b->wm_base != NULL)
        xdg_wm_base_destroy(b->wm_base);
    if (b->shm != NULL)
        wl_shm_destroy(b->shm);
    if (b->compositor != NULL)
        wl_compositor_destroy(b->compositor);
    if (b->registry != NULL)
        wl_registry_destroy(b->registry);

    xkb_state_unref(b->xkb_state);
    xkb_keymap_unref(b->keymap);
    xkb_context_unref(b->xkb_ctx);

    wl_display_flush(b->display);
    wl_display_disconnect(b->display);
    free(b);
}

static void
wl_set_title(struct backend *b, const char *title) {
    xdg_toplevel_set_title(b->xdg_toplevel, title);
}

static void
wl_set_cursor_shape(struct backend *b, enum cursor_shape shape) {
    if (b->shape == shape)
        return;
    b->shape = shape;
    apply_cursor_shape(b);
}

const struct backend_ops backend_wayland = {
    .name = "wayland",
    .create = wl_create,
    .destroy = wl_destroy,
    .get_buffer = wl_get_buffer,
    .commit = wl_commit,
    .set_title = wl_set_title,
    .bell = NULL,
    .set_cursor_shape = wl_set_cursor_shape,
    .set_selection = wl_set_selection,
    .request_paste = wl_request_paste,
};
