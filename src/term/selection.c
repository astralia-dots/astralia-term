#include "term/selection.h"

#include <ctype.h>
#include <string.h>

#define LOG_MODULE "selection"
#include "core/util.h"
#include "term/composed.h"
#include "term/term.h"

enum char_class {
    CLASS_BLANK,
    CLASS_WORD,
    CLASS_DELIM,
};

static uint32_t
resolve_cp(const struct term *t, const struct row *row, int col) {
    uint32_t cp = row->cells[col].cp;
    if (cp == CELL_SPACER && col > 0)
        cp = row->cells[col - 1].cp;
    const struct composed_chain *chain = composed_get(&t->composed, cp);
    return chain != NULL ? chain->cps[0] : cp;
}

static enum char_class
classify(uint32_t cp) {
    if (cp == 0)
        return CLASS_BLANK;
    if (cp == '_' || cp >= 0x80 || isalnum((int)cp))
        return CLASS_WORD;
    return CLASS_DELIM;
}

static int
point_cmp(struct grid_point a, struct grid_point b) {
    if (a.row != b.row)
        return a.row < b.row ? -1 : 1;
    if (a.col != b.col)
        return a.col < b.col ? -1 : 1;
    return 0;
}

/* Word (or run of mixed punctuation) under (row, col), crossing into a
 * neighboring row only where the row being left is soft-wrapped. */
static struct grid_range
word_range_at(struct term *t, int row, int col) {
    enum char_class cls = classify(resolve_cp(t, grid_row(t->grid, row), col));

    struct grid_point start = {row, col};
    for (;;) {
        if (start.col > 0) {
            if (classify(resolve_cp(t, grid_row(t->grid, start.row), start.col - 1)) != cls)
                break;
            start.col--;
        } else if (start.row - 1 >= -t->grid->scrollback_used &&
                   grid_row(t->grid, start.row - 1)->wrapped) {
            if (classify(resolve_cp(t, grid_row(t->grid, start.row - 1), t->cols - 1)) != cls)
                break;
            start.row--;
            start.col = t->cols - 1;
        } else
            break;
    }

    struct grid_point end = {row, col};
    for (;;) {
        if (end.col < t->cols - 1) {
            if (classify(resolve_cp(t, grid_row(t->grid, end.row), end.col + 1)) != cls)
                break;
            end.col++;
        } else if (end.row < t->rows - 1 && grid_row(t->grid, end.row)->wrapped) {
            if (classify(resolve_cp(t, grid_row(t->grid, end.row + 1), 0)) != cls)
                break;
            end.row++;
            end.col = 0;
        } else
            break;
    }

    return (struct grid_range){start, end};
}

/* The full soft-wrapped logical line through row, same concept grid.c's
 * resize_reflow() uses to join wrapped lines. */
static struct grid_range
line_range_at(struct term *t, int row) {
    int top = row;
    while (top - 1 >= -t->grid->scrollback_used && grid_row(t->grid, top - 1)->wrapped)
        top--;
    int bottom = row;
    while (bottom < t->rows - 1 && grid_row(t->grid, bottom)->wrapped)
        bottom++;
    return (struct grid_range){{top, 0}, {bottom, t->cols - 1}};
}

static struct grid_range
range_for_point(struct term *t, int row, int col, enum selection_kind kind) {
    switch (kind) {
    case SEL_WORD:
        return word_range_at(t, row, col);
    case SEL_LINE:
        return line_range_at(t, row);
    default:
        return (struct grid_range){{row, col}, {row, col}};
    }
}

static struct grid_range
range_union(struct grid_range a, struct grid_range b) {
    struct grid_point lo = point_cmp(a.start, b.start) <= 0 ? a.start : b.start;
    struct grid_point hi = point_cmp(a.end, b.end) >= 0 ? a.end : b.end;
    return (struct grid_range){lo, hi};
}

void selection_start(struct term *t, int col, int row, enum selection_kind kind) {
    struct grid_range r = range_for_point(t, row, col, kind);
    t->selection.kind = kind;
    t->selection.pivot = r;
    t->selection.coords = r;
    t->selection.active = true;
    t->selection.ongoing = true;
    t->selection_changed = true;
}

