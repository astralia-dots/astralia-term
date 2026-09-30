/* X11 backend: xcb + MIT-SHM, keyboard via xkbcommon-x11. */
#include "backend/backend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#define LOG_MODULE "x11"
#include "core/loop.h"
#include "core/util.h"

#define FRAME_NS (1000000000ull / 60)

struct x11_buffer {
    struct buffer base;
    xcb_shm_seg_t seg; /* 0 when not using MIT-SHM */
    size_t size;
};

struct backend {
    struct loop *loop;
    const struct backend_listener *listener;
    void *data;

    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_window_t win;
    xcb_gcontext_t gc;
    xcb_cursor_context_t *cursor_ctx;
    xcb_cursor_t cursors[2]; /* indexed by enum cursor_shape; XCB_CURSOR_NONE if unavailable */
    uint8_t depth;
    xcb_visualid_t visual;
    xcb_colormap_t colormap; /* only for the ARGB visual */
    bool argb;               /* 32-bit visual: translucency possible */
    uint32_t max_request_bytes;

    xcb_atom_t wm_protocols, wm_delete_window, net_wm_name, utf8_string, net_wm_pid;
    xcb_atom_t clipboard, targets_atom, incr_atom;

    /* Text this window owns per target; the property atom used to hand it
     * to a requestor is the same as the selection atom (PRIMARY/CLIPBOARD). */
    struct {
        char *text;
        size_t len;
    } sel_owned[2];

    /* At most one outgoing INCR transfer at a time (see plan's open items). */
    struct {
        bool active;
        xcb_window_t requestor;
        xcb_atom_t property;
        xcb_atom_t target_atom;
        enum selection_target sel;
        size_t sent;
    } incr_send;

    struct {
        bool active;
        bool incr;
        enum selection_target target;
        char *data;
        size_t len, cap;
    } paste_recv;

    bool have_shm;
    uint8_t shm_event_base;

    /* Looked ahead while collapsing an autorepeat release/press pair;
     * drain_events() dispatches it before polling for a new one. */
    xcb_generic_event_t *pending_event;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int32_t kbd_device_id;
    uint8_t xkb_event_base;

    int width, height;
    struct x11_buffer bufs[2];
    struct x11_buffer *last_committed;

    int frame_timer;
    bool frame_pending;
};

static xcb_atom_t
intern_atom(xcb_connection_t *conn, const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    xcb_atom_t atom = reply != NULL ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

/* ---- keyboard ---- */

static bool
xkb_reload_keymap(struct backend *b) {
    struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(
        b->xkb_ctx, b->conn, b->kbd_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == NULL) {
        LOG_ERR("failed to load keymap");
        return false;
    }
    struct xkb_state *state = xkb_x11_state_new_from_device(keymap, b->conn, b->kbd_device_id);
    if (state == NULL) {
        LOG_ERR("failed to create keyboard state");
        xkb_keymap_unref(keymap);
        return false;
    }
    xkb_state_unref(b->xkb_state);
    xkb_keymap_unref(b->keymap);
    b->keymap = keymap;
    b->xkb_state = state;
    return true;
}

static bool
xkb_setup(struct backend *b) {
    if (!xkb_x11_setup_xkb_extension(b->conn, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                     XKB_X11_MIN_MINOR_XKB_VERSION,
                                     XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                     NULL, NULL, &b->xkb_event_base, NULL)) {
        LOG_ERR("XKB extension not available");
        return false;
    }

    b->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    b->kbd_device_id = xkb_x11_get_core_keyboard_device_id(b->conn);
    if (b->xkb_ctx == NULL || b->kbd_device_id < 0 || !xkb_reload_keymap(b))
        return false;

    /* Follow keymap and modifier-state changes (from xkbcommon's examples) */
    enum {
        events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
                 XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
                 XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
        nkn_details = XCB_XKB_NKN_DETAIL_KEYCODES,
        map_parts = XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS |
                    XCB_XKB_MAP_PART_MODIFIER_MAP | XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
                    XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS |
                    XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP,
        state_details = XCB_XKB_STATE_PART_MODIFIER_BASE | XCB_XKB_STATE_PART_MODIFIER_LATCH |
                        XCB_XKB_STATE_PART_MODIFIER_LOCK | XCB_XKB_STATE_PART_GROUP_BASE |
                        XCB_XKB_STATE_PART_GROUP_LATCH | XCB_XKB_STATE_PART_GROUP_LOCK,
    };
    static const xcb_xkb_select_events_details_t details = {
        .affectNewKeyboard = nkn_details,
        .newKeyboardDetails = nkn_details,
        .affectState = state_details,
        .stateDetails = state_details,
    };
    xcb_xkb_select_events_aux(b->conn, b->kbd_device_id, events, 0, 0,
                              map_parts, map_parts, &details);
    return true;
}

static void
handle_xkb_event(struct backend *b, xcb_generic_event_t *ev) {
    union {
        struct {
            uint8_t response_type;
            uint8_t xkb_type;
            uint16_t sequence;
            xcb_timestamp_t time;
            uint8_t device_id;
        } any;
        xcb_xkb_new_keyboard_notify_event_t new_keyboard;
        xcb_xkb_map_notify_event_t map;
        xcb_xkb_state_notify_event_t state;
    } *e = (void *)ev;

    if (e->any.device_id != b->kbd_device_id)
        return;

    switch (e->any.xkb_type) {
    case XCB_XKB_NEW_KEYBOARD_NOTIFY:
        if (e->new_keyboard.changed & XCB_XKB_NKN_DETAIL_KEYCODES)
            xkb_reload_keymap(b);
        break;
    case XCB_XKB_MAP_NOTIFY:
        xkb_reload_keymap(b);
        break;
    case XCB_XKB_STATE_NOTIFY:
        xkb_state_update_mask(b->xkb_state, e->state.baseMods, e->state.latchedMods,
                              e->state.lockedMods, e->state.baseGroup,
                              e->state.latchedGroup, e->state.lockedGroup);
        break;
    }
}

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
    for (size_t i = 0; i < ARRAY_LEN(mods); i++) {
        if (xkb_state_mod_name_is_active(b->xkb_state, mods[i].name,
                                         XKB_STATE_MODS_EFFECTIVE) > 0)
            result |= mods[i].mod;
    }
    return result;
}

