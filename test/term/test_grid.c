/* Terminal + grid behavior, driven through escape sequences. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "term/selection.h"
#include "term/term.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static char reply[256];

static void
on_write(void *user, const void *data, size_t len) {
    size_t cur = strlen(reply);
    if (cur + len < sizeof(reply)) {
        memcpy(reply + cur, data, len);
        reply[cur + len] = '\0';
    }
}

static const struct term_host host = {.write = on_write};

static void
feed(struct term *t, const char *s) {
    term_feed(t, (const uint8_t *)s, strlen(s));
}

/* A row as ASCII: empty cells as ' ', spacers as '_', non-ASCII as '?' */
static const char *
cells_text(const struct row *row, int cols) {
    static char buf[256];
    for (int c = 0; c < cols; c++) {
        uint32_t cp = row->cells[c].cp;
        buf[c] = cp == 0 ? ' ' : cp == CELL_SPACER ? '_'
                             : cp < 0x80           ? (char)cp
                                                   : '?';
    }
    buf[cols] = '\0';
    return buf;
}

static const char *
row_text(struct term *t, int r) {
    return cells_text(grid_row(t->grid, r), t->cols);
}

static const char *
view_text(struct term *t, int r) {
    return cells_text(term_view_row(t, r), t->cols);
}

#define CHECK_ROW(t, r, expected)                                                \
    do {                                                                         \
        const char *got_ = row_text(t, r);                                       \
        if (strcmp(got_, expected) != 0) {                                       \
            fprintf(stderr, "%s:%d: row %d\n  expected [%s]\n  got      [%s]\n", \
                    __FILE__, __LINE__, r, expected, got_);                      \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_CURSOR(t, r, c)                                                    \
    do {                                                                         \
        if ((t)->cursor.row != (r) || (t)->cursor.col != (c)) {                  \
            fprintf(stderr, "%s:%d: cursor at %d,%d, expected %d,%d\n",          \
                    __FILE__, __LINE__, (t)->cursor.row, (t)->cursor.col, r, c); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void
test_width(void) {
    CHECK(cp_width('a') == 1);
    CHECK(cp_width('\t') == 0);
    CHECK(cp_width(0x00e9) == 1);  /* é */
    CHECK(cp_width(0x0301) == 0);  /* combining acute */
    CHECK(cp_width(0x200b) == 0);  /* zero width space */
    CHECK(cp_width(0x00ad) == 1);  /* soft hyphen */
    CHECK(cp_width(0x4e2d) == 2);  /* 中 */
    CHECK(cp_width(0xff21) == 2);  /* fullwidth A */
    CHECK(cp_width(0x1f600) == 2); /* 😀 */
    CHECK(cp_width(0x1160) == 0);  /* Hangul medial vowel */
    CHECK(cp_width(0x2fffd) == 2); /* unassigned, plane 2 */
    CHECK(cp_width(0x2500) == 1);  /* box drawing */
    CHECK(cp_width(0xe000) == 1);  /* private use */
}

static void
test_print_and_wrap(void) {
    struct term t;
    term_init(&t, 5, 3, &host, NULL);

    feed(&t, "abcde");
    CHECK_ROW(&t, 0, "abcde");
    CHECK_CURSOR(&t, 0, 4);
    CHECK(t.cursor.wrap_pending);

    feed(&t, "f");
    CHECK_ROW(&t, 1, "f    ");
    CHECK(grid_row(t.grid, 0)->wrapped);
    CHECK_CURSOR(&t, 1, 1);

    /* no autowrap: last column is overwritten */
    feed(&t, "\x1b[?7l\x1b[3;1Hvwxyz12");
    CHECK_ROW(&t, 2, "vwxy2");
    term_destroy(&t);
}

static void
test_scroll(void) {
    struct term t;
    term_init(&t, 3, 3, &host, NULL);

    feed(&t, "a\r\nb\r\nc\r\nd");
    CHECK_ROW(&t, 0, "b  ");
    CHECK_ROW(&t, 1, "c  ");
    CHECK_ROW(&t, 2, "d  ");
    CHECK(t.grid->scrollback_used == 1);

    /* scroll region rows 2..3: only those scroll */
    feed(&t, "\x1b[2;3r\x1b[3;1H\nx");
    CHECK_ROW(&t, 0, "b  ");
    CHECK_ROW(&t, 1, "d  ");
    CHECK_ROW(&t, 2, "x  ");

    /* reverse index at the top margin scrolls the region down */
    feed(&t, "\x1b[2;1H\x1bM");
    CHECK_ROW(&t, 0, "b  ");
    CHECK_ROW(&t, 1, "   ");
    CHECK_ROW(&t, 2, "d  ");
    term_destroy(&t);
}

static void
test_erase_and_edit(void) {
    struct term t;
    term_init(&t, 5, 2, &host, NULL);

    feed(&t, "abcde\x1b[1;3H\x1b[K");
    CHECK_ROW(&t, 0, "ab   ");
    feed(&t, "\x1b[1;1Habcde\x1b[1;3H\x1b[1K");
    CHECK_ROW(&t, 0, "   de");
    feed(&t, "\x1b[1;1Habcde\x1b[1;2H\x1b[2P");
    CHECK_ROW(&t, 0, "ade  ");
    feed(&t, "\x1b[1;2H\x1b[2@");
    CHECK_ROW(&t, 0, "a  de");
    feed(&t, "\x1b[1;1Habcde\x1b[1;2H\x1b[3X");
    CHECK_ROW(&t, 0, "a   e");
    feed(&t, "\x1b[2J");
    CHECK_ROW(&t, 0, "     ");

    /* background color erase */
    feed(&t, "\x1b[41m\x1b[2K");
    CHECK(grid_row(t.grid, 0)->cells[0].bg == COLOR_MAKE(COLOR_PALETTE, 1));
    term_destroy(&t);
}

static void
test_wide(void) {
    struct term t;
    term_init(&t, 4, 2, &host, NULL);

    feed(&t, "\xe4\xb8\xad"); /* 中 */
    CHECK_ROW(&t, 0, "?_  ");
    CHECK_CURSOR(&t, 0, 2);

    /* wide char at the last column wraps */
    feed(&t, "a\xe4\xb8\xad");
    CHECK_ROW(&t, 0, "?_a ");
    CHECK_ROW(&t, 1, "?_  ");

    /* overwriting the spacer blanks the head */
    feed(&t, "\x1b[1;2Hx");
    CHECK_ROW(&t, 0, " xa ");
    term_destroy(&t);
}

static void
test_sgr(void) {
    struct term t;
    term_init(&t, 4, 1, &host, NULL);

    feed(&t, "\x1b[1;4:3;38;5;196;48:2::1:2:3mx");
    struct cell c = grid_row(t.grid, 0)->cells[0];
    CHECK(c.attrs & ATTR_BOLD);
    CHECK(((c.attrs & ATTR_UNDERLINE_MASK) >> ATTR_UNDERLINE_SHIFT) == 3);
    CHECK(c.fg == COLOR_MAKE(COLOR_PALETTE, 196));
    CHECK(c.bg == COLOR_MAKE(COLOR_RGB, 0x010203));

    feed(&t, "\x1b[38;2;10;20;30;22;24my");
    c = grid_row(t.grid, 0)->cells[1];
    CHECK(c.fg == COLOR_MAKE(COLOR_RGB, 0x0a141e));
    CHECK(!(c.attrs & ATTR_BOLD));
    CHECK(!(c.attrs & ATTR_UNDERLINE_MASK));

    feed(&t, "\x1b[mz");
    c = grid_row(t.grid, 0)->cells[2];
    CHECK(c.fg == 0 && c.bg == 0 && c.attrs == 0);
    term_destroy(&t);
}

static void
test_alt_screen(void) {
    struct term t;
    term_init(&t, 3, 2, &host, NULL);

    feed(&t, "ab\x1b[?1049h");
    CHECK(t.modes.alt_screen);
    CHECK_ROW(&t, 0, "   ");
    feed(&t, "xy\x1b[?1049l");
    CHECK(!t.modes.alt_screen);
    CHECK_ROW(&t, 0, "ab ");
    CHECK_CURSOR(&t, 0, 2);
    term_destroy(&t);
}

static void
test_replies(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);

    reply[0] = '\0';
    feed(&t, "\x1b[3;4H\x1b[6n");
    CHECK(strcmp(reply, "\x1b[3;4R") == 0);

    reply[0] = '\0';
    feed(&t, "\x1b[c");
    CHECK(strcmp(reply, "\x1b[?62;22c") == 0);

    reply[0] = '\0';
    feed(&t, "\x1b[>q");
    CHECK(strstr(reply, "kitty") != NULL);
    term_destroy(&t);
}

static void
test_default_cursor(void) {
    struct term t;
    term_init(&t, 4, 2, &host, NULL);
    CHECK(t.cursor_style == CURSOR_BAR && t.cursor_blink);

    feed(&t, "\x1b[2 q");
    CHECK(t.cursor_style == CURSOR_BLOCK && !t.cursor_blink);
    feed(&t, "\x1b[0 q");
    CHECK(t.cursor_style == CURSOR_BAR && t.cursor_blink);
    term_destroy(&t);
}

static void
test_resize(void) {
    struct term t;
    term_init(&t, 4, 3, &host, NULL);

    /* The cursor sits after "cd", so at 2 columns it wraps onto its own row */
    feed(&t, "a\r\nb\r\ncd");
    term_resize(&t, 2, 2);
    CHECK_ROW(&t, 0, "cd");
    CHECK_ROW(&t, 1, "  ");
    CHECK_CURSOR(&t, 1, 0);
    CHECK(t.grid->scrollback_used == 2);

    term_resize(&t, 5, 4);
    CHECK_ROW(&t, 0, "a    ");
    CHECK_ROW(&t, 1, "b    ");
    CHECK_ROW(&t, 2, "cd   ");
    CHECK_ROW(&t, 3, "     ");
    CHECK_CURSOR(&t, 2, 2);

    /* The alternate screen truncates instead of reflowing */
    feed(&t, "\x1b[?1049h\x1b[Hxyz12");
    term_resize(&t, 3, 4);
    CHECK_ROW(&t, 0, "xyz");
    CHECK_ROW(&t, 1, "   ");
    term_destroy(&t);
}

static void
test_resize_clip(void) {
    struct term t;
    term_init(&t, 10, 3, &host, NULL);

    /* Unwrapped rows off the cursor line clip instead of wrapping, and come back */
    feed(&t, "abcdefgh\r\n12345678\r\n");
    term_resize(&t, 5, 3);
    CHECK_ROW(&t, 0, "abcde");
    CHECK_ROW(&t, 1, "12345");
    CHECK_CURSOR(&t, 2, 0);
    CHECK(t.grid->scrollback_used == 0);
    term_resize(&t, 10, 3);
    CHECK_ROW(&t, 0, "abcdefgh  ");
    CHECK_ROW(&t, 1, "12345678  ");

    /* An erase drops the hidden tail */
    term_resize(&t, 5, 3);
    feed(&t, "\x1b[1;3H\x1b[K");
    term_resize(&t, 10, 3);
    CHECK_ROW(&t, 0, "ab        ");
    CHECK_ROW(&t, 1, "12345678  ");
    term_destroy(&t);

    /* A wrapped run holding an image anchor re-wraps and rejoins; the anchor rides along */
    term_init(&t, 6, 3, &host, NULL);
    feed(&t, "abcdefgh");
    grid_row(t.grid, 0)->cells[2] = (struct cell){.ul = 1, .attrs = ATTR_IMAGE};
    term_resize(&t, 4, 3);
    CHECK_ROW(&t, 0, "ab d");
    CHECK_ROW(&t, 1, "efgh");
    CHECK(grid_row(t.grid, 0)->cells[2].attrs & ATTR_IMAGE);
    term_resize(&t, 6, 3);
    CHECK_ROW(&t, 0, "ab def");
    CHECK_ROW(&t, 1, "gh    ");
    CHECK(grid_row(t.grid, 0)->cells[2].attrs & ATTR_IMAGE && grid_row(t.grid, 0)->cells[2].ul == 1);
    term_destroy(&t);
}

static void
test_dec_graphics(void) {
    struct term t;
    term_init(&t, 3, 1, &host, NULL);

    feed(&t, "\x1b(0q\x1b(Bq");
    CHECK(grid_row(t.grid, 0)->cells[0].cp == 0x2500);
    CHECK(grid_row(t.grid, 0)->cells[1].cp == 'q');
    term_destroy(&t);
}

static void
test_reflow(void) {
    struct term t;
    term_init(&t, 10, 3, &host, NULL);

    /* A wrapped line re-wraps, and the cursor follows its text */
    feed(&t, "abcdefghijKLM");
    CHECK_ROW(&t, 0, "abcdefghij");
    CHECK_CURSOR(&t, 1, 3);
    term_resize(&t, 5, 3);
    CHECK_ROW(&t, 0, "abcde");
    CHECK_ROW(&t, 1, "fghij");
    CHECK_ROW(&t, 2, "KLM  ");
    CHECK_CURSOR(&t, 2, 3);
    term_resize(&t, 10, 3);
    CHECK_ROW(&t, 0, "abcdefghij");
    CHECK_ROW(&t, 1, "KLM       ");
    CHECK_CURSOR(&t, 1, 3);
    term_destroy(&t);

    /* Growing taller pulls lines back out of scrollback */
    term_init(&t, 4, 2, &host, NULL);
    feed(&t, "a\r\nb\r\nc");
    CHECK(t.grid->scrollback_used == 1);
    term_resize(&t, 4, 3);
    CHECK(t.grid->scrollback_used == 0);
    CHECK_ROW(&t, 0, "a   ");
    CHECK_ROW(&t, 1, "b   ");
    CHECK_ROW(&t, 2, "c   ");
    CHECK_CURSOR(&t, 2, 1);

    /* Shrinking pushes lines into scrollback instead of dropping them */
    term_resize(&t, 4, 1);
    CHECK_ROW(&t, 0, "c   ");
    CHECK(t.grid->scrollback_used == 2);
    CHECK_ROW(&t, -2, "a   ");
    term_destroy(&t);

    /* A wide character never splits across rows */
    term_init(&t, 5, 2, &host, NULL);
    feed(&t, "ab\xe4\xb8\xad");
    term_resize(&t, 3, 2);
    CHECK_ROW(&t, 0, "ab ");
    CHECK_ROW(&t, 1, "?_ ");
    term_destroy(&t);
}

static void
test_combining(void) {
    struct term t;
    term_init(&t, 5, 1, &host, NULL);

    feed(&t, "e\xcc\x81x");
    CHECK_CURSOR(&t, 0, 2);
    const struct cell *cells = grid_row(t.grid, 0)->cells;
    const struct composed_chain *chain = composed_get(&t.composed, cells[0].cp);
    CHECK(chain != NULL && chain->count == 2 && chain->cps[0] == 'e' && chain->cps[1] == 0x301);
    CHECK(cells[1].cp == 'x');
    uint32_t e_acute = cells[0].cp;

    /* Same chain reuses the same value; marks after a wide char go to its head */
    feed(&t, "\re\xcc\x81\xe4\xb8\xad\xcc\x81");
    CHECK(grid_row(t.grid, 0)->cells[0].cp == e_acute);
    chain = composed_get(&t.composed, grid_row(t.grid, 0)->cells[1].cp);
    CHECK(chain != NULL && chain->cps[0] == 0x4e2d);
    CHECK(grid_row(t.grid, 0)->cells[2].cp == CELL_SPACER);
    term_destroy(&t);
}

static void
test_mode_reports(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 8, 19);

    reply[0] = '\0';
    feed(&t, "\x1b[?2026$p");
    CHECK(strcmp(reply, "\x1b[?2026;2$y") == 0);
    reply[0] = '\0';
    feed(&t, "\x1b[?2026h\x1b[?2026$p");
    CHECK(strcmp(reply, "\x1b[?2026;1$y") == 0);
    CHECK(t.modes.sync_updates);
    reply[0] = '\0';
    feed(&t, "\x1b[?9999$p");
    CHECK(strcmp(reply, "\x1b[?9999;0$y") == 0);
    reply[0] = '\0';
    feed(&t, "\x1b[4$p");
    CHECK(strcmp(reply, "\x1b[4;2$y") == 0);

    reply[0] = '\0';
    feed(&t, "\x1b[18t");
    CHECK(strcmp(reply, "\x1b[8;5;10t") == 0);
    reply[0] = '\0';
    feed(&t, "\x1b[14t");
    CHECK(strcmp(reply, "\x1b[4;95;80t") == 0);
    reply[0] = '\0';
    feed(&t, "\x1b[16t");
    CHECK(strcmp(reply, "\x1b[6;19;8t") == 0);

    feed(&t, "\x1b[?1004h\x1b[?2004h");
    CHECK(t.modes.focus_events && t.modes.bracketed_paste);
    term_destroy(&t);
}

static void
test_mouse_modes(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);

    feed(&t, "\x1b[?1000h");
    CHECK(t.modes.mouse_mode == MOUSE_NORMAL);
    /* switching tracking mode without resetting the old one first */
    feed(&t, "\x1b[?1002h");
    CHECK(t.modes.mouse_mode == MOUSE_BUTTON);
    /* resetting a mode that isn't the active one is a no-op */
    feed(&t, "\x1b[?1000l");
    CHECK(t.modes.mouse_mode == MOUSE_BUTTON);
    feed(&t, "\x1b[?1002l");
    CHECK(t.modes.mouse_mode == MOUSE_OFF);

    feed(&t, "\x1b[?1003h\x1b[?1006h\x1b[?1007h");
    CHECK(t.modes.mouse_mode == MOUSE_ANY);
    CHECK(t.modes.mouse_sgr);
    CHECK(t.modes.mouse_alt_scroll);

    reply[0] = '\0';
    feed(&t, "\x1b[?1000$p");
    CHECK(strcmp(reply, "\x1b[?1000;2$y") == 0); /* off */
    reply[0] = '\0';
    feed(&t, "\x1b[?1003$p");
    CHECK(strcmp(reply, "\x1b[?1003;1$y") == 0); /* on */
    reply[0] = '\0';
    feed(&t, "\x1b[?1006$p");
    CHECK(strcmp(reply, "\x1b[?1006;1$y") == 0);
    reply[0] = '\0';
    feed(&t, "\x1b[?1007$p");
    CHECK(strcmp(reply, "\x1b[?1007;1$y") == 0);

    /* DECSTR (soft reset) doesn't touch mouse tracking, matching xterm */
    feed(&t, "\x1b[!p");
    CHECK(t.modes.mouse_mode == MOUSE_ANY);
    CHECK(t.modes.mouse_sgr);
    CHECK(t.modes.mouse_alt_scroll);

    /* RIS (full reset) clears everything, including mouse tracking */
    term_reset(&t);
    CHECK(t.modes.mouse_mode == MOUSE_OFF);
    CHECK(!t.modes.mouse_sgr);
    CHECK(!t.modes.mouse_alt_scroll);
    term_destroy(&t);
}

static void
test_scroll_view(void) {
    struct term t;
    term_init(&t, 3, 2, &host, NULL);

    feed(&t, "1\r\n2\r\n3\r\n4");
    CHECK(t.grid->scrollback_used == 2);
    term_scroll_view(&t, 1);
    CHECK(t.view_offset == 1);
    CHECK(strcmp(view_text(&t, 0), "2  ") == 0);

    /* New output keeps the scrolled-back view on the same text */
    feed(&t, "\r\n5");
    CHECK(t.view_offset == 2);
    CHECK(strcmp(view_text(&t, 0), "2  ") == 0);

    term_scroll_view(&t, 100);
    CHECK(t.view_offset == 3);
    term_scroll_view_reset(&t);
    CHECK(t.view_offset == 0);
    CHECK(strcmp(view_text(&t, 1), "5  ") == 0);
    term_destroy(&t);
}

static char *
sel_text(struct term *t) {
    static char buf[256];
    size_t len;
    char *text = selection_to_text(t, &len);
    if (text == NULL)
        return NULL;
    memcpy(buf, text, MIN(len, sizeof(buf) - 1));
    buf[MIN(len, sizeof(buf) - 1)] = '\0';
    free(text);
    return buf;
}

static void
test_selection_click(void) {
    struct term t;
    term_init(&t, 20, 3, &host, NULL);
    feed(&t, "foo bar-baz qux");

    /* A plain click with no drag deselects (xterm-style) */
    selection_start(&t, 2, 0, SEL_CHAR);
    selection_finish(&t);
    CHECK(!t.selection.active);

    /* Character selection: click-drag */
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 2, 0);
    selection_finish(&t);
    CHECK(t.selection.active);
    CHECK(strcmp(sel_text(&t), "foo") == 0);

    /* Word selection: a letter selects the run of word chars, a punctuation
     * run selects on its own (foot's isword() behavior) */
    selection_start(&t, 5, 0, SEL_WORD); /* 'a' in "bar" */
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "bar") == 0);

    selection_start(&t, 7, 0, SEL_WORD); /* '-' between "bar" and "baz" */
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "-") == 0);

    /* Dragging a word selection extends a whole word at a time */
    selection_start(&t, 5, 0, SEL_WORD);
    selection_update(&t, 9, 0); /* 'a' in "baz" */
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "bar-baz") == 0);

    /* Line selection spans a soft-wrapped logical line */
    term_destroy(&t);
    term_init(&t, 5, 4, &host, NULL);
    feed(&t, "abcdefghij");
    selection_start(&t, 2, 1, SEL_LINE);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "abcdefghij") == 0);
    term_destroy(&t);
}

