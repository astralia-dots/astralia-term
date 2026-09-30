#include "term/grid.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "grid"
#include "core/util.h"

void grid_init(struct grid *g, int cols, int rows, int scrollback) {
    *g = (struct grid){
        .cols = cols,
        .rows = rows,
        .num_lines = rows + scrollback,
    };
    g->lines = xcalloc(g->num_lines, sizeof(g->lines[0]));
}

void grid_free(struct grid *g) {
    if (g->lines == NULL)
        return;
    for (int i = 0; i < g->num_lines; i++) {
        free(g->lines[i].cells);
        free(g->lines[i].overflow);
    }
    free(g->lines);
    g->lines = NULL;
}

static inline struct row *
ring_row(struct grid *g, int r) {
    int idx = (g->offset + r) % g->num_lines;
    if (idx < 0)
        idx += g->num_lines;
    return &g->lines[idx];
}

struct row *
grid_row(struct grid *g, int r) {
    struct row *row = ring_row(g, r);
    if (unlikely(row->cells == NULL)) {
        row->cells = xcalloc(g->cols, sizeof(struct cell));
        row->dirty = true;
    }
    return row;
}

void grid_row_fill(struct row *row, int from, int to, struct cell blank) {
    free(row->overflow);
    row->overflow = NULL;
    row->overflow_len = 0;

    struct cell *c = row->cells;
    if (blank.cp == 0 && blank.fg == 0 && blank.bg == 0 && blank.attrs == 0)
        memset(&c[from], 0, (size_t)(to - from) * sizeof(*c));
    else {
        for (int i = from; i < to; i++)
            c[i] = blank;
    }
    row->dirty = true;
}

void grid_mark_all_dirty(struct grid *g) {
    for (int r = 0; r < g->rows; r++)
        ring_row(g, r)->dirty = true;
}

static void
reset_row(struct grid *g, struct row *row, struct cell blank) {
    if (row->cells == NULL)
        row->cells = xcalloc(g->cols, sizeof(struct cell));
    grid_row_fill(row, 0, g->cols, blank);
    row->wrapped = false;
}

int grid_scroll_up(struct grid *g, int top, int bottom, int n, struct cell blank) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0)
        return 0;
    if (n > height)
        n = height;

    if (top == 0 && bottom == g->rows - 1) {
        /* Rotate the ring; the old top rows become scrollback */
        g->offset = (g->offset + n) % g->num_lines;
        g->scrollback_used = MIN(g->scrollback_used + n, g->num_lines - g->rows);
        for (int r = g->rows - n; r < g->rows; r++)
            reset_row(g, ring_row(g, r), blank);
        grid_mark_all_dirty(g);
        return g->num_lines > g->rows ? n : 0;
    }

    /* Rotate row structs inside the region (cells pointers move, no copy) */
    for (int r = top; r <= bottom - n; r++) {
        struct row *dst = ring_row(g, r);
        struct row *src = ring_row(g, r + n);
        struct row tmp = *dst;
        *dst = *src;
        *src = tmp;
        dst->dirty = true;
    }
    for (int r = bottom - n + 1; r <= bottom; r++)
        reset_row(g, ring_row(g, r), blank);
    return 0;
}

void grid_scroll_down(struct grid *g, int top, int bottom, int n, struct cell blank) {
    int height = bottom - top + 1;
    if (n <= 0 || height <= 0)
        return;
    if (n > height)
        n = height;

    for (int r = bottom; r >= top + n; r--) {
        struct row *dst = ring_row(g, r);
        struct row *src = ring_row(g, r - n);
        struct row tmp = *dst;
        *dst = *src;
        *src = tmp;
        dst->dirty = true;
    }
    for (int r = top; r < top + n; r++)
        reset_row(g, ring_row(g, r), blank);
}

static void
clamp_points(struct grid_point *points, int npoints, int cols, int rows) {
    for (int p = 0; p < npoints; p++) {
        points[p].row = CLAMP(points[p].row, 0, rows - 1);
        points[p].col = CLAMP(points[p].col, 0, cols - 1);
    }
}