static void
emit_key(struct backend *b, xkb_keycode_t code, enum key_action action) {
    struct key_event key = {.sym = xkb_state_key_get_one_sym(b->xkb_state, code),
                            .action = action,
                            .mods = mods_from_xkb(b)};

    int n = xkb_state_key_get_utf8(b->xkb_state, code, key.utf8, sizeof(key.utf8));
    key.utf8_len = CLAMP(n, 0, (int)sizeof(key.utf8) - 1);

    const xkb_keysym_t *syms;
    xkb_layout_index_t layout = xkb_state_key_get_layout(b->xkb_state, code);
    if (layout != XKB_LAYOUT_INVALID &&
        xkb_keymap_key_get_syms_by_level(b->keymap, code, layout, 0, &syms) > 0)
        key.unshifted = syms[0];
    if (xkb_keymap_key_get_syms_by_level(b->keymap, code, 0, 0, &syms) > 0)
        key.base = syms[0];

    b->listener->key(b->data, &key);
}

static void
handle_key_press(struct backend *b, xcb_key_press_event_t *ev) {
    emit_key(b, ev->detail, KEY_PRESS);
}

/* X11 (without the detectable-autorepeat extension) signals a held key by
 * sending a release immediately followed by a press for the same key at the
 * same timestamp; collapse that pair into one KEY_REPEAT. */
static void
handle_key_release(struct backend *b, xcb_key_release_event_t *ev) {
    xcb_generic_event_t *next = xcb_poll_for_queued_event(b->conn);
    if (next != NULL && (next->response_type & 0x7f) == XCB_KEY_PRESS) {
        xcb_key_press_event_t *press = (void *)next;
        if (press->detail == ev->detail && press->time == ev->time) {
            emit_key(b, press->detail, KEY_REPEAT);
            free(next);
            return;
        }
    }
    emit_key(b, ev->detail, KEY_RELEASE);
    b->pending_event = next;
}

/* ---- pointer ---- */

/* Wheel is reported as button press/release pairs (detail 4-7); only the
 * press is meaningful, so the release is dropped in dispatch_event(). */
static void
handle_button_press(struct backend *b, xcb_button_press_event_t *ev, bool pressed) {
    unsigned mods = mods_from_xkb(b);
    switch (ev->detail) {
    case 4:
        b->listener->scroll(b->data, 0, -1, ev->event_x, ev->event_y, mods);
        break;
    case 5:
        b->listener->scroll(b->data, 0, 1, ev->event_x, ev->event_y, mods);
        break;
    case 6:
        b->listener->scroll(b->data, -1, 0, ev->event_x, ev->event_y, mods);
        break;
    case 7:
        b->listener->scroll(b->data, 1, 0, ev->event_x, ev->event_y, mods);
        break;
    case 1:
    case 2:
    case 3:
        b->listener->pointer_button(b->data, ev->detail, pressed, ev->event_x, ev->event_y,
                                    mods);
        break;
    default:
        break; /* side/extra buttons: out of scope */
    }
}

