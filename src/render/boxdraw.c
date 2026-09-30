#include "render/boxdraw.h"

#include <math.h>

#include "core/util.h"

#define W(l, r, u, d) ((uint8_t)((l) | (r) << 2 | (u) << 4 | (d) << 6))

/* U+2500..U+257F as left, right, up, down weights (0 none, 1 light, 2 heavy, 3 double); 0 entries are dashes, arcs and diagonals */
static const uint8_t line_table[0x80] = {
    /* U+2500..U+250F */
    W(1, 1, 0, 0),
    W(2, 2, 0, 0),
    W(0, 0, 1, 1),
    W(0, 0, 2, 2),
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    W(0, 1, 0, 1),
    W(0, 2, 0, 1),
    W(0, 1, 0, 2),
    W(0, 2, 0, 2),
    /* U+2510..U+251F */
    W(1, 0, 0, 1),
    W(2, 0, 0, 1),
    W(1, 0, 0, 2),
    W(2, 0, 0, 2),
    W(0, 1, 1, 0),
    W(0, 2, 1, 0),
    W(0, 1, 2, 0),
    W(0, 2, 2, 0),
    W(1, 0, 1, 0),
    W(2, 0, 1, 0),
    W(1, 0, 2, 0),
    W(2, 0, 2, 0),
    W(0, 1, 1, 1),
    W(0, 2, 1, 1),
    W(0, 1, 2, 1),
    W(0, 1, 1, 2),
    /* U+2520..U+252F */
    W(0, 1, 2, 2),
    W(0, 2, 2, 1),
    W(0, 2, 1, 2),
    W(0, 2, 2, 2),
    W(1, 0, 1, 1),
    W(2, 0, 1, 1),
    W(1, 0, 2, 1),
    W(1, 0, 1, 2),
    W(1, 0, 2, 2),
    W(2, 0, 2, 1),
    W(2, 0, 1, 2),
    W(2, 0, 2, 2),
    W(1, 1, 0, 1),
    W(2, 1, 0, 1),
    W(1, 2, 0, 1),
    W(2, 2, 0, 1),
    /* U+2530..U+253F */
    W(1, 1, 0, 2),
    W(2, 1, 0, 2),
    W(1, 2, 0, 2),
    W(2, 2, 0, 2),
    W(1, 1, 1, 0),
    W(2, 1, 1, 0),
    W(1, 2, 1, 0),
    W(2, 2, 1, 0),
    W(1, 1, 2, 0),
    W(2, 1, 2, 0),
    W(1, 2, 2, 0),
    W(2, 2, 2, 0),
    W(1, 1, 1, 1),
    W(2, 1, 1, 1),
    W(1, 2, 1, 1),
    W(2, 2, 1, 1),
    /* U+2540..U+254F */
    W(1, 1, 2, 1),
    W(1, 1, 1, 2),
    W(1, 1, 2, 2),
    W(2, 1, 2, 1),
    W(1, 2, 2, 1),
    W(2, 1, 1, 2),
    W(1, 2, 1, 2),
    W(2, 2, 2, 1),
    W(2, 2, 1, 2),
    W(2, 1, 2, 2),
    W(1, 2, 2, 2),
    W(2, 2, 2, 2),
    0,
    0,
    0,
    0,
    /* U+2550..U+255F */
    W(3, 3, 0, 0),
    W(0, 0, 3, 3),
    W(0, 3, 0, 1),
    W(0, 1, 0, 3),
    W(0, 3, 0, 3),
    W(3, 0, 0, 1),
    W(1, 0, 0, 3),
    W(3, 0, 0, 3),
    W(0, 3, 1, 0),
    W(0, 1, 3, 0),
    W(0, 3, 3, 0),
    W(3, 0, 1, 0),
    W(1, 0, 3, 0),
    W(3, 0, 3, 0),
    W(0, 3, 1, 1),
    W(0, 1, 3, 3),
    /* U+2560..U+256F */
    W(0, 3, 3, 3),
    W(3, 0, 1, 1),
    W(1, 0, 3, 3),
    W(3, 0, 3, 3),
    W(3, 3, 0, 1),
    W(1, 1, 0, 3),
    W(3, 3, 0, 3),
    W(3, 3, 1, 0),
    W(1, 1, 3, 0),
    W(3, 3, 3, 0),
    W(3, 3, 1, 1),
    W(1, 1, 3, 3),
    W(3, 3, 3, 3),
    0,
    0,
    0,
    /* U+2570..U+257F */
    0,
    0,
    0,
    0,
    W(1, 0, 0, 0),
    W(0, 0, 1, 0),
    W(0, 1, 0, 0),
    W(0, 0, 0, 1),
    W(2, 0, 0, 0),
    W(0, 0, 2, 0),
    W(0, 2, 0, 0),
    W(0, 0, 0, 2),
    W(1, 2, 0, 0),
    W(0, 0, 1, 2),
    W(2, 1, 0, 0),
    W(0, 0, 2, 1),
};

