#include <string.h>

#define LOG_MODULE "csi"
#include "core/util.h"
#include "term/term.h"

/* Parses an extended color (38/48/58) starting at params[i]. Returns the
 * index of the last parameter consumed; *color is 0 if the form is invalid. */
static int
parse_ext_color(const struct vt_csi *c, int i, uint32_t *color) {
    *color = 0;

    if (vt_param_is_sub(c, i + 1)) {
        /* colon form: 38:5:n, 38:2:r:g:b or 38:2:colorspace:r:g:b */
        int k = 0;
        while (vt_param_is_sub(c, i + 1 + k))
            k++;
        uint32_t kind = c->params[i + 1];
        if (kind == 5 && k >= 2)
            *color = COLOR_MAKE(COLOR_PALETTE, c->params[i + 2] & 0xff);
        else if (kind == 2 && k >= 4) {
            int base = k >= 5 ? i + 3 : i + 2;
            *color = COLOR_MAKE(COLOR_RGB, (c->params[base] & 0xff) << 16 |
                                               (c->params[base + 1] & 0xff) << 8 |
                                               (c->params[base + 2] & 0xff));
        }
        return i + k;
    }

    /* semicolon form: 38;5;n or 38;2;r;g;b */
    if (i + 1 >= c->nparams)
        return i;
    uint32_t kind = c->params[i + 1];
    if (kind == 5 && i + 2 < c->nparams) {
        *color = COLOR_MAKE(COLOR_PALETTE, c->params[i + 2] & 0xff);
        return i + 2;
    }
    if (kind == 2 && i + 4 < c->nparams) {
        *color = COLOR_MAKE(COLOR_RGB, (c->params[i + 2] & 0xff) << 16 |
                                           (c->params[i + 3] & 0xff) << 8 |
                                           (c->params[i + 4] & 0xff));
        return i + 4;
    }
    return c->nparams - 1; /* malformed: stop parsing */
}

static void
set_underline(struct pen *pen, uint32_t style) {
    pen->attrs = (pen->attrs & ~ATTR_UNDERLINE_MASK) |
                 ((MIN(style, 5u) << ATTR_UNDERLINE_SHIFT) & ATTR_UNDERLINE_MASK);
}

static void
sgr(struct term *t, const struct vt_csi *c) {
    struct pen *pen = &t->pen;

    if (c->nparams == 0) {
        *pen = (struct pen){0};
        return;
    }

    for (int i = 0; i < c->nparams; i++) {
        uint32_t p = c->params[i];
        uint32_t color;

        switch (p) {
        case 0:
            *pen = (struct pen){0};
            break;
        case 1:
            pen->attrs |= ATTR_BOLD;
            break;
        case 2:
            pen->attrs |= ATTR_DIM;
            break;
        case 3:
            pen->attrs |= ATTR_ITALIC;
            break;
        case 4:
            if (vt_param_is_sub(c, i + 1))
                set_underline(pen, c->params[++i]);
            else
                set_underline(pen, 1);
            break;
        case 5:
        case 6:
            pen->attrs |= ATTR_BLINK;
            break;
        case 7:
            pen->attrs |= ATTR_REVERSE;
            break;
        case 8:
            pen->attrs |= ATTR_INVISIBLE;
            break;
        case 9:
            pen->attrs |= ATTR_STRIKE;
            break;
        case 21:
            set_underline(pen, 2);
            break;
        case 22:
            pen->attrs &= ~(ATTR_BOLD | ATTR_DIM);
            break;
        case 23:
            pen->attrs &= ~ATTR_ITALIC;
            break;
        case 24:
            set_underline(pen, 0);
            break;
        case 25:
            pen->attrs &= ~ATTR_BLINK;
            break;
        case 27:
            pen->attrs &= ~ATTR_REVERSE;
            break;
        case 28:
            pen->attrs &= ~ATTR_INVISIBLE;
            break;
        case 29:
            pen->attrs &= ~ATTR_STRIKE;
            break;

        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            pen->fg = COLOR_MAKE(COLOR_PALETTE, p - 30);
            break;
        case 38:
            i = parse_ext_color(c, i, &color);
            if (color != 0)
                pen->fg = color;
            break;
        case 39:
            pen->fg = 0;
            break;

        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            pen->bg = COLOR_MAKE(COLOR_PALETTE, p - 40);
            break;
        case 48:
            i = parse_ext_color(c, i, &color);
            if (color != 0)
                pen->bg = color;
            break;
        case 49:
            pen->bg = 0;
            break;

        case 53:
            pen->attrs |= ATTR_OVERLINE;
            break;
        case 55:
            pen->attrs &= ~ATTR_OVERLINE;
            break;
        case 58:
            i = parse_ext_color(c, i, &color);
            if (color != 0)
                pen->ul = color;
            break;
        case 59:
            pen->ul = 0;
            break;

        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            pen->fg = COLOR_MAKE(COLOR_PALETTE, p - 90 + 8);
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            pen->bg = COLOR_MAKE(COLOR_PALETTE, p - 100 + 8);
            break;

        default:
            /* skip unknown sub-parameters along with their parent */
            while (vt_param_is_sub(c, i + 1))
                i++;
            LOG_DBG("unhandled SGR %u", p);
            break;
        }
    }
}