static void
test_selection_extract(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "a\x1b[5Gb"); /* 'a' at col 0, cursor to col 4 (1-based col 5), 'b' */

    /* Untouched cells between real text collapse to a single space */
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 4, 0);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "a b") == 0);

    /* Trailing untouched cells at the end of a row are dropped entirely */
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 9, 0);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "a b") == 0);
    term_destroy(&t);

    /* A composed cell's whole chain is emitted */
    term_init(&t, 5, 1, &host, NULL);
    feed(&t, "e\xcc\x81x");
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 1, 0);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "e\xcc\x81x") == 0);
    term_destroy(&t);
}

static void
test_selection_scroll_and_resize(void) {
    struct term t;
    term_init(&t, 5, 2, &host, NULL);
    feed(&t, "111\r\n222");

    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 2, 0);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "111") == 0);

    /* A full-screen scroll pushes the selected line into scrollback,
     * keeping the highlight pinned to its content */
    feed(&t, "\r\n333");
    CHECK(t.selection.active);
    CHECK(t.selection.coords.start.row == -1);
    CHECK(strcmp(sel_text(&t), "111") == 0);

    /* Scrolled far enough that the ring overwrites it: selection clears */
    for (int i = 0; i < 1001; i++)
        feed(&t, "\r\nx");
    CHECK(!t.selection.active);
    term_destroy(&t);

    /* A completed selection's coordinates survive a reflow resize */
    term_init(&t, 10, 3, &host, NULL);
    feed(&t, "ab\r\ncd\r\nef");
    selection_start(&t, 0, 1, SEL_CHAR);
    selection_update(&t, 1, 1);
    selection_finish(&t);
    CHECK(strcmp(sel_text(&t), "cd") == 0);
    term_resize(&t, 6, 3);
    CHECK(t.selection.active);
    CHECK(strcmp(sel_text(&t), "cd") == 0);

    /* An in-progress drag is cancelled outright by a resize, rather than
     * tracked through reflow */
    selection_start(&t, 0, 2, SEL_CHAR);
    CHECK(t.selection.ongoing);
    term_resize(&t, 8, 3);
    CHECK(!t.selection.active && !t.selection.ongoing);
    term_destroy(&t);
}