static void
resize_truncate(struct grid *g, int cols, int rows, struct grid_point *points, int npoints) {
    /* Keep the cursor line on screen: drop lines from the top if needed */
    int skip = npoints > 0 && points[0].row >= rows ? points[0].row - rows + 1 : 0;
    int copy = MIN(g->rows - skip, rows);

    struct grid ng;
    grid_init(&ng, cols, rows, g->num_lines - g->rows);

    for (int r = 0; r < copy; r++) {
        struct row *src = ring_row(g, r + skip);
        struct row *dst = &ng.lines[r];
        if (src->cells == NULL)
            continue;
        dst->cells = xcalloc(cols, sizeof(struct cell));
        memcpy(dst->cells, src->cells, (size_t)MIN(cols, g->cols) * sizeof(struct cell));
        /* Don't leave half a wide character at the right edge */
        if (cols < g->cols && dst->cells[cols - 1].cp != 0 && src->cells[cols].cp == CELL_SPACER)
            dst->cells[cols - 1] = (struct cell){0};
        dst->wrapped = src->wrapped && cols >= g->cols;
    }

    grid_free(g);
    *g = ng;
    for (int p = 0; p < npoints; p++)
        points[p].row -= skip;
    clamp_points(points, npoints, cols, rows);
}

/* Index of the last non-blank cell, plus one */
static int
row_used(const struct row *row, int cols) {
    if (row->cells == NULL)
        return 0;
    static const struct cell blank = {0};
    for (int i = cols; i > 0; i--) {
        if (memcmp(&row->cells[i - 1], &blank, sizeof(blank)) != 0)
            return i;
    }
    return 0;
}

struct reflow_out {
    struct row *rows;
    int count, cap, cols;
};

static int
out_new_row(struct reflow_out *o) {
    if (o->count == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        o->rows = xrealloc(o->rows, o->cap * sizeof(o->rows[0]));
    }
    o->rows[o->count] = (struct row){.cells = xcalloc(o->cols, sizeof(struct cell))};
    return o->count++;
}

#define MAX_POINTS 4

/* Cell i of a row's cells followed by its overflow */
static struct cell
src_cell(const struct row *row, int old_cols, int i) {
    if (i < old_cols)
        return row->cells != NULL ? row->cells[i] : (struct cell){0};
    return row->overflow[i - old_cols];
}

/* Copies src into dst (cols wide, zeroed) up to the right edge and stashes
 * the rest, trailing blanks trimmed, in dst's overflow. */
static void
clip_row(struct row *dst, const struct row *src, int old_cols, int cols) {
    static const struct cell blank = {0};
    int len = src->overflow != NULL ? old_cols + src->overflow_len : row_used(src, old_cols);
    while (len > 0) {
        struct cell c = src_cell(src, old_cols, len - 1);
        if (memcmp(&c, &blank, sizeof(blank)) != 0)
            break;
        len--;
    }

    for (int i = 0; i < MIN(len, cols); i++)
        dst->cells[i] = src_cell(src, old_cols, i);
    if (len > cols) {
        dst->overflow_len = len - cols;
        dst->overflow = xmalloc((size_t)dst->overflow_len * sizeof(struct cell));
        for (int i = cols; i < len; i++)
            dst->overflow[i - cols] = src_cell(src, old_cols, i);
        /* Don't leave half a wide character at the right edge; it is lost */
        if (dst->overflow[0].cp == CELL_SPACER)
            dst->cells[cols - 1] = dst->overflow[0] = blank;
    }
    dst->wrapped = src->wrapped;
}

