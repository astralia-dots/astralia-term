/* Kitty graphics protocol Unicode placeholders: diacritic table round-trip,
 * left-neighbor inheritance, and image/placement id extraction from a
 * placeholder cell's raw fg/ul colors. Driven through real escape sequences
 * matching the worked examples in the protocol spec, not synthetic structs,
 * so a mistake in how term.c/vt_csi.c feed the cell can't hide behind a
 * hand-built fixture. */
#include <stdio.h>
#include <string.h>

#include "term/kitty_diacritics.h"
#include "term/kitty_placeholder.h"
#include "term/term.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static void
on_write(void *user, const void *data, size_t len) {
    (void)user;
    (void)data;
    (void)len;
}

static const struct term_host host = {.write = on_write};

static void
utf8_append(char **p, uint32_t cp) {
    if (cp < 0x80)
        *(*p)++ = (char)cp;
    else if (cp < 0x800) {
        *(*p)++ = (char)(0xc0 | (cp >> 6));
        *(*p)++ = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *(*p)++ = (char)(0xe0 | (cp >> 12));
        *(*p)++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *(*p)++ = (char)(0x80 | (cp & 0x3f));
    } else {
        *(*p)++ = (char)(0xf0 | (cp >> 18));
        *(*p)++ = (char)(0x80 | ((cp >> 12) & 0x3f));
        *(*p)++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *(*p)++ = (char)(0x80 | (cp & 0x3f));
    }
}

/* Prints a placeholder cell (base + up to 3 diacritics) as UTF-8. cps[0] is
 * always KITTY_PLACEHOLDER_CP; pass 0 to omit a trailing diacritic slot. */
static void
feed_placeholder(struct term *t, uint32_t row_diac, uint32_t col_diac, uint32_t msb_diac) {
    char buf[32];
    char *p = buf;
    utf8_append(&p, KITTY_PLACEHOLDER_CP);
    if (row_diac != 0)
        utf8_append(&p, row_diac);
    if (col_diac != 0)
        utf8_append(&p, col_diac);
    if (msb_diac != 0)
        utf8_append(&p, msb_diac);
    term_feed(t, (const uint8_t *)buf, (size_t)(p - buf));
}

static void
feed(struct term *t, const char *s) {
    term_feed(t, (const uint8_t *)s, strlen(s));
}

/* U+0305/U+030D/U+030E are the spec's own worked-example diacritics for
 * row/column values 0/1/2 (graphics-protocol.rst, Unicode placeholders). */
static void
test_diacritic_table_matches_spec_examples(void) {
    CHECK(kitty_diacritic_index(0x305) == 0);
    CHECK(kitty_diacritic_index(0x30D) == 1);
    CHECK(kitty_diacritic_index(0x30E) == 2);
    CHECK(kitty_diacritic_index('A') == -1); /* not a row/col diacritic */
}

static void
test_decode_non_placeholder(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "A");
    struct kitty_placeholder_cell pc =
        kitty_placeholder_decode(&t, grid_row(t.grid, 0)->cells[0].cp);
    CHECK(!pc.is_placeholder);
    term_destroy(&t);
}

static void
test_decode_placeholder_full_diacritics(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    /* Row 1, column 2, MSB byte 0: diacritic indices 1, 2, 0. */
    feed_placeholder(&t, 0x30D, 0x30E, 0x305);
    struct kitty_placeholder_cell pc =
        kitty_placeholder_decode(&t, grid_row(t.grid, 0)->cells[0].cp);
    CHECK(pc.is_placeholder);
    CHECK(pc.have_row && pc.row == 1);
    CHECK(pc.have_col && pc.col == 2);
    CHECK(pc.have_msb && pc.msb == 0);
    term_destroy(&t);
}

static void
test_decode_bare_placeholder(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed_placeholder(&t, 0, 0, 0);
    struct kitty_placeholder_cell pc =
        kitty_placeholder_decode(&t, grid_row(t.grid, 0)->cells[0].cp);
    CHECK(pc.is_placeholder);
    CHECK(!pc.have_row && !pc.have_col && !pc.have_msb);
    term_destroy(&t);
}

/* The 2x2 example from graphics-protocol.rst, every cell fully specified:
 *   printf "\e[38;5;42m\U10EEEE\U0305\U0305\U10EEEE\U0305\U030D\e[39m\n"
 *   printf "\e[38;5;42m\U10EEEE\U030D\U0305\U10EEEE\U030D\U030D\e[39m\n"
 * yields (row,col) 0,0 / 0,1 on the first line and 1,0 / 1,1 on the second. */