static void
handle_motion(struct backend *b, xcb_motion_notify_event_t *ev) {
    b->listener->pointer_motion(b->data, ev->event_x, ev->event_y, mods_from_xkb(b));
}

/* ---- buffers ---- */

static void
buffer_destroy(struct backend *b, struct x11_buffer *buf) {
    if (buf->base.pix != NULL)
        pixman_image_unref(buf->base.pix);
    if (buf->seg != 0) {
        if (!xcb_connection_has_error(b->conn))
            xcb_shm_detach(b->conn, buf->seg);
        munmap(buf->base.data, buf->size);
    } else
        free(buf->base.data);
    if (b->last_committed == buf)
        b->last_committed = NULL;
    *buf = (struct x11_buffer){0};
}

static bool
buffer_create(struct backend *b, struct x11_buffer *buf, int width, int height) {
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    void *data = NULL;
    xcb_shm_seg_t seg = 0;

    if (b->have_shm) {
        int fd = memfd_create("astralia-x11-shm", MFD_CLOEXEC);
        if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
            data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (data == MAP_FAILED)
                data = NULL;
        }
        if (data != NULL) {
            seg = xcb_generate_id(b->conn);
            xcb_shm_attach_fd(b->conn, seg, fd, 1); /* xcb closes fd */
        } else {
            LOG_ERRNO("failed to allocate shared memory; falling back to put_image");
            if (fd >= 0)
                close(fd);
            b->have_shm = false;
        }
    }
    if (data == NULL)
        data = xmalloc(size);

    *buf = (struct x11_buffer){
        .base = {
            .width = width,
            .height = height,
            .stride = stride,
            .data = data,
            .pix = pixman_image_create_bits_no_clear(
                b->argb ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8, width, height, data, stride),
        },
        .seg = seg,
        .size = size,
    };
    return buf->base.pix != NULL;
}

static struct buffer *
x11_get_buffer(struct backend *b, int width, int height) {
    if (b->bufs[0].base.width != width || b->bufs[0].base.height != height) {
        for (size_t i = 0; i < ARRAY_LEN(b->bufs); i++) {
            buffer_destroy(b, &b->bufs[i]);
            if (!buffer_create(b, &b->bufs[i], width, height))
                return NULL;
        }
    }
    for (size_t i = 0; i < ARRAY_LEN(b->bufs); i++) {
        if (!b->bufs[i].base.busy)
            return &b->bufs[i].base;
    }
    return NULL;
}

static void
put_box(struct backend *b, struct x11_buffer *buf, const pixman_box32_t *box, bool notify) {
    int x = MAX(box->x1, 0), y = MAX(box->y1, 0);
    int w = MIN(box->x2, buf->base.width) - x;
    int h = MIN(box->y2, buf->base.height) - y;
    if (w <= 0 || h <= 0)
        return;

    if (buf->seg != 0) {
        xcb_shm_put_image(b->conn, b->win, b->gc, buf->base.width, buf->base.height,
                          x, y, w, h, x, y, b->depth, XCB_IMAGE_FORMAT_Z_PIXMAP,
                          notify, buf->seg, 0);
        if (notify)
            buf->base.busy = true;
        return;
    }

    /* No MIT-SHM: send full-width stripes, split to fit the request limit */
    int stride = buf->base.stride;
    int rows_per_req = MAX(1, (int)((b->max_request_bytes - 64) / stride));
    for (int row = y; row < y + h; row += rows_per_req) {
        int n = MIN(rows_per_req, y + h - row);
        xcb_put_image(b->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, b->win, b->gc,
                      buf->base.width, n, 0, row, 0, b->depth, (uint32_t)(n * stride),
                      (const uint8_t *)buf->base.data + (size_t)row * stride);
    }
}

static void
x11_commit(struct backend *b, struct buffer *base, pixman_region32_t *damage) {
    struct x11_buffer *buf = (struct x11_buffer *)base;

    int n;
    pixman_box32_t *boxes = pixman_region32_rectangles(damage, &n);
    for (int i = 0; i < n; i++)
        put_box(b, buf, &boxes[i], i == n - 1);

    b->last_committed = buf;
    b->frame_pending = true;
    loop_timer_set(b->frame_timer, FRAME_NS, 0);
}

static void
handle_shm_completion(struct backend *b, xcb_shm_completion_event_t *ev) {
    for (size_t i = 0; i < ARRAY_LEN(b->bufs); i++) {
        if (b->bufs[i].seg == ev->shmseg)
            b->bufs[i].base.busy = false;
    }
}