static void
set_lines(struct boxdraw_spec *s, uint8_t packed) {
    s->kind = BOXDRAW_LINES;
    s->left = packed & 3;
    s->right = (packed >> 2) & 3;
    s->up = (packed >> 4) & 3;
    s->down = (packed >> 6) & 3;
}

static void
set_block(struct boxdraw_spec *s, int x0, int y0, int x1, int y1) {
    s->kind = BOXDRAW_BLOCK;
    s->x0 = x0;
    s->y0 = y0;
    s->x1 = x1;
    s->y1 = y1;
    s->alpha = 255;
}

static bool
lookup_dash(uint32_t cp, struct boxdraw_spec *s) {
    static const uint8_t dashes[] = {3, 3, 3, 3, 4, 4, 4, 4};
    s->kind = BOXDRAW_DASH;
    if (cp >= 0x2504 && cp <= 0x250b) {
        uint32_t i = cp - 0x2504;
        s->dashes = dashes[i];
        s->horizontal = (i & 2) == 0;
        s->weight = (i & 1) ? BOXDRAW_HEAVY : BOXDRAW_LIGHT;
        return true;
    }
    if (cp >= 0x254c && cp <= 0x254f) {
        uint32_t i = cp - 0x254c;
        s->dashes = 2;
        s->horizontal = (i & 2) == 0;
        s->weight = (i & 1) ? BOXDRAW_HEAVY : BOXDRAW_LIGHT;
        return true;
    }
    return false;
}

static void
lookup_block(uint32_t cp, struct boxdraw_spec *s) {
    static const uint8_t quadrants[] = {
        BOXDRAW_LL,
        BOXDRAW_LR,
        BOXDRAW_UL,
        BOXDRAW_UL | BOXDRAW_LL | BOXDRAW_LR,
        BOXDRAW_UL | BOXDRAW_LR,
        BOXDRAW_UL | BOXDRAW_UR | BOXDRAW_LL,
        BOXDRAW_UL | BOXDRAW_UR | BOXDRAW_LR,
        BOXDRAW_UR,
        BOXDRAW_UR | BOXDRAW_LL,
        BOXDRAW_UR | BOXDRAW_LL | BOXDRAW_LR,
    };
    int n = cp - 0x2580;

    if (n == 0)
        set_block(s, 0, 0, 8, 4);
    else if (n <= 8)
        set_block(s, 0, 8 - n, 8, 8);
    else if (n <= 15)
        set_block(s, 0, 0, 16 - n, 8);
    else if (n == 16)
        set_block(s, 4, 0, 8, 8);
    else if (n <= 19) {
        set_block(s, 0, 0, 8, 8);
        s->alpha = 64 * (n - 16) - 1;
    } else if (n == 20)
        set_block(s, 0, 0, 8, 1);
    else if (n == 21)
        set_block(s, 7, 0, 8, 8);
    else {
        set_block(s, 0, 0, 0, 0);
        s->quadrants = quadrants[n - 22];
    }
}

bool boxdraw_lookup(uint32_t cp, struct boxdraw_spec *out) {
    if (cp < BOXDRAW_FIRST || cp > BOXDRAW_LAST)
        return false;

    struct boxdraw_spec s = {0};
    if (cp >= 0x2580) {
        lookup_block(cp, &s);
    } else if (cp >= 0x2571 && cp <= 0x2573) {
        s.kind = BOXDRAW_DIAG;
        s.diag = cp - 0x2570;
    } else if (cp >= 0x256d && cp <= 0x2570) {
        s.kind = BOXDRAW_ARC;
        s.right = cp == 0x256d || cp == 0x2570;
        s.left = !s.right;
        s.down = cp == 0x256d || cp == 0x256e;
        s.up = !s.down;
    } else if (!lookup_dash(cp, &s)) {
        set_lines(&s, line_table[cp - BOXDRAW_FIRST]);
    }
    *out = s;
    return true;
}

