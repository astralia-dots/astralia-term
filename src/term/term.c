#include "term/term.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "term"
#include "core/util.h"
#include "term/sixel.h"

/* DEC special graphics, 0x5f..0x7e */
static const uint16_t dec_special[] = {
    0x00a0,
    0x25c6,
    0x2592,
    0x2409,
    0x240c,
    0x240d,
    0x240a,
    0x00b0,
    0x00b1,
    0x2424,
    0x240b,
    0x2518,
    0x2510,
    0x250c,
    0x2514,
    0x253c,
    0x23ba,
    0x23bb,
    0x2500,
    0x23bc,
    0x23bd,
    0x251c,
    0x2524,
    0x2534,
    0x252c,
    0x2502,
    0x2264,
    0x2265,
    0x03c0,
    0x2260,
    0x00a3,
    0x00b7,
};

/* Initial 16 colors */
static const uint32_t default_colors16[16] = {
    0x222222,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,
    0x666666,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xfcace3,
    0xb3ffff,
    0xffffff,
};

static void
init_initial_colors(struct term *t) {
    memcpy(t->initial_palette, default_colors16, sizeof(default_colors16));

    static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++) {
        uint32_t r = levels[i / 36], g = levels[(i / 6) % 6], b = levels[i % 6];
        t->initial_palette[16 + i] = r << 16 | g << 8 | b;
    }
    for (int i = 0; i < 24; i++) {
        uint32_t v = 8 + 10 * i;
        t->initial_palette[232 + i] = v << 16 | v << 8 | v;
    }

    t->initial_fg = 0xdcdccc;
    t->initial_bg = 0x000000;
}

static void
reset_colors(struct term *t) {
    memcpy(t->palette, t->initial_palette, sizeof(t->palette));
    t->default_fg = t->initial_fg;
    t->default_bg = t->initial_bg;
    t->cursor_color_set = false;
    t->colors_changed = true;
}

static void
reset_tabs(struct term *t) {
    for (int i = 0; i < t->cols; i++)
        t->tabs[i] = i % 8 == 0 && i != 0;
}

static void
reset_state(struct term *t) {
    t->cursor = (struct cursor){0};
    t->pen = (struct pen){0};
    t->scroll_top = 0;
    t->scroll_bottom = t->rows - 1;
    memset(&t->modes, 0, sizeof(t->modes));
    t->modes.autowrap = true;
    t->modes.cursor_visible = true;
    memset(t->charsets, 'B', sizeof(t->charsets));
    t->gl = 0;
    t->last_cp = 0;
    t->cursor_style = CURSOR_BAR;
    t->cursor_blink = true;
    for (int i = 0; i < 2; i++) {
        t->saved[i] = (struct saved_cursor){.autowrap = true};
        memset(t->saved[i].charsets, 'B', sizeof(t->saved[i].charsets));
    }
    reset_tabs(t);
    reset_colors(t);
    t->view_offset = 0;
    t->view_changed = true;
    t->cursor_dirty = true;
}

/* ---- parser glue ---- */

static void term_execute(struct term *t, uint8_t c);
static void term_esc(struct term *t, const uint8_t *inter, int n_inter, uint8_t final);

static void
cb_print(void *user, uint32_t cp) {
    term_print(user, cp);
}

static void
cb_print_ascii(void *user, const uint8_t *s, size_t len) {
    struct term *t = user;
    for (size_t i = 0; i < len; i++)
        term_print(t, s[i]);
}

static void
cb_execute(void *user, uint8_t c) {
    term_execute(user, c);
}

static void
cb_csi(void *user, const struct vt_csi *csi) {
    term_csi(user, csi);
}

static void
cb_esc(void *user, const uint8_t *inter, int n_inter, uint8_t final) {
    term_esc(user, inter, n_inter, final);
}

static void
cb_osc(void *user, const uint8_t *data, size_t len, bool bel) {
    term_osc(user, data, len, bel);
}

static void
cb_apc(void *user, const uint8_t *data, size_t len) {
    term_apc(user, data, len);
}

