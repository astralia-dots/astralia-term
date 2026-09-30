#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pixman.h>
#include <xkbcommon/xkbcommon.h>

#include "config-build.h"

struct loop;
struct backend;

/* A shared-memory buffer the renderer draws into: PIXMAN_a8r8g8b8
 * (premultiplied) when the window supports translucency, otherwise
 * PIXMAN_x8r8g8b8. Backends embed this as the first member of their own
 * buffer struct. */
struct buffer {
    int width, height, stride;
    void *data;
    pixman_image_t *pix;
    bool busy; /* owned by the display server */
};

enum key_mods {
    MOD_SHIFT = 1 << 0,
    MOD_ALT = 1 << 1,
    MOD_CTRL = 1 << 2,
    MOD_SUPER = 1 << 3,
};

enum key_action {
    KEY_PRESS,
    KEY_REPEAT,
    KEY_RELEASE,
};

enum selection_target {
    SELECTION_PRIMARY,
    SELECTION_CLIPBOARD,
};

struct key_event {
    xkb_keysym_t sym;
    xkb_keysym_t unshifted; /* level 0 at the active layout, for the alternate key */
    xkb_keysym_t base;      /* level 0 at layout 0, for the base-layout key */
    unsigned mods;          /* enum key_mods */
    enum key_action action;
    char utf8[32]; /* xkb text, Ctrl transformation applied */
    int utf8_len;
};

/* Events from the backend to the application. */
struct backend_listener {
    void (*configure)(void *data, int width, int height); /* buffer pixel size */
    void (*frame)(void *data);                            /* ready for a new frame */
    void (*key)(void *data, const struct key_event *ev);
    void (*focus)(void *data, bool focused);
    void (*close)(void *data);
    /* Output scale (1.0 = 96 dpi): Wayland fractional scale, X11 Xft.dpi / 96.
     * Fires when it changes, possibly before the first configure; a Wayland
     * change is followed by a configure with the rescaled pixel size. */
    void (*scale)(void *data, double scale);

    /* Pointer events, in buffer pixel coordinates (same space as configure). */
    void (*pointer_motion)(void *data, int x, int y, unsigned mods);
    /* button: 1=left, 2=middle, 3=right */
    void (*pointer_button)(void *data, int button, bool pressed, int x, int y, unsigned mods);
    /* steps_x/steps_y: positive = right/down; magnitude is notch count */
    void (*scroll)(void *data, int steps_x, int steps_y, int x, int y, unsigned mods);

    /* Reply to request_paste(); fires 0..1 times, never for an empty or
     * unavailable selection. */
    void (*paste)(void *data, const char *text, size_t len);
    /* Another client claimed target; PRIMARY loss also clears the visible
     * highlight (the caller calls selection_clear()); CLIPBOARD loss is
     * silent: the highlight always tracks PRIMARY. */
    void (*selection_lost)(void *data, enum selection_target target);
};

/* Pointer shape over the window. */
enum cursor_shape {
    CURSOR_SHAPE_TEXT,
    CURSOR_SHAPE_POINTER,
};

struct backend_ops {
    const char *name;

    /* Connects, creates the window and registers fds with the loop.
     * Returns NULL if the display server can't be reached. */
    struct backend *(*create)(struct loop *loop, const struct backend_listener *l,
                              void *data, int width, int height,
                              const char *app_id, const char *title);
    void (*destroy)(struct backend *b);

    /* A buffer of the current window size that isn't busy, or NULL. */
    struct buffer *(*get_buffer)(struct backend *b, int width, int height);
    /* Presents buf; damage is in buffer coordinates. The backend calls
     * listener->frame when the next frame may be drawn. */
    void (*commit)(struct backend *b, struct buffer *buf, pixman_region32_t *damage);

    void (*set_title)(struct backend *b, const char *title);
    void (*bell)(struct backend *b);
    /* Cheap to repeat; the shape is also restored when the pointer re-enters. */
    void (*set_cursor_shape)(struct backend *b, enum cursor_shape shape);

    /* Copies text into backend-owned storage and claims ownership of target. */
    void (*set_selection)(struct backend *b, enum selection_target target,
                          const char *text, size_t len);
    /* Asynchronous; listener->paste() fires once the data arrives. */
    void (*request_paste)(struct backend *b, enum selection_target target);
};

#if HAVE_WAYLAND
extern const struct backend_ops backend_wayland;
#endif
#if HAVE_X11
extern const struct backend_ops backend_x11;
#endif