static void
resize_reflow(struct grid *g, int cols, int rows, struct grid_point *points, int npoints) {
    const int old_cols = g->cols;
    struct reflow_out o = {.cols = cols};
    struct grid_point out_pt[MAX_POINTS];
    bool found[MAX_POINTS] = {false};

    /* Walk logical lines: runs of rows joined by the wrapped flag */
    for (int r = -g->scrollback_used; r < g->rows;) {
        int e = r;
        while (e < g->rows - 1 && ring_row(g, e)->wrapped)
            e++;

        /* Only soft-wrapped text and the cursor's line re-wrap; cursor-drawn
         * rows are clipped so they never tear. */
        bool has_cursor = npoints > 0 && points[0].row >= r && points[0].row <= e;
        if (r == e && !has_cursor) {
            int di = out_new_row(&o);
            clip_row(&o.rows[di], ring_row(g, r), old_cols, cols);
            for (int p = 0; p < npoints; p++) {
                if (points[p].row == r) {
                    out_pt[p] = (struct grid_point){di, MIN(points[p].col, cols - 1)};
                    found[p] = true;
                }
            }
            r = e + 1;
            continue;
        }

        /* Only the last row's overflow joins the line; a middle row's is lost */
        const struct row *last = ring_row(g, e);
        int last_len = last->overflow != NULL ? old_cols + last->overflow_len : row_used(last, old_cols);
        int len = (e - r) * old_cols + last_len;
        int offs[MAX_POINTS];
        for (int p = 0; p < npoints; p++) {
            offs[p] = -1;
            if (points[p].row >= r && points[p].row <= e) {
                offs[p] = (points[p].row - r) * old_cols + points[p].col;
                len = MAX(len, offs[p] + 1);
            }
        }

        int di = out_new_row(&o);
        int col = 0;
        for (int i = 0; i < len; i++) {
            int k = MIN(r + i / old_cols, e);
            const struct row *src = ring_row(g, k);
            int sc = i - (k - r) * old_cols;
            int src_len = k == e ? last_len : old_cols;
            struct cell c = sc < src_len ? src_cell(src, old_cols, sc) : (struct cell){0};
            if (c.cp == CELL_SPACER)
                continue; /* placed together with its head */

            int w = sc + 1 < src_len && src_cell(src, old_cols, sc + 1).cp == CELL_SPACER ? 2 : 1;
            if (w > cols) {
                c = (struct cell){0};
                w = 1;
            }
            if (col + w > cols) {
                o.rows[di].wrapped = true;
                di = out_new_row(&o);
                col = 0;
            }

            o.rows[di].cells[col] = c;
            if (w == 2) {
                o.rows[di].cells[col + 1] = c;
                o.rows[di].cells[col + 1].cp = CELL_SPACER;
            }
            for (int p = 0; p < npoints; p++) {
                if (offs[p] >= i && offs[p] < i + w) {
                    out_pt[p] = (struct grid_point){di, col + offs[p] - i};
                    found[p] = true;
                }
            }
            col += w;
        }
        for (int p = 0; p < npoints; p++) {
            if (offs[p] >= 0 && !found[p]) {
                out_pt[p] = (struct grid_point){di, MIN(col, cols - 1)};
                found[p] = true;
            }
        }
        r = e + 1;
    }

    /* Drop trailing blank rows below the content and the tracked points */
    int keep = 0;
    for (int i = 0; i < o.count; i++) {
        if (row_used(&o.rows[i], cols) > 0 || o.rows[i].overflow != NULL)
            keep = i + 1;
    }
    for (int p = 0; p < npoints; p++) {
        if (found[p])
            keep = MAX(keep, out_pt[p].row + 1);
    }

    int top = MAX(0, keep - rows);
    if (npoints > 0 && found[0])
        top = MIN(top, out_pt[0].row);
    int history_cap = g->num_lines - g->rows;
    int start = MAX(0, top - history_cap);
    int end = MIN(keep, top + rows);

    struct grid ng;
    grid_init(&ng, cols, rows, history_cap);
    for (int i = 0; i < o.count; i++) {
        if (i >= start && i < end)
            *ring_row(&ng, i - top) = o.rows[i];
        else {
            free(o.rows[i].cells);
            free(o.rows[i].overflow);
        }
    }
    ng.scrollback_used = top - start;
    free(o.rows);

    grid_free(g);
    *g = ng;
    for (int p = 0; p < npoints; p++) {
        if (found[p])
            points[p] = (struct grid_point){out_pt[p].row - top, out_pt[p].col};
    }
    clamp_points(points, npoints, cols, rows);
}

void grid_resize(struct grid *g, int cols, int rows, bool reflow,
                 struct grid_point *points, int npoints) {
    npoints = MIN(npoints, MAX_POINTS);
    if (reflow)
        resize_reflow(g, cols, rows, points, npoints);
    else
        resize_truncate(g, cols, rows, points, npoints);
    grid_mark_all_dirty(g);
}