static void
cb_dcs(void *user, uint8_t private_marker, const uint32_t *params, int nparams, uint8_t final,
       const uint8_t *data, size_t len) {
    term_dcs(user, private_marker, params, nparams, final, data, len);
}

void term_init(struct term *t, int cols, int rows, const struct term_host *host, void *user) {
    *t = (struct term){
        .cols = cols,
        .rows = rows,
        .cell_width = 1,
        .cell_height = 1,
        .host = host,
        .host_user = user,
    };
    grid_init(&t->normal, cols, rows, TERM_SCROLLBACK_DEFAULT);
    grid_init(&t->alt, cols, rows, 0);
    t->grid = &t->normal;
    t->tabs = xcalloc(cols, sizeof(t->tabs[0]));
    composed_init(&t->composed);
    hyperlink_init(&t->hyperlinks);
    graphics_init(&t->graphics);
    init_initial_colors(t);
    reset_state(t);

    static const struct vt_callbacks cbs = {
        .print = cb_print,
        .print_ascii = cb_print_ascii,
        .execute = cb_execute,
        .csi = cb_csi,
        .esc = cb_esc,
        .osc = cb_osc,
        .apc = cb_apc,
        .dcs = cb_dcs,
    };
    vt_parser_init(&t->parser, &cbs, t);
}

void term_destroy(struct term *t) {
    vt_parser_destroy(&t->parser);
    grid_free(&t->normal);
    grid_free(&t->alt);
    composed_destroy(&t->composed);
    hyperlink_destroy(&t->hyperlinks);
    graphics_destroy(&t->graphics);
    free(t->tabs);
    t->tabs = NULL;
}

void term_set_cell_size(struct term *t, int width, int height) {
    t->cell_width = width;
    t->cell_height = height;
}

void term_scroll_view(struct term *t, int delta) {
    int max = t->modes.alt_screen ? 0 : t->grid->scrollback_used;
    int offset = CLAMP(t->view_offset + delta, 0, max);
    if (offset == t->view_offset)
        return;
    t->view_offset = offset;
    t->view_changed = true;
}

void term_scroll_view_reset(struct term *t) {
    term_scroll_view(t, -t->view_offset);
}

struct row *
term_view_row(struct term *t, int r) {
    return grid_row(t->grid, r - t->view_offset);
}

void term_feed(struct term *t, const uint8_t *data, size_t len) {
    vt_parser_feed(&t->parser, data, len);
}

void term_reset(struct term *t) {
    t->grid = &t->normal;
    struct cell blank = {0};
    for (int r = 0; r < t->rows; r++) {
        grid_row_fill(grid_row(&t->normal, r), 0, t->cols, blank);
        grid_row_fill(grid_row(&t->alt, r), 0, t->cols, blank);
    }
    t->normal.scrollback_used = 0;
    memset(&t->normal.kitty_kbd, 0, sizeof(t->normal.kitty_kbd));
    memset(&t->alt.kitty_kbd, 0, sizeof(t->alt.kitty_kbd));
    composed_clear(&t->composed);
    hyperlink_clear(&t->hyperlinks);
    t->link = 0;
    graphics_clear(&t->graphics);
    t->link_hover.active = false;
    selection_clear(t);
    reset_state(t);
}

static struct grid_point
to_point(const struct cursor *c) {
    return (struct grid_point){c->row, c->col};
}

static void
from_point(struct cursor *c, struct grid_point p) {
    c->row = p.row;
    c->col = p.col;
    c->wrap_pending = false;
}