static void
handle_expose(struct backend *b, xcb_expose_event_t *ev) {
    struct x11_buffer *buf = b->last_committed;
    if (buf == NULL)
        return;
    pixman_box32_t box = {ev->x, ev->y, ev->x + ev->width, ev->y + ev->height};
    put_box(b, buf, &box, false);
}

/* ---- clipboard ---- */

static xcb_atom_t
selection_atom(struct backend *b, enum selection_target target) {
    return target == SELECTION_PRIMARY ? XCB_ATOM_PRIMARY : b->clipboard;
}

static void
x11_set_selection(struct backend *b, enum selection_target target, const char *text, size_t len) {
    free(b->sel_owned[target].text);
    b->sel_owned[target].text = xmalloc(len > 0 ? len : 1);
    memcpy(b->sel_owned[target].text, text, len);
    b->sel_owned[target].len = len;
    xcb_set_selection_owner(b->conn, b->win, selection_atom(b, target), XCB_CURRENT_TIME);
}

static void
x11_request_paste(struct backend *b, enum selection_target target) {
    if (b->paste_recv.active)
        return; /* one paste in flight at a time */
    b->paste_recv.active = true;
    b->paste_recv.incr = false;
    b->paste_recv.target = target;
    b->paste_recv.len = 0;
    xcb_atom_t sel = selection_atom(b, target);
    xcb_convert_selection(b->conn, b->win, sel, b->utf8_string, sel, XCB_CURRENT_TIME);
}

static void
handle_selection_clear(struct backend *b, xcb_selection_clear_event_t *ev) {
    enum selection_target target =
        ev->selection == b->clipboard ? SELECTION_CLIPBOARD : SELECTION_PRIMARY;
    free(b->sel_owned[target].text);
    b->sel_owned[target].text = NULL;
    b->sel_owned[target].len = 0;
    if (target == SELECTION_PRIMARY)
        b->listener->selection_lost(b->data, SELECTION_PRIMARY);
}

static void
send_selection_notify(struct backend *b, xcb_window_t requestor, xcb_atom_t selection,
                      xcb_atom_t target, xcb_atom_t property, xcb_timestamp_t time) {
    xcb_selection_notify_event_t ev = {
        .response_type = XCB_SELECTION_NOTIFY,
        .time = time,
        .requestor = requestor,
        .selection = selection,
        .target = target,
        .property = property,
    };
    xcb_send_event(b->conn, false, requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void
handle_selection_request(struct backend *b, xcb_selection_request_event_t *ev) {
    enum selection_target target =
        ev->selection == b->clipboard ? SELECTION_CLIPBOARD : SELECTION_PRIMARY;
    xcb_atom_t property = ev->property != XCB_ATOM_NONE ? ev->property : ev->target;

    if (ev->target == b->targets_atom) {
        xcb_atom_t targets[] = {b->targets_atom, b->utf8_string, XCB_ATOM_STRING};
        xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, ev->requestor, property,
                            XCB_ATOM_ATOM, 32, ARRAY_LEN(targets), targets);
        send_selection_notify(b, ev->requestor, ev->selection, ev->target, property, ev->time);
        return;
    }

    if (ev->target != b->utf8_string && ev->target != XCB_ATOM_STRING) {
        send_selection_notify(b, ev->requestor, ev->selection, ev->target, XCB_ATOM_NONE, ev->time);
        return;
    }

    const char *text = b->sel_owned[target].text;
    size_t len = b->sel_owned[target].len;
    if (text == NULL) {
        send_selection_notify(b, ev->requestor, ev->selection, ev->target, XCB_ATOM_NONE, ev->time);
        return;
    }

    size_t chunk = b->max_request_bytes > 64 ? b->max_request_bytes - 64 : 4096;
    if (len <= chunk) {
        xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, ev->requestor, property,
                            ev->target, 8, (uint32_t)len, text);
        send_selection_notify(b, ev->requestor, ev->selection, ev->target, property, ev->time);
        return;
    }

    if (b->incr_send.active) {
        send_selection_notify(b, ev->requestor, ev->selection, ev->target, XCB_ATOM_NONE, ev->time);
        return;
    }

    uint32_t len32 = (uint32_t)len;
    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, ev->requestor, property,
                        b->incr_atom, 32, 1, &len32);
    b->incr_send.active = true;
    b->incr_send.requestor = ev->requestor;
    b->incr_send.property = property;
    b->incr_send.target_atom = ev->target;
    b->incr_send.sel = target;
    b->incr_send.sent = 0;
    send_selection_notify(b, ev->requestor, ev->selection, ev->target, property, ev->time);
}

