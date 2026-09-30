#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOXDRAW_FIRST 0x2500
#define BOXDRAW_LAST 0x259f
#define BOXDRAW_MAX_RECTS 16

enum boxdraw_kind {
    BOXDRAW_LINES,
    BOXDRAW_DASH,
    BOXDRAW_BLOCK,
    BOXDRAW_ARC,
    BOXDRAW_DIAG,
};

enum boxdraw_weight {
    BOXDRAW_NONE,
    BOXDRAW_LIGHT,
    BOXDRAW_HEAVY,
    BOXDRAW_DOUBLE,
};

enum boxdraw_quadrant {
    BOXDRAW_UL = 1,
    BOXDRAW_UR = 2,
    BOXDRAW_LL = 4,
    BOXDRAW_LR = 8,
};

struct boxdraw_spec {
    enum boxdraw_kind kind;
    uint8_t left, right, up, down; /* enum boxdraw_weight; LINES and ARC */
    uint8_t dashes;                /* DASH: segments per cell */
    bool horizontal;               /* DASH */
    uint8_t weight;                /* DASH */
    uint8_t x0, y0, x1, y1;        /* BLOCK: one rectangle in eighths of the cell */
    uint8_t quadrants;             /* BLOCK: union of enum boxdraw_quadrant, replaces the rectangle */
    uint8_t alpha;                 /* BLOCK: 255 solid, less for shades */
    uint8_t diag;                  /* DIAG: 1 for U+2571, 2 for U+2572, 3 for both */
};

struct boxdraw_rect {
    int x, y, w, h;
    uint8_t alpha;
};

/* Return false when the codepoint is not drawn procedurally */
bool boxdraw_lookup(uint32_t cp, struct boxdraw_spec *out);

/* Fill up to BOXDRAW_MAX_RECTS rectangles for a LINES, DASH or BLOCK spec; returns the count */
int boxdraw_rects(const struct boxdraw_spec *s, int cw, int ch, int light,
                  struct boxdraw_rect *out);

/* Write 8-bit coverage of an ARC or DIAG spec into a cw x ch mask */
void boxdraw_mask(const struct boxdraw_spec *s, int cw, int ch, int light, uint8_t *mask,
                  int stride);