void term_resize(struct term *t, int cols, int rows) {
    if (cols == t->cols && rows == t->rows)
        return;

    /* A drag in flight isn't tracked through reflow; cancel it outright. */
    if (t->selection.ongoing)
        selection_clear(t);
    bool sel_active = t->selection.active;

    /* The live cursor belongs to the active screen; the saved cursor of
     * the normal screen is what 1049 restores, so it follows the text too. */
    bool alt = t->modes.alt_screen;
    struct grid_point normal_pts[4] = {to_point(alt ? &t->saved[0].cursor : &t->cursor),
                                       to_point(&t->saved[0].cursor)};
    struct grid_point alt_pts[4] = {to_point(alt ? &t->cursor : &t->saved[1].cursor),
                                    to_point(&t->saved[1].cursor)};
    int normal_npts = alt ? 1 : 2;
    int alt_npts = alt ? 2 : 1;

    struct grid_point *sel_pts = alt ? alt_pts : normal_pts;
    int *sel_npts = alt ? &alt_npts : &normal_npts;
    if (sel_active) {
        sel_pts[*sel_npts] = t->selection.coords.start;
        sel_pts[*sel_npts + 1] = t->selection.coords.end;
        *sel_npts += 2;
    }

    grid_resize(&t->normal, cols, rows, true, normal_pts, normal_npts);
    grid_resize(&t->alt, cols, rows, false, alt_pts, alt_npts);

    if (sel_active) {
        t->selection.coords.start = sel_pts[*sel_npts - 2];
        t->selection.coords.end = sel_pts[*sel_npts - 1];
        t->selection_changed = true;
    }

    if (alt) {
        from_point(&t->cursor, alt_pts[0]);
        from_point(&t->saved[1].cursor, alt_pts[1]);
        from_point(&t->saved[0].cursor, normal_pts[0]);
    } else {
        from_point(&t->cursor, normal_pts[0]);
        from_point(&t->saved[0].cursor, normal_pts[1]);
        from_point(&t->saved[1].cursor, alt_pts[0]);
    }

    t->cols = cols;
    t->rows = rows;
    t->view_offset = MIN(t->view_offset, t->grid->scrollback_used);
    t->view_changed = true;

    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;

    t->tabs = xrealloc(t->tabs, cols * sizeof(t->tabs[0]));
    reset_tabs(t);
    t->cursor_dirty = true;
}

static void
reply_va(struct term *t, const char *suffix, const char *fmt, va_list ap) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0 || t->host == NULL || t->host->write == NULL)
        return;
    size_t len = MIN((size_t)n, sizeof(buf) - 1);
    t->host->write(t->host_user, buf, len);
    if (suffix != NULL)
        t->host->write(t->host_user, suffix, strlen(suffix));
}

void term_reply(struct term *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_va(t, NULL, fmt, ap);
    va_end(ap);
}

void term_reply_st(struct term *t, bool bel, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_va(t, bel ? "\a" : "\033\\", fmt, ap);
    va_end(ap);
}

void term_reply_apc(struct term *t, const char *fmt, ...) {
    if (t->host == NULL || t->host->write == NULL)
        return;
    t->host->write(t->host_user, "\033_G", 3);
    va_list ap;
    va_start(ap, fmt);
    reply_va(t, "\033\\", fmt, ap);
    va_end(ap);
}

struct cell
term_blank(const struct term *t) {
    return (struct cell){.bg = t->pen.bg};
}

static inline struct row *
cur_row(struct term *t) {
    return grid_row(t->grid, t->cursor.row);
}

/* ---- cursor movement ---- */

static inline void
set_cursor(struct term *t, int row, int col) {
    t->cursor.row = row;
    t->cursor.col = col;
    t->cursor.wrap_pending = false;
    t->cursor_dirty = true;
}

void term_cursor_to(struct term *t, int row, int col) {
    if (t->modes.origin) {
        row = CLAMP(row + t->scroll_top, t->scroll_top, t->scroll_bottom);
    } else
        row = CLAMP(row, 0, t->rows - 1);
    set_cursor(t, row, CLAMP(col, 0, t->cols - 1));
}

void term_cursor_left(struct term *t, int n) {
    set_cursor(t, t->cursor.row, MAX(t->cursor.col - n, 0));
}

void term_cursor_right(struct term *t, int n) {
    set_cursor(t, t->cursor.row, MIN(t->cursor.col + n, t->cols - 1));
}

