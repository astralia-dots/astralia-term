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
    for (int i = 0; i < g->num_lines; i++)
        free(g->lines[i].cells);
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

        int len = (e - r) * old_cols + row_used(ring_row(g, e), old_cols);
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
            const struct row *src = ring_row(g, r + i / old_cols);
            int sc = i % old_cols;
            struct cell c = src->cells != NULL ? src->cells[sc] : (struct cell){0};
            if (c.cp == CELL_SPACER)
                continue; /* placed together with its head */

            int w = sc + 1 < old_cols && src->cells != NULL && src->cells[sc + 1].cp == CELL_SPACER ? 2 : 1;
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
        if (row_used(&o.rows[i], cols) > 0)
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
        else
            free(o.rows[i].cells);
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