static void
decset(struct term *t, uint32_t mode, bool enable) {
    switch (mode) {
    case 1:
        t->modes.app_cursor_keys = enable;
        break;
    case 5:
        if (t->modes.reverse_video != enable) {
            t->modes.reverse_video = enable;
            grid_mark_all_dirty(t->grid);
        }
        break;
    case 6:
        t->modes.origin = enable;
        term_cursor_to(t, 0, 0);
        break;
    case 7:
        t->modes.autowrap = enable;
        break;
    case 12:
        t->cursor_blink = enable;
        t->cursor_dirty = true;
        break;
    case 25:
        t->modes.cursor_visible = enable;
        t->cursor_dirty = true;
        break;
    case 1000:
        if (enable)
            t->modes.mouse_mode = MOUSE_NORMAL;
        else if (t->modes.mouse_mode == MOUSE_NORMAL)
            t->modes.mouse_mode = MOUSE_OFF;
        break;
    case 1002:
        if (enable)
            t->modes.mouse_mode = MOUSE_BUTTON;
        else if (t->modes.mouse_mode == MOUSE_BUTTON)
            t->modes.mouse_mode = MOUSE_OFF;
        break;
    case 1003:
        if (enable)
            t->modes.mouse_mode = MOUSE_ANY;
        else if (t->modes.mouse_mode == MOUSE_ANY)
            t->modes.mouse_mode = MOUSE_OFF;
        break;
    case 1006:
        t->modes.mouse_sgr = enable;
        break;
    case 1007:
        t->modes.mouse_alt_scroll = enable;
        break;
    case 47:
        term_set_alt_screen(t, enable, false, false);
        break;
    case 1047:
        term_set_alt_screen(t, enable, false, !enable);
        break;
    case 1048:
        if (enable)
            term_save_cursor(t);
        else
            term_restore_cursor(t);
        break;
    case 1049:
        term_set_alt_screen(t, enable, true, true);
        break;
    case 1004:
        t->modes.focus_events = enable;
        break;
    case 2004:
        t->modes.bracketed_paste = enable;
        break;
    case 2026:
        t->modes.sync_updates = enable;
        break;
    default:
        LOG_DBG("unhandled DEC mode %u (%s)", mode, enable ? "set" : "reset");
        break;
    }
}

/* ---- kitty keyboard protocol (CSI ... u); see kitty_keys.h and input.c ---- */

static void
kitty_kbd_query(struct term *t) {
    term_reply(t, "\033[?%uu", t->grid->kitty_kbd.flags[t->grid->kitty_kbd.idx]);
}