static void
incr_send_chunk(struct backend *b) {
    const char *text = b->sel_owned[b->incr_send.sel].text;
    size_t len = b->sel_owned[b->incr_send.sel].len;
    size_t chunk = b->max_request_bytes > 64 ? b->max_request_bytes - 64 : 4096;
    size_t remain = len > b->incr_send.sent ? len - b->incr_send.sent : 0;
    size_t n = MIN(remain, chunk);

    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->incr_send.requestor,
                        b->incr_send.property, b->incr_send.target_atom, 8, (uint32_t)n,
                        n > 0 && text != NULL ? text + b->incr_send.sent : NULL);
    b->incr_send.sent += n;
    if (n == 0)
        b->incr_send.active = false;
}

static void
paste_recv_append(struct backend *b, const void *data, size_t len) {
    if (b->paste_recv.len + len > b->paste_recv.cap) {
        b->paste_recv.cap = MAX(b->paste_recv.cap * 2, b->paste_recv.len + len);
        b->paste_recv.data = xrealloc(b->paste_recv.data, b->paste_recv.cap);
    }
    memcpy(b->paste_recv.data + b->paste_recv.len, data, len);
    b->paste_recv.len += len;
}

static void
paste_deliver(struct backend *b, const void *data, size_t len) {
    b->paste_recv.active = false;
    b->paste_recv.incr = false;
    if (len > 0)
        b->listener->paste(b->data, data, len);
    free(b->paste_recv.data);
    b->paste_recv.data = NULL;
    b->paste_recv.len = 0;
    b->paste_recv.cap = 0;
}

static void
handle_selection_notify(struct backend *b, xcb_selection_notify_event_t *ev) {
    if (!b->paste_recv.active || ev->requestor != b->win)
        return;

    if (ev->property == XCB_ATOM_NONE) {
        paste_deliver(b, NULL, 0);
        return;
    }

    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        b->conn,
        xcb_get_property(b->conn, false, b->win, ev->property, XCB_GET_PROPERTY_TYPE_ANY, 0,
                         UINT32_MAX / 4),
        NULL);
    if (reply == NULL) {
        paste_deliver(b, NULL, 0);
        return;
    }

    if (reply->type == b->incr_atom) {
        b->paste_recv.incr = true;
        xcb_delete_property(b->conn, b->win, ev->property);
    } else {
        paste_deliver(b, xcb_get_property_value(reply),
                      (size_t)xcb_get_property_value_length(reply));
        xcb_delete_property(b->conn, b->win, ev->property);
    }
    free(reply);
}

static void
handle_property_notify(struct backend *b, xcb_property_notify_event_t *ev) {
    if (ev->window != b->win)
        return;

    if (b->paste_recv.active && b->paste_recv.incr && ev->state == XCB_PROPERTY_NEW_VALUE &&
        ev->atom == selection_atom(b, b->paste_recv.target)) {
        xcb_get_property_reply_t *reply = xcb_get_property_reply(
            b->conn,
            xcb_get_property(b->conn, false, b->win, ev->atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
                             UINT32_MAX / 4),
            NULL);
        if (reply == NULL)
            return;
        int len = xcb_get_property_value_length(reply);
        if (len == 0)
            paste_deliver(b, b->paste_recv.data, b->paste_recv.len);
        else {
            paste_recv_append(b, xcb_get_property_value(reply), (size_t)len);
            xcb_delete_property(b->conn, b->win, ev->atom);
        }
        free(reply);
        return;
    }

    if (b->incr_send.active && ev->atom == b->incr_send.property &&
        ev->state == XCB_PROPERTY_DELETE)
        incr_send_chunk(b);
}

/* ---- event dispatch ---- */

