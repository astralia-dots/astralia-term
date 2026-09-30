#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Colors: tag in the top byte, value in the low 24 bits. An all-zero
 * color is the default fg/bg, so a zeroed cell is a blank default cell. */
enum color_tag { COLOR_DEFAULT = 0,
                 COLOR_PALETTE = 1,
                 COLOR_RGB = 2 };
#define COLOR_TAG(c) ((c) >> 24)
#define COLOR_VALUE(c) ((c) & 0xffffff)
#define COLOR_MAKE(tag, v) (((uint32_t)(tag) << 24) | ((uint32_t)(v) & 0xffffff))

enum cell_attr {
    ATTR_BOLD = 1 << 0,
    ATTR_DIM = 1 << 1,
    ATTR_ITALIC = 1 << 2,
    ATTR_UNDERLINE_SHIFT = 3, /* 3 bits: 0 none, 1 single, 2 double,
                                 3 curly, 4 dotted, 5 dashed */
    ATTR_UNDERLINE_MASK = 7 << 3,
    ATTR_BLINK = 1 << 6,
    ATTR_REVERSE = 1 << 7,
    ATTR_INVISIBLE = 1 << 8,
    ATTR_STRIKE = 1 << 9,
    ATTR_OVERLINE = 1 << 10,
    /* Cell is part of a non-virtual graphics placement: `ul` is a placement
     * ID (not an underline color), `cp` is 0, and tile_row/tile_col give
     * this cell's position within the placement's cell grid. */
    ATTR_IMAGE = 1 << 11,
    /* Cell is inside an OSC 8 hyperlink: tile_row/tile_col hold the link
     * handle (high/low byte); never set together with ATTR_IMAGE. */
    ATTR_LINK = 1 << 12,
};

/* Second half of a double-width character; outside the Unicode range. */
#define CELL_SPACER 0x110000u

/* Kitty keyboard protocol progressive-enhancement flags (CSI ... u). */
enum kitty_kbd_flags {
    KITTY_KBD_DISAMBIGUATE = 1 << 0,
    KITTY_KBD_REPORT_EVENT = 1 << 1,
    KITTY_KBD_REPORT_ALTERNATE = 1 << 2,
    KITTY_KBD_REPORT_ALL = 1 << 3,
    KITTY_KBD_REPORT_ASSOCIATED = 1 << 4,
};
#define KITTY_KBD_SUPPORTED                                                         \
    (KITTY_KBD_DISAMBIGUATE | KITTY_KBD_REPORT_EVENT | KITTY_KBD_REPORT_ALTERNATE | \
     KITTY_KBD_REPORT_ALL | KITTY_KBD_REPORT_ASSOCIATED)

struct cell {
    uint32_t cp;
    uint32_t fg, bg;
    uint32_t ul; /* underline color (SGR 58/59); kitty graphics placement ID */
    uint16_t attrs;
    uint8_t tile_row, tile_col; /* ATTR_IMAGE: position within the placement's cell grid;
                                   ATTR_LINK: hyperlink handle */
};
_Static_assert(sizeof(struct cell) == 20, "struct cell must be 20 bytes");

struct row {
    struct cell *cells; /* NULL until first touched */
    bool dirty;
    bool wrapped; /* line continues on the next row (soft wrap) */
};

struct grid {
    int cols, rows;      /* visible size */
    int num_lines;       /* ring capacity: rows + scrollback */
    int offset;          /* ring index of visible row 0 */
    int scrollback_used; /* lines above row 0 holding history */
    struct row *lines;

    /* Kitty keyboard protocol flags stack; normal and alt screens each
     * have their own, since term.grid switches between the two. All-zero
     * (the calloc/RIS default) means the protocol is off. */
    struct {
        uint8_t flags[8];
        int idx;
    } kitty_kbd;
};

void grid_init(struct grid *g, int cols, int rows, int scrollback);
void grid_free(struct grid *g);

/* A position in visible-row coordinates, tracked through grid_resize(). */
struct grid_point {
    int row, col;
};

/* Visible row r (0-based; down to -scrollback_used for history),
 * allocating its cells on first use. */
struct row *grid_row(struct grid *g, int r);

/* Scroll rows [top, bottom] (inclusive) by n. New rows are filled with
 * blank. A full-screen scroll-up pushes lines into scrollback; returns the
 * number of lines pushed. */
int grid_scroll_up(struct grid *g, int top, int bottom, int n, struct cell blank);
void grid_scroll_down(struct grid *g, int top, int bottom, int n, struct cell blank);

/* Fill cells [from, to) of a row with blank and mark it dirty. */
void grid_row_fill(struct row *row, int from, int to, struct cell blank);

void grid_mark_all_dirty(struct grid *g);

/* Resize to cols x rows. With reflow, soft-wrapped lines (scrollback
 * included) are re-wrapped to the new width; without it, rows are
 * truncated and history is dropped. points[0] (the cursor) stays on screen;
 * every point is moved along with its text. */
void grid_resize(struct grid *g, int cols, int rows, bool reflow,
                 struct grid_point *points, int npoints);