/* Push is a ring buffer, not a bounded stack: a push past the end evicts
 * the oldest entry by wrapping the index back to 0. */
static void
kitty_kbd_push(struct term *t, uint32_t flags) {
    struct grid *g = t->grid;
    int idx = g->kitty_kbd.idx + 1 >= (int)ARRAY_LEN(g->kitty_kbd.flags) ? 0
                                                                         : g->kitty_kbd.idx + 1;
    g->kitty_kbd.flags[idx] = (uint8_t)(flags & KITTY_KBD_SUPPORTED);
    g->kitty_kbd.idx = idx;
}

static void
kitty_kbd_pop(struct term *t, uint32_t n) {
    struct grid *g = t->grid;
    int idx = g->kitty_kbd.idx;
    for (uint32_t i = 0; i < MIN(n, ARRAY_LEN(g->kitty_kbd.flags)); i++) {
        g->kitty_kbd.flags[idx] = 0; /* also covers over-popping past the base */
        idx = idx == 0 ? (int)ARRAY_LEN(g->kitty_kbd.flags) - 1 : idx - 1;
    }
    g->kitty_kbd.idx = idx;
}

static void
kitty_kbd_set(struct term *t, uint32_t flags, uint32_t mode) {
    uint8_t *cur = &t->grid->kitty_kbd.flags[t->grid->kitty_kbd.idx];
    flags &= KITTY_KBD_SUPPORTED;
    switch (mode) {
    case 2:
        *cur |= (uint8_t)flags;
        break;
    case 3:
        *cur &= (uint8_t)~flags;
        break;
    default:
        *cur = (uint8_t)flags;
        break;
    }
}

/* DECRQM status: 1 set, 2 reset, 0 not recognized */
static int
dec_mode_status(const struct term *t, uint32_t mode) {
    bool v;
    switch (mode) {
    case 1:
        v = t->modes.app_cursor_keys;
        break;
    case 5:
        v = t->modes.reverse_video;
        break;
    case 6:
        v = t->modes.origin;
        break;
    case 7:
        v = t->modes.autowrap;
        break;
    case 12:
        v = t->cursor_blink;
        break;
    case 25:
        v = t->modes.cursor_visible;
        break;
    case 47:
    case 1047:
    case 1049:
        v = t->modes.alt_screen;
        break;
    case 1000:
        v = t->modes.mouse_mode == MOUSE_NORMAL;
        break;
    case 1002:
        v = t->modes.mouse_mode == MOUSE_BUTTON;
        break;
    case 1003:
        v = t->modes.mouse_mode == MOUSE_ANY;
        break;
    case 1006:
        v = t->modes.mouse_sgr;
        break;
    case 1007:
        v = t->modes.mouse_alt_scroll;
        break;
    case 1004:
        v = t->modes.focus_events;
        break;
    case 2004:
        v = t->modes.bracketed_paste;
        break;
    case 2026:
        v = t->modes.sync_updates;
        break;
    default:
        return 0;
    }
    return v ? 1 : 2;
}

static int
ansi_mode_status(const struct term *t, uint32_t mode) {
    switch (mode) {
    case 4:
        return t->modes.insert ? 1 : 2;
    case 20:
        return t->modes.newline ? 1 : 2;
    default:
        return 0;
    }
}

static void
xtwinops(struct term *t, const struct vt_csi *c) {
    switch (vt_param(c, 0, 0)) {
    case 14:
        term_reply(t, "\033[4;%d;%dt", t->rows * t->cell_height, t->cols * t->cell_width);
        break;
    case 16:
        term_reply(t, "\033[6;%d;%dt", t->cell_height, t->cell_width);
        break;
    case 18:
        term_reply(t, "\033[8;%d;%dt", t->rows, t->cols);
        break;
    default:
        LOG_DBG("unhandled XTWINOPS %u", vt_param(c, 0, 0));
        break;
    }
}