static int
thickness(unsigned weight, int light) {
    switch (weight) {
    case BOXDRAW_LIGHT:
        return light;
    case BOXDRAW_HEAVY:
        return light * 2;
    case BOXDRAW_DOUBLE:
        return light * 3;
    default:
        return 0;
    }
}

static void
add_rect(struct boxdraw_rect *out, int *n, int x0, int y0, int x1, int y1, uint8_t alpha) {
    if (x1 <= x0 || y1 <= y0)
        return;
    out[(*n)++] = (struct boxdraw_rect){x0, y0, x1 - x0, y1 - y0, alpha};
}

/* Where perpendicular arms meet: the span all strokes cross */
static void
structure_span(int len, int t_a, int t_b, int *lo, int *hi) {
    *lo = *hi = len / 2;
    bool have = false;
    int ts[2] = {t_a, t_b};
    for (int i = 0; i < 2; i++) {
        if (ts[i] == 0)
            continue;
        int s = (len - ts[i]) / 2;
        *lo = have ? MIN(*lo, s) : s;
        *hi = have ? MAX(*hi, s + ts[i]) : s + ts[i];
        have = true;
    }
}

static int
line_rects(const struct boxdraw_spec *s, int cw, int ch, int light, struct boxdraw_rect *out) {
    int n = 0;
    int tl = thickness(s->left, light), tr = thickness(s->right, light);
    int tu = thickness(s->up, light), td = thickness(s->down, light);
    int vx0, vx1, hy0, hy1;
    structure_span(cw, tu, td, &vx0, &vx1);
    structure_span(ch, tl, tr, &hy0, &hy1);
    bool vdouble = s->up == BOXDRAW_DOUBLE || s->down == BOXDRAW_DOUBLE;
    bool hdouble = s->left == BOXDRAW_DOUBLE || s->right == BOXDRAW_DOUBLE;

    /* A double arm meeting a double structure stops its inner stroke at the corner */
    for (int k = 0; k < 2; k++) {
        int x = (cw - 3 * light) / 2 + 2 * light * k;
        if (s->up == BOXDRAW_DOUBLE) {
            bool inner = hdouble && (k == 0 ? s->left : s->right);
            add_rect(out, &n, x, 0, x + light, inner ? hy0 + light : hy1, 255);
        }
        if (s->down == BOXDRAW_DOUBLE) {
            bool inner = hdouble && (k == 0 ? s->left : s->right);
            add_rect(out, &n, x, inner ? hy1 - light : hy0, x + light, ch, 255);
        }
        int y = (ch - 3 * light) / 2 + 2 * light * k;
        if (s->left == BOXDRAW_DOUBLE) {
            bool inner = vdouble && (k == 0 ? s->up : s->down);
            add_rect(out, &n, 0, y, inner ? vx0 + light : vx1, y + light, 255);
        }
        if (s->right == BOXDRAW_DOUBLE) {
            bool inner = vdouble && (k == 0 ? s->up : s->down);
            add_rect(out, &n, inner ? vx1 - light : vx0, y, cw, y + light, 255);
        }
    }

    int x = (cw - tu) / 2;
    if (s->up != BOXDRAW_DOUBLE)
        add_rect(out, &n, x, 0, x + tu, hy1, 255);
    x = (cw - td) / 2;
    if (s->down != BOXDRAW_DOUBLE)
        add_rect(out, &n, x, hy0, x + td, ch, 255);
    int y = (ch - tl) / 2;
    if (s->left != BOXDRAW_DOUBLE)
        add_rect(out, &n, 0, y, vx1, y + tl, 255);
    y = (ch - tr) / 2;
    if (s->right != BOXDRAW_DOUBLE)
        add_rect(out, &n, vx0, y, cw, y + tr, 255);
    return n;
}