static void
test_selection_cancel(void) {
    struct term t;
    term_init(&t, 10, 3, &host, NULL);
    feed(&t, "abc");

    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 2, 0);
    selection_finish(&t);
    CHECK(t.selection.active);

    /* RIS clears it */
    feed(&t, "\x1b"
             "c");
    CHECK(!t.selection.active);

    feed(&t, "abc");
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 2, 0);
    selection_finish(&t);
    CHECK(t.selection.active);

    /* Switching to the alternate screen clears it too */
    feed(&t, "\x1b[?1049h");
    CHECK(!t.selection.active);
    term_destroy(&t);

    /* Output on another row leaves it alone; writing to a selected row,
     * erasing it, or scrolling it within a region clears it (kitty-style) */
    term_init(&t, 10, 3, &host, NULL);
    feed(&t, "abc\r\ndef\r\n");
    selection_start(&t, 0, 0, SEL_CHAR);
    selection_update(&t, 2, 0);
    selection_finish(&t);
    feed(&t, "x");
    CHECK(t.selection.active);
    feed(&t, "\x1b[1;5Hy");
    CHECK(!t.selection.active);

    const char *changes[] = {"\x1b[2J", "\x1b[1;1H\x1b[K", "\x1b[1;1H\x1b[P", "\x1b[1;1H\x1b[L",
                             "\x1b[1;1H\x1bM", "\x1b[1;2r\x1b[2;1H\n"};
    for (size_t i = 0; i < ARRAY_LEN(changes); i++) {
        feed(&t, "\x1b"
                 "c"
                 "abc");
        selection_start(&t, 0, 0, SEL_CHAR);
        selection_update(&t, 2, 0);
        selection_finish(&t);
        CHECK(t.selection.active);
        feed(&t, changes[i]);
        CHECK(!t.selection.active);
    }
    term_destroy(&t);
}