static void
ansi_mode(struct term *t, uint32_t mode, bool enable) {
    switch (mode) {
    case 4:
        t->modes.insert = enable;
        break;
    case 20:
        t->modes.newline = enable;
        break;
    default:
        LOG_DBG("unhandled ANSI mode %u", mode);
        break;
    }
}

static void
decscusr(struct term *t, uint32_t style) {
    switch (style) {
    case 0:
        t->cursor_style = CURSOR_BAR;
        t->cursor_blink = true;
        break;
    case 1:
        t->cursor_style = CURSOR_BLOCK;
        t->cursor_blink = true;
        break;
    case 2:
        t->cursor_style = CURSOR_BLOCK;
        t->cursor_blink = false;
        break;
    case 3:
        t->cursor_style = CURSOR_UNDERLINE;
        t->cursor_blink = true;
        break;
    case 4:
        t->cursor_style = CURSOR_UNDERLINE;
        t->cursor_blink = false;
        break;
    case 5:
        t->cursor_style = CURSOR_BAR;
        t->cursor_blink = true;
        break;
    case 6:
        t->cursor_style = CURSOR_BAR;
        t->cursor_blink = false;
        break;
    }
    t->cursor_dirty = true;
}

static void
soft_reset(struct term *t) {
    t->modes.cursor_visible = true;
    t->modes.insert = false;
    t->modes.origin = false;
    t->modes.autowrap = true;
    t->modes.app_cursor_keys = false;
    t->modes.app_keypad = false;
    t->scroll_top = 0;
    t->scroll_bottom = t->rows - 1;
    memset(t->charsets, 'B', sizeof(t->charsets));
    t->gl = 0;
    t->pen = (struct pen){0};
    t->saved[t->modes.alt_screen] = (struct saved_cursor){.autowrap = true};
    memset(t->saved[t->modes.alt_screen].charsets, 'B', 4);
}

static void
csi_plain(struct term *t, const struct vt_csi *c) {
    int n = (int)vt_param(c, 0, 1);

    switch (c->final) {
    case '@':
        term_insert_chars(t, n);
        break;
    case 'A':
        term_cursor_up(t, n);
        break;
    case 'B':
        term_cursor_down(t, n);
        break;
    case 'C':
    case 'a':
        term_cursor_right(t, n);
        break;
    case 'D':
        term_cursor_left(t, n);
        break;
    case 'E':
        term_cursor_down(t, n);
        term_cursor_left(t, t->cols);
        break;
    case 'F':
        term_cursor_up(t, n);
        term_cursor_left(t, t->cols);
        break;
    case 'G':
    case '`': {
        int row = t->cursor.row - (t->modes.origin ? t->scroll_top : 0);
        term_cursor_to(t, row, n - 1);
        break;
    }
    case 'H':
    case 'f':
        term_cursor_to(t, (int)vt_param(c, 0, 1) - 1, (int)vt_param(c, 1, 1) - 1);
        break;
    case 'I':
        term_tab_forward(t, n);
        break;
    case 'J':
        term_erase_display(t, (int)vt_param(c, 0, 0));
        break;
    case 'K':
        term_erase_line(t, (int)vt_param(c, 0, 0));
        break;
    case 'L':
        term_insert_lines(t, n);
        break;
    case 'M':
        term_delete_lines(t, n);
        break;
    case 'P':
        term_delete_chars(t, n);
        break;
    case 'S':
        term_scroll_up(t, n);
        break;
    case 'T':
        if (c->nparams <= 1)
            term_scroll_down(t, n);
        break;
    case 'X':
        term_erase_chars(t, n);
        break;
    case 'Z':
        term_tab_backward(t, n);
        break;
    case 'b':
        if (t->last_cp != 0) {
            for (int i = 0; i < MIN(n, 65535); i++)
                term_print(t, t->last_cp);
        }
        break;
    case 'c':
        if (vt_param(c, 0, 0) == 0)
            term_reply(t, "\033[?62;22c");
        break;
    case 'd':
        term_cursor_to(t, n - 1, t->cursor.col);
        break;
    case 'e':
        term_cursor_down(t, n);
        break;
    case 'g':
        term_tab_clear(t, (int)vt_param(c, 0, 0));
        break;
    case 'h':
    case 'l':
        for (int i = 0; i < c->nparams; i++)
            ansi_mode(t, c->params[i], c->final == 'h');
        break;
    case 'm':
        sgr(t, c);
        break;
    case 'n':
        switch (vt_param(c, 0, 0)) {
        case 5:
            term_reply(t, "\033[0n");
            break;
        case 6: {
            int row = t->cursor.row - (t->modes.origin ? t->scroll_top : 0);
            term_reply(t, "\033[%d;%dR", row + 1, t->cursor.col + 1);
            break;
        }
        }
        break;
    case 'r':
        term_set_margins(t, (int)vt_param(c, 0, 1) - 1, (int)vt_param(c, 1, t->rows) - 1);
        break;
    case 's':
        term_save_cursor(t);
        break;
    case 'u':
        term_restore_cursor(t);
        break;
    case 't':
        xtwinops(t, c);
        break;
    default:
        LOG_DBG("unhandled CSI %c", c->final);
        break;
    }
}