static bool
dispatch_event(struct backend *b, xcb_generic_event_t *ev) {
    uint8_t type = ev->response_type & 0x7f;

    if (b->have_shm && type == b->shm_event_base + XCB_SHM_COMPLETION) {
        handle_shm_completion(b, (void *)ev);
        return true;
    }
    if (type == b->xkb_event_base) {
        handle_xkb_event(b, ev);
        return true;
    }

    switch (type) {
    case 0: {
        xcb_generic_error_t *err = (void *)ev;
        LOG_WARN("X error %u (request %u.%u)", err->error_code,
                 err->major_code, err->minor_code);
        break;
    }
    case XCB_EXPOSE:
        handle_expose(b, (void *)ev);
        break;
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *e = (void *)ev;
        if (e->width != b->width || e->height != b->height) {
            b->width = e->width;
            b->height = e->height;
            b->listener->configure(b->data, b->width, b->height);
        }
        break;
    }
    case XCB_KEY_PRESS:
        handle_key_press(b, (void *)ev);
        break;
    case XCB_KEY_RELEASE:
        handle_key_release(b, (void *)ev);
        break;
    case XCB_BUTTON_PRESS:
        handle_button_press(b, (void *)ev, true);
        break;
    case XCB_BUTTON_RELEASE: {
        xcb_button_release_event_t *e = (void *)ev;
        if (e->detail >= 1 && e->detail <= 3) /* wheel (4-7) release carries no new info */
            b->listener->pointer_button(b->data, e->detail, false, e->event_x, e->event_y,
                                        mods_from_xkb(b));
        break;
    }
    case XCB_MOTION_NOTIFY:
        handle_motion(b, (void *)ev);
        break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
        xcb_focus_in_event_t *e = (void *)ev;
        if (e->mode != XCB_NOTIFY_MODE_GRAB && e->mode != XCB_NOTIFY_MODE_UNGRAB)
            b->listener->focus(b->data, type == XCB_FOCUS_IN);
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *e = (void *)ev;
        if (e->type == b->wm_protocols && e->data.data32[0] == b->wm_delete_window) {
            b->listener->close(b->data);
            return false;
        }
        break;
    }
    case XCB_SELECTION_CLEAR:
        handle_selection_clear(b, (void *)ev);
        break;
    case XCB_SELECTION_REQUEST:
        handle_selection_request(b, (void *)ev);
        break;
    case XCB_SELECTION_NOTIFY:
        handle_selection_notify(b, (void *)ev);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify(b, (void *)ev);
        break;
    }
    return true;
}

static bool
drain_events(struct backend *b, bool queued_only) {
    xcb_generic_event_t *ev;
    bool ok = true;
    while ((ev = b->pending_event != NULL
                     ? b->pending_event
                 : queued_only ? xcb_poll_for_queued_event(b->conn)
                               : xcb_poll_for_event(b->conn)) != NULL) {
        b->pending_event = NULL;
        if (!dispatch_event(b, ev))
            ok = false;
        free(ev);
    }
    if (xcb_connection_has_error(b->conn)) {
        LOG_ERR("lost connection to the X server");
        b->listener->close(b->data);
        return false;
    }
    return ok;
}

static bool
fd_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct backend *b = data;
    if (events & (EPOLLHUP | EPOLLERR)) {
        LOG_ERR("X server connection closed");
        b->listener->close(b->data);
        return false;
    }
    drain_events(b, false);
    return true;
}

/* Events can sit in xcb's queue without the fd being readable (read while
 * waiting for a reply), so drain them before blocking... */
static void
pre_poll_dispatch(struct loop *loop, void *data) {
    drain_events(data, true);
}

/* ...and flush requests made by the renderer after it has run. */
static void
pre_poll_flush(struct loop *loop, void *data) {
    struct backend *b = data;
    xcb_flush(b->conn);
}

static bool
frame_timer_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct backend *b = data;
    loop_timer_ack(fd);
    b->frame_pending = false;
    b->listener->frame(b->data);
    return true;
}

/* ---- setup ---- */

static void
set_title(struct backend *b, const char *title) {
    size_t len = strlen(title);
    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->win, b->net_wm_name,
                        b->utf8_string, 8, len, title);
    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, len, title);
}

static bool
setup_shm(struct backend *b) {
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(b->conn, &xcb_shm_id);
    if (ext == NULL || !ext->present)
        return false;

    xcb_shm_query_version_reply_t *ver =
        xcb_shm_query_version_reply(b->conn, xcb_shm_query_version(b->conn), NULL);
    bool ok = ver != NULL &&
              (ver->major_version > 1 || (ver->major_version == 1 && ver->minor_version >= 2));
    free(ver);
    if (!ok)
        return false;

    b->shm_event_base = ext->first_event;
    return true;
}

/* A 32-bit TrueColor visual, for translucent windows (needs a compositor). */
static xcb_visualtype_t *
find_argb_visual(xcb_screen_t *screen) {
    xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(screen);
    for (; d.rem > 0; xcb_depth_next(&d)) {
        if (d.data->depth != 32)
            continue;
        xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data);
        for (; v.rem > 0; xcb_visualtype_next(&v)) {
            if (v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return v.data;
        }
    }
    return NULL;
}

static void x11_destroy(struct backend *b);