void term_cursor_up(struct term *t, int n) {
    int limit = t->cursor.row >= t->scroll_top ? t->scroll_top : 0;
    set_cursor(t, MAX(t->cursor.row - n, limit), t->cursor.col);
}

void term_cursor_down(struct term *t, int n) {
    int limit = t->cursor.row <= t->scroll_bottom ? t->scroll_bottom : t->rows - 1;
    set_cursor(t, MIN(t->cursor.row + n, limit), t->cursor.col);
}

/* Scrolls the margins up; a scrolled-back view keeps showing the same text. */
static void
scroll_region_up(struct term *t, int n) {
    int pushed = grid_scroll_up(t->grid, t->scroll_top, t->scroll_bottom, n, term_blank(t));
    if (pushed == 0)
        selection_on_rows(t, t->scroll_top, t->scroll_bottom);
    else {
        selection_scroll(t, pushed);
        if (t->view_offset > 0) {
            t->view_offset = MIN(t->view_offset + pushed, t->grid->scrollback_used);
            t->view_changed = true;
        }
    }
}

void term_index(struct term *t) {
    if (t->cursor.row == t->scroll_bottom)
        scroll_region_up(t, 1);
    else if (t->cursor.row < t->rows - 1)
        t->cursor.row++;
    t->cursor.wrap_pending = false;
    t->cursor_dirty = true;
}

static void
term_reverse_index(struct term *t) {
    if (t->cursor.row == t->scroll_top)
        term_scroll_down(t, 1);
    else if (t->cursor.row > 0)
        t->cursor.row--;
    t->cursor.wrap_pending = false;
    t->cursor_dirty = true;
}

/* ---- erasing and editing ---- */

void term_erase_display(struct term *t, int mode) {
    struct cell blank = term_blank(t);
    switch (mode) {
    case 0:
        selection_on_rows(t, t->cursor.row, t->rows - 1);
        grid_row_fill(cur_row(t), t->cursor.col, t->cols, blank);
        for (int r = t->cursor.row + 1; r < t->rows; r++)
            grid_row_fill(grid_row(t->grid, r), 0, t->cols, blank);
        break;
    case 1:
        selection_on_rows(t, 0, t->cursor.row);
        for (int r = 0; r < t->cursor.row; r++)
            grid_row_fill(grid_row(t->grid, r), 0, t->cols, blank);
        grid_row_fill(cur_row(t), 0, t->cursor.col + 1, blank);
        break;
    case 2:
        selection_on_rows(t, 0, t->rows - 1);
        for (int r = 0; r < t->rows; r++)
            grid_row_fill(grid_row(t->grid, r), 0, t->cols, blank);
        break;
    case 3:
        selection_on_rows(t, -t->grid->scrollback_used, -1);
        t->grid->scrollback_used = 0;
        term_scroll_view_reset(t);
        break;
    }
    t->cursor.wrap_pending = false;
}

void term_erase_line(struct term *t, int mode) {
    struct cell blank = term_blank(t);
    struct row *row = cur_row(t);
    selection_on_rows(t, t->cursor.row, t->cursor.row);
    switch (mode) {
    case 0:
        grid_row_fill(row, t->cursor.col, t->cols, blank);
        break;
    case 1:
        grid_row_fill(row, 0, t->cursor.col + 1, blank);
        break;
    case 2:
        grid_row_fill(row, 0, t->cols, blank);
        break;
    }
    if (mode != 1)
        row->wrapped = false;
    t->cursor.wrap_pending = false;
}

void term_erase_chars(struct term *t, int n) {
    int end = MIN(t->cursor.col + n, t->cols);
    selection_on_rows(t, t->cursor.row, t->cursor.row);
    grid_row_fill(cur_row(t), t->cursor.col, end, term_blank(t));
    t->cursor.wrap_pending = false;
}

void term_insert_chars(struct term *t, int n) {
    struct row *row = cur_row(t);
    int col = t->cursor.col;
    n = MIN(n, t->cols - col);
    selection_on_rows(t, t->cursor.row, t->cursor.row);
    memmove(&row->cells[col + n], &row->cells[col],
            (size_t)(t->cols - col - n) * sizeof(struct cell));
    grid_row_fill(row, col, col + n, term_blank(t));
    t->cursor.wrap_pending = false;
}