static void
csi_private(struct term *t, const struct vt_csi *c) {
    switch (c->final) {
    case 'h':
    case 'l':
        for (int i = 0; i < c->nparams; i++)
            decset(t, c->params[i], c->final == 'h');
        break;
    case 'J':
        term_erase_display(t, (int)vt_param(c, 0, 0));
        break; /* DECSED */
    case 'K':
        term_erase_line(t, (int)vt_param(c, 0, 0));
        break; /* DECSEL */
    case 'n':
        if (vt_param(c, 0, 0) == 6) {
            int row = t->cursor.row - (t->modes.origin ? t->scroll_top : 0);
            term_reply(t, "\033[?%d;%dR", row + 1, t->cursor.col + 1);
        }
        break;
    case 'u':
        kitty_kbd_query(t);
        break;
    default:
        LOG_DBG("unhandled CSI ? %c", c->final);
        break;
    }
}

void term_csi(struct term *t, const struct vt_csi *c) {
    if (c->n_inter == 0) {
        switch (c->private_marker) {
        case 0:
            csi_plain(t, c);
            return;
        case '?':
            csi_private(t, c);
            return;
        case '>':
            if (c->final == 'c')
                term_reply(t, "\033[>1;10;0c");
            else if (c->final == 'q')
                term_reply(t, "\033P>|astralia-kitty\033\\");
            else if (c->final == 'u')
                kitty_kbd_push(t, vt_param(c, 0, 0));
            return;
        case '<':
            if (c->final == 'u')
                kitty_kbd_pop(t, vt_param(c, 0, 1));
            return;
        case '=':
            if (c->final == 'u')
                kitty_kbd_set(t, vt_param(c, 0, 0), vt_param(c, 1, 1));
            return;
        }
    } else if (c->n_inter == 1 && c->inter[0] == '$' && c->final == 'p') {
        /* DECRQM */
        uint32_t mode = vt_param(c, 0, 0);
        if (c->private_marker == '?')
            term_reply(t, "\033[?%u;%d$y", mode, dec_mode_status(t, mode));
        else if (c->private_marker == 0)
            term_reply(t, "\033[%u;%d$y", mode, ansi_mode_status(t, mode));
        return;
    } else if (c->n_inter == 1 && c->private_marker == 0) {
        if (c->inter[0] == ' ' && c->final == 'q') {
            decscusr(t, vt_param(c, 0, 0));
            return;
        }
        if (c->inter[0] == '!' && c->final == 'p') {
            soft_reset(t);
            return;
        }
    }
    LOG_DBG("unhandled CSI %c%.*s%c", c->private_marker ? c->private_marker : ' ',
            c->n_inter, (const char *)c->inter, c->final);
}