/* Regression for the fastfetch/sixel corruption bug (root-caused in
 * local/testing-field/bug-fastfetch-corruption.md): astralia-term used to
 * discard sixel DCS payloads without ever advancing the cursor, so
 * fastfetch's compensating "CSI 9 A" (cursor up, assuming the image already
 * moved the cursor down) overshot and box-label text landed on the wrong
 * row, colliding with stale content ("memm" etc.). The fixture is the
 * minimal known repro pair: a 1006-byte tail of a real fastfetch
 * invocation's output, immediately followed by a second invocation whose
 * logo is a sixel image. */
static void
test_fastfetch_sixel_regression(void) {
    FILE *f = fopen(FASTFETCH_REGRESSION_RAW, "rb");
    CHECK(f != NULL);
    if (f == NULL)
        return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = xmalloc((size_t)fsize);
    CHECK(fread(data, 1, (size_t)fsize, f) == (size_t)fsize);
    fclose(f);

    struct term t;
    term_init(&t, 187, 47, &host, NULL);
    term_set_cell_size(&t, 10, 23);
    term_feed(&t, data, (size_t)fsize);
    free(data);

    for (int r = 0; r < t.rows; r++) {
        const char *text = row_text(&t, r);
        if (strstr(text, "memm") != NULL) {
            fprintf(stderr, "%s:%d: row %d contains corrupted \"memm\": [%s]\n", __FILE__,
                    __LINE__, r, text);
            failures++;
        }
    }

    /* Shrinking then widening restores the logo and box cell-for-cell */
    struct cell *before = xmalloc((size_t)t.rows * t.cols * sizeof(struct cell));
    for (int r = 0; r < t.rows; r++)
        memcpy(&before[r * t.cols], grid_row(t.grid, r)->cells, t.cols * sizeof(struct cell));
    term_resize(&t, 60, 47);
    term_resize(&t, 187, 47);
    for (int r = 0; r < t.rows; r++) {
        if (memcmp(&before[r * t.cols], grid_row(t.grid, r)->cells, t.cols * sizeof(struct cell)) != 0) {
            fprintf(stderr, "%s:%d: row %d changed across resize: [%s]\n", __FILE__, __LINE__, r,
                    row_text(&t, r));
            failures++;
        }
    }
    free(before);
    term_destroy(&t);
}