void term_delete_chars(struct term *t, int n) {
    struct row *row = cur_row(t);
    int col = t->cursor.col;
    n = MIN(n, t->cols - col);
    selection_on_rows(t, t->cursor.row, t->cursor.row);
    memmove(&row->cells[col], &row->cells[col + n],
            (size_t)(t->cols - col - n) * sizeof(struct cell));
    grid_row_fill(row, t->cols - n, t->cols, term_blank(t));
    t->cursor.wrap_pending = false;
}

void term_insert_lines(struct term *t, int n) {
    if (t->cursor.row < t->scroll_top || t->cursor.row > t->scroll_bottom)
        return;
    selection_on_rows(t, t->cursor.row, t->scroll_bottom);
    grid_scroll_down(t->grid, t->cursor.row, t->scroll_bottom, n, term_blank(t));
    set_cursor(t, t->cursor.row, 0);
}

void term_delete_lines(struct term *t, int n) {
    if (t->cursor.row < t->scroll_top || t->cursor.row > t->scroll_bottom)
        return;
    selection_on_rows(t, t->cursor.row, t->scroll_bottom);
    grid_scroll_up(t->grid, t->cursor.row, t->scroll_bottom, n, term_blank(t));
    set_cursor(t, t->cursor.row, 0);
}

void term_scroll_up(struct term *t, int n) {
    scroll_region_up(t, n);
}

void term_scroll_down(struct term *t, int n) {
    selection_on_rows(t, t->scroll_top, t->scroll_bottom);
    grid_scroll_down(t->grid, t->scroll_top, t->scroll_bottom, n, term_blank(t));
}

void term_set_margins(struct term *t, int top, int bottom) {
    top = CLAMP(top, 0, t->rows - 1);
    bottom = CLAMP(bottom, 0, t->rows - 1);
    if (top >= bottom)
        return;
    t->scroll_top = top;
    t->scroll_bottom = bottom;
    term_cursor_to(t, 0, 0);
}

/* ---- tabs ---- */

void term_tab_forward(struct term *t, int n) {
    int col = t->cursor.col;
    while (n-- > 0) {
        col++;
        while (col < t->cols - 1 && !t->tabs[col])
            col++;
        if (col >= t->cols - 1) {
            col = t->cols - 1;
            break;
        }
    }
    set_cursor(t, t->cursor.row, col);
}

void term_tab_backward(struct term *t, int n) {
    int col = t->cursor.col;
    while (n-- > 0 && col > 0) {
        col--;
        while (col > 0 && !t->tabs[col])
            col--;
    }
    set_cursor(t, t->cursor.row, col);
}

void term_tab_clear(struct term *t, int mode) {
    if (mode == 0)
        t->tabs[t->cursor.col] = false;
    else if (mode == 3)
        memset(t->tabs, 0, t->cols * sizeof(t->tabs[0]));
}

/* ---- printing ---- */

/* Writing w cells at col may split a wide character; blank the orphan. */
static void
fix_wide_overwrite(struct term *t, struct cell *cells, int col, int w) {
    if (cells[col].cp == CELL_SPACER && col > 0)
        cells[col - 1].cp = 0;
    int last = col + w - 1;
    if (last + 1 < t->cols && cells[last + 1].cp == CELL_SPACER)
        cells[last + 1].cp = 0;
}

static void
wrap_line(struct term *t) {
    cur_row(t)->wrapped = true;
    t->cursor.col = 0;
    term_index(t);
}

/* Zero-width codepoints join the character written last. */
static void
attach_combining(struct term *t, uint32_t cp) {
    int col = t->cursor.wrap_pending ? t->cursor.col : t->cursor.col - 1;
    if (col < 0)
        return;

    struct row *row = cur_row(t);
    if (row->cells[col].cp == CELL_SPACER && col > 0)
        col--;
    struct cell *c = &row->cells[col];
    if (c->cp == 0 || c->cp == CELL_SPACER)
        return;

    uint32_t composed = composed_add(&t->composed, c->cp, cp);
    if (composed != 0) {
        selection_on_rows(t, t->cursor.row, t->cursor.row);
        c->cp = composed;
        row->dirty = true;
    }
}