void selection_update(struct term *t, int col, int row) {
    if (!t->selection.ongoing)
        return;
    struct grid_range cur = range_for_point(t, row, col, t->selection.kind);
    struct grid_range next = range_union(t->selection.pivot, cur);
    if (point_cmp(next.start, t->selection.coords.start) != 0 ||
        point_cmp(next.end, t->selection.coords.end) != 0) {
        t->selection.coords = next;
        t->selection_changed = true;
    }
}

void selection_finish(struct term *t) {
    if (!t->selection.ongoing)
        return;
    t->selection.ongoing = false;
    if (t->selection.kind == SEL_CHAR &&
        point_cmp(t->selection.coords.start, t->selection.coords.end) == 0)
        t->selection.active = false;
    t->selection_changed = true;
}

void selection_clear(struct term *t) {
    if (!t->selection.active && !t->selection.ongoing)
        return;
    t->selection = (struct selection){0};
    t->selection_changed = true;
}

void selection_on_rows(struct term *t, int top, int bottom) {
    if (!t->selection.active && !t->selection.ongoing)
        return;
    if (t->selection.coords.start.row <= bottom && t->selection.coords.end.row >= top)
        selection_clear(t);
}

bool selection_row_span(const struct term *t, int row, int *from, int *to) {
    const struct selection *s = &t->selection;
    if (!s->active || row < s->coords.start.row || row > s->coords.end.row)
        return false;

    int f = row == s->coords.start.row ? s->coords.start.col : 0;
    int e = row == s->coords.end.row ? s->coords.end.col + 1 : t->cols;
    if (f >= e)
        return false;
    *from = f;
    *to = e;
    return true;
}

static int
utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

struct text_buf {
    char *data;
    size_t len, cap;
};

static void
tb_append(struct text_buf *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = MAX(b->cap * 2, b->len + n);
        b->data = xrealloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

char *
selection_to_text(const struct term *t, size_t *len) {
    if (!t->selection.active) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }

    /* grid_row() only allocates a row's cells on first touch; harmless
     * to cast away const for a read-only walk, as render_needed() does. */
    struct term *tm = (struct term *)t;
    const struct selection *s = &t->selection;

    struct text_buf tb = {0};
    for (int row = s->coords.start.row; row <= s->coords.end.row; row++) {
        int from = row == s->coords.start.row ? s->coords.start.col : 0;
        int to = row == s->coords.end.row ? s->coords.end.col + 1 : t->cols;

        struct row *r = grid_row(tm->grid, row);
        int empty_run = 0;
        for (int col = from; col < to; col++) {
            uint32_t cp = r->cells[col].cp;
            if (cp == CELL_SPACER)
                continue;
            if (cp == 0) {
                empty_run++;
                continue;
            }
            if (empty_run > 0) {
                tb_append(&tb, " ", 1);
                empty_run = 0;
            }
            const struct composed_chain *chain = composed_get(&t->composed, cp);
            int count = chain != NULL ? chain->count : 1;
            for (int i = 0; i < count; i++) {
                char enc[4];
                int n = utf8_encode(chain != NULL ? chain->cps[i] : cp, enc);
                tb_append(&tb, enc, (size_t)n);
            }
        }
        if (row < s->coords.end.row && !r->wrapped)
            tb_append(&tb, "\n", 1);
    }

    tb_append(&tb, "", 1); /* NUL terminator, not counted in *len */
    tb.len--;
    if (len != NULL)
        *len = tb.len;
    return tb.data;
}

void selection_scroll(struct term *t, int n) {
    if (!t->selection.active && !t->selection.ongoing)
        return;

    t->selection.pivot.start.row -= n;
    t->selection.pivot.end.row -= n;
    t->selection.coords.start.row -= n;
    t->selection.coords.end.row -= n;

    if (t->selection.coords.end.row < -t->grid->scrollback_used) {
        selection_clear(t);
        return;
    }
    t->selection_changed = true;
}
