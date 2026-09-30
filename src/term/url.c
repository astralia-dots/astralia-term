#include "term/url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "term/hyperlink.h"
#include "term/term.h"

#define MAX_WRAP_ROWS 8 /* rows joined above and below the pointer */

/* Same set main.c's open_url() allows, so every underlined URL can open */
static const char *const schemes[] = {"http://", "https://", "mailto:", "ftp://", "file://"};

static struct cell *
cell_at(struct term *t, int top, int i) {
    return &grid_row(t->grid, top + i / t->cols)->cells[i % t->cols];
}

static bool
url_char(uint32_t cp) {
    return cp > ' ' && cp < 0x7f && (isalnum((int)cp) || strchr("-._~:/?#[]@!$&'()*+,;=%", (int)cp));
}

static int
count(const char *s, int from, int to, char c) {
    int n = 0;
    for (int i = from; i < to; i++)
        n += s[i] == c;
    return n;
}

static size_t
scheme_at(const char *s) {
    for (size_t k = 0; k < ARRAY_LEN(schemes); k++) {
        size_t len = strlen(schemes[k]);
        if (strncmp(s, schemes[k], len) == 0)
            return len;
    }
    return 0;
}

/* Next plain-text URL at or after from in buf (non-URL characters are ' '),
 * as [*s, *e). Both hover and the always-on underline scan through this, so
 * they agree on every link's extent. */
static bool
next_text_url(const char *buf, int n, int from, int *s, int *e) {
    for (int i = from; i < n; i++) {
        if (buf[i] == ' ' || (i > 0 && isalnum((unsigned char)buf[i - 1])))
            continue;
        size_t scheme_len = scheme_at(buf + i);
        if (scheme_len == 0)
            continue;

        int end = i;
        while (end < n && buf[end] != ' ')
            end++;
        for (;;) {
            char last = buf[end - 1];
            if (strchr(".,;:!?'", last) != NULL ||
                (last == ')' && count(buf, i, end, ')') > count(buf, i, end, '(')) ||
                (last == ']' && count(buf, i, end, ']') > count(buf, i, end, '[')))
                end--;
            else
                break;
        }
        if (end - i > (int)scheme_len) {
            *s = i;
            *e = end;
            return true;
        }
    }
    return false;
}

/* The soft-wrapped line around row: first row and cell count. */
static int
line_bounds(struct term *t, int row, int *n) {
    struct grid *g = t->grid;
    int top = row, bottom = row;
    while (top > -g->scrollback_used && row - top < MAX_WRAP_ROWS && grid_row(g, top - 1)->wrapped)
        top--;
    while (bottom < t->rows - 1 && bottom - row < MAX_WRAP_ROWS && grid_row(g, bottom)->wrapped)
        bottom++;
    *n = (bottom - top + 1) * t->cols;
    return top;
}

/* The line as URL characters, or NULL if it can't hold a URL; caller frees. */
static char *
line_text(struct term *t, int top, int n) {
    char *buf = xmalloc(n + 1);
    bool colon = false;
    for (int i = 0; i < n; i++) {
        uint32_t cp = cell_at(t, top, i)->cp;
        buf[i] = url_char(cp) ? (char)cp : ' ';
        colon |= cp == ':';
    }
    buf[n] = '\0';
    if (!colon) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* The cursor's line on the normal screen stands in for the shell's
 * input area, so output still waiting for its newline loses its links too.
 * OSC 133;B/C marks would make it exact. */
static bool
typing_line(const struct term *t, int top, int n) {
    int row = t->cursor.row;
    return !t->modes.alt_screen && row >= top && row < top + n / t->cols;
}

void url_mark_line_dirty(struct term *t, int row) {
    int n;
    int top = line_bounds(t, row, &n);
    for (int r = top; r < top + n / t->cols; r++)
        grid_row(t->grid, r)->dirty = true;
}

bool url_at(struct term *t, int row, int col, struct grid_range *range, char **uri) {
    int n;
    int top = line_bounds(t, row, &n);
    if (typing_line(t, top, n))
        return false;
    int p = (row - top) * t->cols + col;

    int s, e;
    uint16_t id = cell_link(cell_at(t, top, p));
    const struct hyperlink *link = hyperlink_get(&t->hyperlinks, id);
    if (link != NULL) {
        for (s = p; s > 0 && cell_link(cell_at(t, top, s - 1)) == id; s--)
            ;
        for (e = p + 1; e < n && cell_link(cell_at(t, top, e)) == id; e++)
            ;
        if (uri != NULL)
            *uri = xstrdup(link->uri);
    } else {
        char *buf = line_text(t, top, n);
        if (buf == NULL)
            return false;
        bool found = false;
        for (int from = 0; !found && next_text_url(buf, n, from, &s, &e) && s <= p; from = e)
            found = p < e;
        if (found && uri != NULL) {
            buf[e] = '\0';
            *uri = xstrdup(buf + s);
        }
        free(buf);
        if (!found)
            return false;
    }

    range->start = (struct grid_point){top + s / t->cols, s % t->cols};
    range->end = (struct grid_point){top + (e - 1) / t->cols, (e - 1) % t->cols};
    return true;
}

void url_row_links(struct term *t, int row, bool *mask) {
    int n;
    int top = line_bounds(t, row, &n);
    bool typing = typing_line(t, top, n);
    const struct cell *cells = grid_row(t->grid, row)->cells;
    for (int col = 0; col < t->cols; col++)
        mask[col] = !typing && cell_link(&cells[col]) != 0;
    if (typing)
        return;

    /* Rescans the whole wrapped line for each of its rows; cache
     * per-line spans if that shows up. */
    char *buf = line_text(t, top, n);
    if (buf == NULL)
        return;
    int row_from = (row - top) * t->cols, row_to = row_from + t->cols;
    int s, e;
    for (int from = 0; next_text_url(buf, n, from, &s, &e) && s < row_to; from = e) {
        for (int i = MAX(s, row_from); i < MIN(e, row_to); i++)
            mask[i - row_from] = true;
    }
    free(buf);
}

static void
mark_dirty(struct term *t, const struct grid_range *r) {
    int from = MAX(r->start.row, -t->grid->scrollback_used);
    int to = MIN(r->end.row, t->rows - 1);
    for (int row = from; row <= to; row++)
        grid_row(t->grid, row)->dirty = true;
}

void url_hover(struct term *t, int row, int col) {
    struct grid_range r;
    bool on = row >= 0 && row < t->rows && col >= 0 && col < t->cols &&
              url_at(t, row - t->view_offset, col, &r, NULL);

    if (on == t->link_hover.active &&
        (!on || memcmp(&r, &t->link_hover.range, sizeof(r)) == 0))
        return;
    if (t->link_hover.active)
        mark_dirty(t, &t->link_hover.range);
    t->link_hover.active = on;
    if (on) {
        t->link_hover.range = r;
        mark_dirty(t, &r);
    }
}