void term_print(struct term *t, uint32_t cp) {
    if (cp >= 0x5f && cp <= 0x7e && t->charsets[t->gl] == '0')
        cp = dec_special[cp - 0x5f];

    int w = cp_width(cp);
    if (w == 0) {
        attach_combining(t, cp);
        return;
    }

    if (t->cursor.wrap_pending) {
        if (t->modes.autowrap)
            wrap_line(t);
        t->cursor.wrap_pending = false;
    }

    if (w == 2 && t->cursor.col == t->cols - 1) {
        if (t->cols < 2)
            return;
        if (t->modes.autowrap) {
            selection_on_rows(t, t->cursor.row, t->cursor.row);
            grid_row_fill(cur_row(t), t->cursor.col, t->cols, term_blank(t));
            wrap_line(t);
        } else
            t->cursor.col = t->cols - 2;
    }

    if (t->modes.insert)
        term_insert_chars(t, w);

    selection_on_rows(t, t->cursor.row, t->cursor.row);
    struct row *row = cur_row(t);
    struct cell *cells = row->cells;
    int col = t->cursor.col;

    fix_wide_overwrite(t, cells, col, w);
    cells[col] = (struct cell){
        .cp = cp, .fg = t->pen.fg, .bg = t->pen.bg, .ul = t->pen.ul, .attrs = t->pen.attrs};
    cell_set_link(&cells[col], t->link);
    if (w == 2) {
        cells[col + 1] = cells[col];
        cells[col + 1].cp = CELL_SPACER;
    }
    row->dirty = true;
    t->last_cp = cp;

    col += w;
    if (col >= t->cols) {
        t->cursor.col = t->cols - 1;
        t->cursor.wrap_pending = t->modes.autowrap;
    } else
        t->cursor.col = col;
    t->cursor_dirty = true;
}

/* ---- cursor save/restore, alt screen ---- */

void term_save_cursor(struct term *t) {
    struct saved_cursor *s = &t->saved[t->modes.alt_screen];
    s->cursor = t->cursor;
    s->pen = t->pen;
    s->origin_mode = t->modes.origin;
    s->autowrap = t->modes.autowrap;
    memcpy(s->charsets, t->charsets, sizeof(s->charsets));
    s->gl = t->gl;
}

void term_restore_cursor(struct term *t) {
    const struct saved_cursor *s = &t->saved[t->modes.alt_screen];
    t->cursor = s->cursor;
    t->cursor.row = CLAMP(t->cursor.row, 0, t->rows - 1);
    t->cursor.col = CLAMP(t->cursor.col, 0, t->cols - 1);
    t->pen = s->pen;
    t->modes.origin = s->origin_mode;
    t->modes.autowrap = s->autowrap;
    memcpy(t->charsets, s->charsets, sizeof(t->charsets));
    t->gl = s->gl;
    t->cursor_dirty = true;
}

void term_set_alt_screen(struct term *t, bool enable, bool save_cursor, bool clear) {
    if (enable == t->modes.alt_screen)
        return;

    term_scroll_view_reset(t);
    selection_clear(t);

    if (enable) {
        if (save_cursor)
            term_save_cursor(t);
        t->modes.alt_screen = true;
        t->grid = &t->alt;
        if (clear) {
            struct cell blank = term_blank(t);
            for (int r = 0; r < t->rows; r++)
                grid_row_fill(grid_row(t->grid, r), 0, t->cols, blank);
        }
    } else {
        if (clear) {
            for (int r = 0; r < t->rows; r++)
                grid_row_fill(grid_row(&t->alt, r), 0, t->cols, (struct cell){0});
        }
        t->modes.alt_screen = false;
        t->grid = &t->normal;
        if (save_cursor)
            term_restore_cursor(t);
    }
    grid_mark_all_dirty(t->grid);
    t->cursor_dirty = true;
}