/* Xft.dpi / 96 from the root window's RESOURCE_MANAGER; 1.0 when unset or absurd. */
static double
read_xft_scale(struct backend *b) {
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        b->conn,
        xcb_get_property(b->conn, 0, b->screen->root, XCB_ATOM_RESOURCE_MANAGER,
                         XCB_ATOM_STRING, 0, 65536),
        NULL);
    if (reply == NULL)
        return 1.0;

    char *text = xmalloc((size_t)xcb_get_property_value_length(reply) + 1);
    memcpy(text, xcb_get_property_value(reply), (size_t)xcb_get_property_value_length(reply));
    text[xcb_get_property_value_length(reply)] = '\0';
    free(reply);

    double scale = 1.0;
    for (char *line = text; line != NULL && *line != '\0';) {
        char *nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';
        if (strncmp(line, "Xft.dpi:", 8) == 0) {
            double dpi = atof(line + 8); /* skips the leading blanks */
            if (dpi >= 48 && dpi <= 768)
                scale = dpi / 96.0;
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    free(text);
    return scale;
}

static struct backend *
x11_create(struct loop *loop, const struct backend_listener *listener, void *data,
           int width, int height, const char *app_id, const char *title) {
    struct backend *b = xcalloc(1, sizeof(*b));
    b->loop = loop;
    b->listener = listener;
    b->data = data;
    b->width = width;
    b->height = height;
    b->frame_timer = -1;

    int screen_num;
    b->conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(b->conn)) {
        LOG_ERR("failed to connect to the X server");
        xcb_disconnect(b->conn);
        free(b);
        return NULL;
    }

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(b->conn));
    for (int i = 0; i < screen_num; i++)
        xcb_screen_next(&it);
    b->screen = it.data;

    /* The window is sized in pixels, so a HiDPI setup starts proportionally larger */
    double scale = read_xft_scale(b);
    width = (int)lround(width * scale);
    height = (int)lround(height * scale);
    b->width = width;
    b->height = height;

    xcb_visualtype_t *argb_visual = find_argb_visual(b->screen);
    if (argb_visual != NULL) {
        b->argb = true;
        b->depth = 32;
        b->visual = argb_visual->visual_id;
        b->colormap = xcb_generate_id(b->conn);
        xcb_create_colormap(b->conn, XCB_COLORMAP_ALLOC_NONE, b->colormap,
                            b->screen->root, b->visual);
    } else {
        b->depth = b->screen->root_depth;
        b->visual = b->screen->root_visual;
        if (b->depth != 24 && b->depth != 32) {
            LOG_ERR("unsupported root depth %u", b->depth);
            x11_destroy(b);
            return NULL;
        }
    }
    b->max_request_bytes = xcb_get_maximum_request_length(b->conn) * 4;

    b->have_shm = setup_shm(b);
    if (!b->have_shm)
        LOG_WARN("MIT-SHM 1.2 unavailable; drawing will be slower");

    if (!xkb_setup(b)) {
        x11_destroy(b);
        return NULL;
    }

    b->wm_protocols = intern_atom(b->conn, "WM_PROTOCOLS");
    b->wm_delete_window = intern_atom(b->conn, "WM_DELETE_WINDOW");
    b->net_wm_name = intern_atom(b->conn, "_NET_WM_NAME");
    b->utf8_string = intern_atom(b->conn, "UTF8_STRING");
    b->net_wm_pid = intern_atom(b->conn, "_NET_WM_PID");
    b->clipboard = intern_atom(b->conn, "CLIPBOARD");
    b->targets_atom = intern_atom(b->conn, "TARGETS");
    b->incr_atom = intern_atom(b->conn, "INCR");

    b->win = xcb_generate_id(b->conn);
    /* Border pixel and colormap are required when the visual differs from
     * the parent's; values are ordered by their XCB_CW_* bit. */
    uint32_t values[] = {
        0,                      /* XCB_CW_BACK_PIXEL: transparent black */
        0,                      /* XCB_CW_BORDER_PIXEL */
        XCB_GRAVITY_NORTH_WEST, /* XCB_CW_BIT_GRAVITY */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_PROPERTY_CHANGE,
        b->colormap, /* XCB_CW_COLORMAP */
    };
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_BIT_GRAVITY |
                    XCB_CW_EVENT_MASK | (b->argb ? XCB_CW_COLORMAP : 0);
    xcb_create_window(b->conn, b->depth, b->win, b->screen->root,
                      0, 0, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      b->visual, mask, values);

    b->gc = xcb_generate_id(b->conn);
    uint32_t gc_values[] = {0};
    xcb_create_gc(b->conn, b->gc, b->win, XCB_GC_GRAPHICS_EXPOSURES, gc_values);

    set_title(b, title);

    size_t id_len = strlen(app_id);
    char *wm_class = xmalloc(id_len * 2 + 2);
    memcpy(wm_class, app_id, id_len + 1);
    memcpy(wm_class + id_len + 1, app_id, id_len + 1);
    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->win, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, id_len * 2 + 2, wm_class);
    free(wm_class);

    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->win, b->wm_protocols,
                        XCB_ATOM_ATOM, 32, 1, &b->wm_delete_window);
    uint32_t pid = (uint32_t)getpid();
    xcb_change_property(b->conn, XCB_PROP_MODE_REPLACE, b->win, b->net_wm_pid,
                        XCB_ATOM_CARDINAL, 32, 1, &pid);

    if (xcb_cursor_context_new(b->conn, b->screen, &b->cursor_ctx) >= 0) {
        b->cursors[CURSOR_SHAPE_TEXT] = xcb_cursor_load_cursor(b->cursor_ctx, "xterm");
        b->cursors[CURSOR_SHAPE_POINTER] = xcb_cursor_load_cursor(b->cursor_ctx, "pointer");
        if (b->cursors[CURSOR_SHAPE_POINTER] == XCB_CURSOR_NONE)
            b->cursors[CURSOR_SHAPE_POINTER] = xcb_cursor_load_cursor(b->cursor_ctx, "hand2");
        if (b->cursors[CURSOR_SHAPE_TEXT] != XCB_CURSOR_NONE)
            xcb_change_window_attributes(b->conn, b->win, XCB_CW_CURSOR,
                                         &b->cursors[CURSOR_SHAPE_TEXT]);
    }

    xcb_map_window(b->conn, b->win);
    xcb_flush(b->conn);

    b->frame_timer = loop_timer_add(loop, frame_timer_cb, b);
    if (b->frame_timer < 0 ||
        !loop_add(loop, xcb_get_file_descriptor(b->conn), EPOLLIN, fd_cb, b) ||
        !loop_hook_add(loop, pre_poll_dispatch, b, LOOP_HOOK_HIGH) ||
        !loop_hook_add(loop, pre_poll_flush, b, LOOP_HOOK_LOW)) {
        x11_destroy(b);
        return NULL;
    }

    LOG_INFO("X11 backend (MIT-SHM %s, scale %.2f)", b->have_shm ? "enabled" : "disabled", scale);
    if (scale != 1.0)
        listener->scale(data, scale);
    return b;
}