static int
dash_rects(const struct boxdraw_spec *s, int cw, int ch, int light, struct boxdraw_rect *out) {
    int n = 0;
    int len = s->horizontal ? cw : ch;
    int t = thickness(s->weight, light);
    int gap = MAX(1, len / (2 * s->dashes));
    int g0 = gap / 2, g1 = gap - g0;

    for (int i = 0; i < s->dashes; i++) {
        int a = i * len / s->dashes + g0;
        int b = (i + 1) * len / s->dashes - g1;
        if (s->horizontal)
            add_rect(out, &n, a, (ch - t) / 2, b, (ch - t) / 2 + t, 255);
        else
            add_rect(out, &n, (cw - t) / 2, a, (cw - t) / 2 + t, b, 255);
    }
    return n;
}

static int
block_rects(const struct boxdraw_spec *s, int cw, int ch, struct boxdraw_rect *out) {
    int n = 0;

    if (s->quadrants == 0) {
        add_rect(out, &n, cw * s->x0 / 8, ch * s->y0 / 8, cw * s->x1 / 8, ch * s->y1 / 8,
                 s->alpha);
        return n;
    }
    int xm = cw / 2, ym = ch / 2;
    if (s->quadrants & BOXDRAW_UL)
        add_rect(out, &n, 0, 0, xm, ym, 255);
    if (s->quadrants & BOXDRAW_UR)
        add_rect(out, &n, xm, 0, cw, ym, 255);
    if (s->quadrants & BOXDRAW_LL)
        add_rect(out, &n, 0, ym, xm, ch, 255);
    if (s->quadrants & BOXDRAW_LR)
        add_rect(out, &n, xm, ym, cw, ch, 255);
    return n;
}

int boxdraw_rects(const struct boxdraw_spec *s, int cw, int ch, int light,
                  struct boxdraw_rect *out) {
    switch (s->kind) {
    case BOXDRAW_LINES:
        return line_rects(s, cw, ch, light, out);
    case BOXDRAW_DASH:
        return dash_rects(s, cw, ch, light, out);
    case BOXDRAW_BLOCK:
        return block_rects(s, cw, ch, out);
    default:
        return 0;
    }
}

#define SUPERSAMPLE 4

struct stroke {
    const struct boxdraw_spec *spec;
    int cw, ch;
    double half;
    double cx, cy; /* where the two stroke centerlines meet */
    double ex, ey; /* +1 or -1: direction of the horizontal and vertical arms */
    double radius;
};

/* Circular corner of radius min(cx, cy), joined to straight arms */
static bool
arc_covers(const struct stroke *k, double x, double y) {
    double u = (x - k->cx) * k->ex, v = (y - k->cy) * k->ey;
    double dist;
    if (u >= k->radius && v >= k->radius)
        return false;
    if (u >= k->radius)
        dist = v;
    else if (v >= k->radius)
        dist = u;
    else
        dist = hypot(u - k->radius, v - k->radius) - k->radius;
    return fabs(dist) <= k->half;
}

static bool
diag_covers(const struct stroke *k, double x, double y) {
    double norm = hypot(k->cw, k->ch);
    if ((k->spec->diag & 1) && fabs(k->ch * x + k->cw * y - (double)k->cw * k->ch) / norm <= k->half)
        return true;
    return (k->spec->diag & 2) && fabs(k->ch * x - k->cw * y) / norm <= k->half;
}

void boxdraw_mask(const struct boxdraw_spec *s, int cw, int ch, int light, uint8_t *mask,
                  int stride) {
    int sx0 = (cw - light) / 2, sy0 = (ch - light) / 2;
    double cx = sx0 + light / 2.0;
    double cy = sy0 + light / 2.0;
    struct stroke k = {.spec = s, .cw = cw, .ch = ch, .half = light / 2.0};
    k.cx = cx;
    k.cy = cy;
    k.ex = s->right ? 1 : -1;
    k.ey = s->down ? 1 : -1;
    k.radius = MIN(cx, cy);

    for (int py = 0; py < ch; py++) {
        for (int px = 0; px < cw; px++) {
            int hits = 0;
            for (int sy = 0; sy < SUPERSAMPLE; sy++) {
                for (int sx = 0; sx < SUPERSAMPLE; sx++) {
                    double x = px + (sx + 0.5) / SUPERSAMPLE;
                    double y = py + (sy + 0.5) / SUPERSAMPLE;
                    hits += s->kind == BOXDRAW_ARC ? arc_covers(&k, x, y) : diag_covers(&k, x, y);
                }
            }
            mask[py * stride + px] = hits * 255 / (SUPERSAMPLE * SUPERSAMPLE);
        }
    }
}