/* ---- C0 and ESC ---- */

static void
term_execute(struct term *t, uint8_t c) {
    switch (c) {
    case '\a':
        if (t->host != NULL && t->host->bell != NULL)
            t->host->bell(t->host_user);
        break;
    case '\b':
        if (t->cursor.col > 0)
            set_cursor(t, t->cursor.row, t->cursor.col - 1);
        else
            t->cursor.wrap_pending = false;
        break;
    case '\t':
        term_tab_forward(t, 1);
        break;
    case '\n':
    case '\v':
    case '\f':
        term_index(t);
        if (t->modes.newline)
            t->cursor.col = 0;
        break;
    case '\r':
        set_cursor(t, t->cursor.row, 0);
        break;
    case 0x0e: /* SO */
        t->gl = 1;
        break;
    case 0x0f: /* SI */
        t->gl = 0;
        break;
    }
}

static void
term_esc(struct term *t, const uint8_t *inter, int n_inter, uint8_t final) {
    if (n_inter == 0) {
        switch (final) {
        case '7':
            term_save_cursor(t);
            break;
        case '8':
            term_restore_cursor(t);
            break;
        case 'D':
            term_index(t);
            break;
        case 'E':
            t->cursor.col = 0;
            term_index(t);
            break;
        case 'H':
            t->tabs[t->cursor.col] = true;
            break;
        case 'M':
            term_reverse_index(t);
            break;
        case 'c':
            term_reset(t);
            break;
        case '=':
            t->modes.app_keypad = true;
            break;
        case '>':
            t->modes.app_keypad = false;
            break;
        case '\\':
            break; /* ST, already handled by the parser */
        default:
            LOG_DBG("unhandled ESC %c", final);
            break;
        }
        return;
    }

    if (n_inter == 1) {
        switch (inter[0]) {
        case '(':
        case ')':
        case '*':
        case '+':
            t->charsets[inter[0] - '('] = final == '0' ? '0' : 'B';
            return;
        case '#':
            if (final == '8') { /* DECALN */
                struct cell e = {.cp = 'E'};
                selection_on_rows(t, 0, t->rows - 1);
                for (int r = 0; r < t->rows; r++)
                    grid_row_fill(grid_row(t->grid, r), 0, t->cols, e);
                t->scroll_top = 0;
                t->scroll_bottom = t->rows - 1;
                set_cursor(t, 0, 0);
            }
            return;
        }
    }
    LOG_DBG("unhandled ESC %.*s%c", n_inter, (const char *)inter, final);
}

/* ---- DCS ---- */

/* Sixel is the only DCS shape in scope: no private marker, final 'q'
 * (its own "Pi;Pa;Pv q" header params -- macro id/aspect ratio/background
 * selection -- are accepted by the parser but not acted on here, matching
 * this file's existing scope). Every other DCS use (DECRQSS, tmux
 * passthrough, ReGIS, ...) is a no-op, preserving the pre-3.3 discard
 * behavior. */
void term_dcs(struct term *t, uint8_t private_marker, const uint32_t *params, int nparams,
              uint8_t final, const uint8_t *data, size_t len) {
    (void)params;
    (void)nparams;
    if (private_marker != 0 || final != 'q')
        return;

    uint8_t *rgba;
    int width, height;
    if (!sixel_decode(data, len, &rgba, &width, &height))
        return;

    size_t bytes = (size_t)width * height * 4;
    if (bytes > GRAPHICS_QUOTA_BYTES) {
        free(rgba);
        return;
    }

    uint32_t id = graphics_store_insert(&t->graphics, rgba, width, height);
    int cols = CLAMP((width + t->cell_width - 1) / t->cell_width, 1, 255);
    int rows = CLAMP((height + t->cell_height - 1) / t->cell_height, 1, 255);
    graphics_place_nonvirtual(t, id, 0, cols, rows, (struct graphics_src_rect){0}, true);
}
