#pragma once

#include <stdbool.h>

#include <pixman.h>

#include "backend/backend.h"
#include "render/font.h"
#include "term/term.h"

struct renderer {
    struct fonts *fonts;
    int pad_x, pad_y;
    bool focused;
    bool blink_off;    /* cursor hidden during the off phase of blinking */
    bool bell_on;      /* visual bell flash: overlays the whole frame */
    bool last_bell_on; /* bell_on as drawn in the previous frame */
    uint16_t bg_alpha; /* default background opacity, 0..0xffff;
                          used when the buffer has an alpha channel */

    uint16_t frame_alpha; /* bg_alpha, or opaque if the buffer has no alpha */

    /* State of the previously drawn frame */
    struct buffer *last_buf;
    int last_width, last_height;
    int last_cursor_row, last_cursor_col;
    bool force_full;
};

void render_init(struct renderer *r, struct fonts *fonts);

/* Requests a full redraw on the next frame (resize, font or focus change). */
void render_invalidate(struct renderer *r);

/* Draws dirty rows of t into buf and adds the redrawn area to damage
 * (initialized by the caller). Clears the rows' dirty flags. */
void render_frame(struct renderer *r, struct term *t, struct buffer *buf,
                  pixman_region32_t *damage);

/* True if the terminal has anything the next frame needs to draw. */
bool render_needed(const struct renderer *r, const struct term *t);