static void
test_inheritance_full_2x2_example(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "\x1b[38;5;42m");
    feed_placeholder(&t, 0x305, 0x305, 0);
    feed_placeholder(&t, 0x305, 0x30D, 0);
    feed(&t, "\x1b[39m");

    const struct cell *cells = grid_row(t.grid, 0)->cells;
    struct kitty_placeholder_run run = {0};
    int row, col, msb;

    struct kitty_placeholder_cell pc0 = kitty_placeholder_decode(&t, cells[0].cp);
    CHECK(kitty_placeholder_resolve(&run, &pc0, cells[0].fg, cells[0].ul, &row, &col, &msb));
    CHECK(row == 0 && col == 0);

    struct kitty_placeholder_cell pc1 = kitty_placeholder_decode(&t, cells[1].cp);
    CHECK(kitty_placeholder_resolve(&run, &pc1, cells[1].fg, cells[1].ul, &row, &col, &msb));
    CHECK(row == 0 && col == 1);

    term_destroy(&t);
}

/* The 2 rows x 3 columns omission example from graphics-protocol.rst:
 *   printf "\e[38;5;42m\U10EEEE\U0305\U10EEEE\U10EEEE\n"
 *   printf "\e[38;5;42m\U10EEEE\U030D\U10EEEE\U10EEEE\n"
 * Only the first cell of each line carries a (row) diacritic; the rest omit
 * every diacritic and inherit row from, and increment column past, the cell
 * to their left. */
static void
test_inheritance_row_then_omitted(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "\x1b[38;5;42m");
    feed_placeholder(&t, 0x305, 0, 0); /* row 0, column omitted -> fresh run, col 0 */
    feed_placeholder(&t, 0, 0, 0);     /* everything omitted -> inherit row, col+1 */
    feed_placeholder(&t, 0, 0, 0);
    feed(&t, "\x1b[39m");

    const struct cell *cells = grid_row(t.grid, 0)->cells;
    struct kitty_placeholder_run run = {0};
    int row, col, msb;
    int expect_col[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) {
        struct kitty_placeholder_cell pc = kitty_placeholder_decode(&t, cells[i].cp);
        CHECK(kitty_placeholder_resolve(&run, &pc, cells[i].fg, cells[i].ul, &row, &col, &msb));
        CHECK(row == 0);
        CHECK(col == expect_col[i]);
    }
    term_destroy(&t);
}

/* A color change breaks the run: the next placeholder's omitted diacritics
 * can't inherit from a cell with a different fg/ul. */
static void
test_inheritance_breaks_on_color_change(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "\x1b[38;5;42m");
    feed_placeholder(&t, 0x305, 0x305, 0); /* row 0, col 0 */
    feed(&t, "\x1b[38;5;43m");
    feed_placeholder(&t, 0, 0, 0); /* different fg: can't inherit */
    feed(&t, "\x1b[39m");

    const struct cell *cells = grid_row(t.grid, 0)->cells;
    struct kitty_placeholder_run run = {0};
    int row, col, msb;
    struct kitty_placeholder_cell pc0 = kitty_placeholder_decode(&t, cells[0].cp);
    CHECK(kitty_placeholder_resolve(&run, &pc0, cells[0].fg, cells[0].ul, &row, &col, &msb));

    struct kitty_placeholder_cell pc1 = kitty_placeholder_decode(&t, cells[1].cp);
    CHECK(!kitty_placeholder_resolve(&run, &pc1, cells[1].fg, cells[1].ul, &row, &col, &msb));
    term_destroy(&t);
}

/* Image ID rides the foreground color (38;5;42 -> palette value 42);
 * placement ID rides the underline color (58;5;7 -> palette value 7), per
 * the spec's "encoding the image ID in its foreground color" /
 * "specify a placement ID using the underline color". */
static void
test_ids_ride_fg_and_underline_color(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "\x1b[38;5;42m\x1b[58;5;7m");
    feed_placeholder(&t, 0x305, 0x305, 0);
    feed(&t, "\x1b[39m\x1b[59m");

    const struct cell *c = &grid_row(t.grid, 0)->cells[0];
    CHECK(COLOR_TAG(c->fg) == COLOR_PALETTE && COLOR_VALUE(c->fg) == 42);
    CHECK(COLOR_TAG(c->ul) == COLOR_PALETTE && COLOR_VALUE(c->ul) == 7);
    term_destroy(&t);
}

int main(void) {
    test_diacritic_table_matches_spec_examples();
    test_decode_non_placeholder();
    test_decode_placeholder_full_diacritics();
    test_decode_bare_placeholder();
    test_inheritance_full_2x2_example();
    test_inheritance_row_then_omitted();
    test_inheritance_breaks_on_color_change();
    test_ids_ride_fg_and_underline_color();

    if (failures == 0)
        printf("test_kitty_placeholder: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
