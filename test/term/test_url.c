/* OSC 8 hyperlinks and plain-text URL detection under a cell. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term/term.h"
#include "term/url.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static void
feed(struct term *t, const char *s) {
    term_feed(t, (const uint8_t *)s, strlen(s));
}

/* URI at (row, col) compared with want (NULL = no link); range checked if found. */
static bool
link_is(struct term *t, int row, int col, const char *want, int r0, int c0, int r1, int c1) {
    struct grid_range range;
    char *uri = NULL;
    bool found = url_at(t, row, col, &range, &uri);
    bool ok = want == NULL ? !found
                           : found && strcmp(uri, want) == 0 && range.start.row == r0 &&
                                 range.start.col == c0 && range.end.row == r1 &&
                                 range.end.col == c1;
    if (!ok)
        fprintf(stderr, "  at %d,%d: got %s\n", row, col, found ? uri : "(none)");
    free(uri);
    return ok;
}

static void
test_osc8(void) {
    struct term t;
    term_init(&t, 20, 3, NULL, NULL);
    feed(&t, "a \x1b]8;id=1;file:///x;y\x1b\\link\x1b[0m\x1b]8;;\x1b\\ b\r\n");
    CHECK(link_is(&t, 0, 0, NULL, 0, 0, 0, 0));
    CHECK(link_is(&t, 0, 2, "file:///x;y", 0, 2, 0, 5));
    CHECK(link_is(&t, 0, 5, "file:///x;y", 0, 2, 0, 5)); /* SGR 0 keeps the link */
    CHECK(link_is(&t, 0, 7, NULL, 0, 0, 0, 0));
    bool mask[20];
    url_row_links(&t, 0, mask);
    CHECK(!mask[1] && mask[2] && mask[5] && !mask[6]);

    feed(&t, "\x1b[1;3H\x1b[K\r\n"); /* erase drops it */
    CHECK(link_is(&t, 0, 3, NULL, 0, 0, 0, 0));
    term_destroy(&t);
}

static void
test_text(void) {
    struct term t;
    term_init(&t, 20, 4, NULL, NULL);
    /* Soft-wraps after "c_(", so the URL continues on row 1 */
    feed(&t, "see (https://a.b/c_(d)).\r\n");
    CHECK(link_is(&t, 0, 10, "https://a.b/c_(d)", 0, 5, 1, 1));
    CHECK(link_is(&t, 1, 0, "https://a.b/c_(d)", 0, 5, 1, 1));
    CHECK(link_is(&t, 1, 2, NULL, 0, 0, 0, 0)); /* unbalanced ')' trimmed */
    CHECK(link_is(&t, 0, 4, NULL, 0, 0, 0, 0));

    feed(&t, "xhttps://q foo:bar\r\nmailto:x@y, https://");
    bool mask[20];
    CHECK(link_is(&t, 3, 2, NULL, 0, 0, 0, 0)); /* the cursor's line: still being typed */
    url_row_links(&t, 3, mask);
    CHECK(!mask[0]);
    t.modes.alt_screen = true; /* full-screen apps have no typing line */
    CHECK(link_is(&t, 3, 2, "mailto:x@y", 3, 0, 3, 9));
    t.modes.alt_screen = false;

    feed(&t, "\x1b[3;1H"); /* cursor off the line, as after Enter */
    CHECK(link_is(&t, 2, 3, NULL, 0, 0, 0, 0));
    CHECK(link_is(&t, 2, 12, NULL, 0, 0, 0, 0));
    CHECK(link_is(&t, 3, 2, "mailto:x@y", 3, 0, 3, 9));
    CHECK(link_is(&t, 3, 14, NULL, 0, 0, 0, 0)); /* scheme alone */

    url_row_links(&t, 0, mask);
    CHECK(!mask[4] && mask[5] && mask[19]);
    url_row_links(&t, 1, mask);
    CHECK(mask[0] && mask[1] && !mask[2]);
    url_row_links(&t, 3, mask);
    CHECK(mask[0] && mask[9] && !mask[10] && !mask[19]);

    url_hover(&t, 3, 2);
    CHECK(t.link_hover.active && t.link_hover.range.end.col == 9);
    url_hover(&t, 3, 11);
    CHECK(!t.link_hover.active);
    term_destroy(&t);
}

int main(void) {
    test_osc8();
    test_text();
    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