/* Regression for the fastfetch/kitty corruption bug (root-caused in
 * local/plan/bug-fastfetch-kitty-image-id.md): astralia-term used to reject
 * fastfetch's `--logo-type kitty` transmit (which never sends an `i=` image
 * id, a spec-legal "anonymous" image) with an EINVAL reply, and that
 * unsolicited reply -- never read by fastfetch -- leaked onto the pty and
 * got typed into the next shell prompt. The fixture is a real, complete
 * chunked `a=T` APC sequence captured from a live `fastfetch --logo-type
 * kitty` invocation, with no `i=` key. */
static void
test_fastfetch_kitty_regression(void) {
    FILE *f = fopen(FASTFETCH_KITTY_REGRESSION_RAW, "rb");
    CHECK(f != NULL);
    if (f == NULL)
        return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = xmalloc((size_t)fsize);
    CHECK(fread(data, 1, (size_t)fsize, f) == (size_t)fsize);
    fclose(f);

    struct term t;
    term_init(&t, 92, 47, &host, NULL);
    term_set_cell_size(&t, 10, 23);
    reply[0] = '\0';
    term_feed(&t, data, (size_t)fsize);
    free(data);

    CHECK(reply[0] == '\0');
    CHECK(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE);

    term_destroy(&t);
}

int main(void) {
    test_width();
    test_print_and_wrap();
    test_scroll();
    test_erase_and_edit();
    test_wide();
    test_sgr();
    test_alt_screen();
    test_replies();
    test_default_cursor();
    test_resize();
    test_dec_graphics();
    test_reflow();
    test_resize_clip();
    test_combining();
    test_mode_reports();
    test_mouse_modes();
    test_scroll_view();
    test_selection_click();
    test_selection_extract();
    test_selection_scroll_and_resize();
    test_selection_cancel();
    test_fastfetch_sixel_regression();
    test_fastfetch_kitty_regression();

    if (failures == 0)
        printf("test_grid: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