static void
x11_destroy(struct backend *b) {
    if (b == NULL)
        return;

    loop_hook_del(b->loop, pre_poll_dispatch, LOOP_HOOK_HIGH);
    loop_hook_del(b->loop, pre_poll_flush, LOOP_HOOK_LOW);
    if (b->frame_timer >= 0)
        loop_del(b->loop, b->frame_timer);
    free(b->pending_event);
    free(b->sel_owned[SELECTION_PRIMARY].text);
    free(b->sel_owned[SELECTION_CLIPBOARD].text);
    free(b->paste_recv.data);

    if (b->conn != NULL) {
        /* fd is only registered once setup got that far; a miss just logs */
        if (b->frame_timer >= 0)
            loop_del_no_close(b->loop, xcb_get_file_descriptor(b->conn));
        for (size_t i = 0; i < ARRAY_LEN(b->bufs); i++)
            buffer_destroy(b, &b->bufs[i]);
        if (b->cursor_ctx != NULL)
            xcb_cursor_context_free(b->cursor_ctx);
        if (!xcb_connection_has_error(b->conn)) {
            if (b->win != 0)
                xcb_destroy_window(b->conn, b->win);
            if (b->colormap != 0)
                xcb_free_colormap(b->conn, b->colormap);
            xcb_flush(b->conn);
        }
    }

    xkb_state_unref(b->xkb_state);
    xkb_keymap_unref(b->keymap);
    xkb_context_unref(b->xkb_ctx);
    if (b->conn != NULL)
        xcb_disconnect(b->conn);
    free(b);
}

static void
x11_set_title(struct backend *b, const char *title) {
    set_title(b, title);
}

static void
x11_bell(struct backend *b) {
    xcb_bell(b->conn, 0);
}

static void
x11_set_cursor_shape(struct backend *b, enum cursor_shape shape) {
    if (b->cursors[shape] == XCB_CURSOR_NONE)
        return;
    xcb_change_window_attributes(b->conn, b->win, XCB_CW_CURSOR, &b->cursors[shape]);
}

const struct backend_ops backend_x11 = {
    .name = "x11",
    .create = x11_create,
    .destroy = x11_destroy,
    .get_buffer = x11_get_buffer,
    .commit = x11_commit,
    .set_title = x11_set_title,
    .bell = x11_bell,
    .set_cursor_shape = x11_set_cursor_shape,
    .set_selection = x11_set_selection,
    .request_paste = x11_request_paste,
};
