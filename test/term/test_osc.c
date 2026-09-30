/* OSC sequences fed through term: title, colors, clipboard, hyperlinks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/base64.h"
#include "core/util.h"
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
static char title[64];
static char clip[64];
static char clip_target;

static void
on_write(void *user, const void *data, size_t len) {
    size_t cur = strlen(reply);
    if (cur + len < sizeof(reply)) {
        memcpy(reply + cur, data, len);
        reply[cur + len] = '\0';
    }
}

static void
on_title(void *user, const char *s) {
    snprintf(title, sizeof(title), "%s", s);
}

static void
on_clipboard(void *user, char target, const char *text, size_t len) {
    clip_target = target;
    snprintf(clip, sizeof(clip), "%.*s", (int)len, text);
}

static const struct term_host host = {
    .write = on_write,
    .set_title = on_title,
    .set_clipboard = on_clipboard,
};

static void
feed(struct term *t, const char *s) {
    reply[0] = '\0';
    term_feed(t, (const uint8_t *)s, strlen(s));
}

static void
test_title(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);
    feed(&t, "\x1b]0;hello\a");
    CHECK(strcmp(title, "hello") == 0);
    feed(&t, "\x1b]2;w\xc3\xb6rld;x\x1b\\");
    CHECK(strcmp(title, "w\xc3\xb6rld;x") == 0);
    term_destroy(&t);
}

static void
test_colors(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    /* Queries answer with the request's terminator */
    feed(&t, "\x1b]4;1;?\a");
    CHECK(strcmp(reply, "\x1b]4;1;rgb:cccc/9393/9393\a") == 0);
    feed(&t, "\x1b]11;?\x1b\\");
    CHECK(strcmp(reply, "\x1b]11;rgb:0000/0000/0000\x1b\\") == 0);

    t.colors_changed = false;
    feed(&t, "\x1b]4;1;rgb:ff/80/0;2;#123456\a");
    CHECK(t.palette[1] == 0xff8000);
    CHECK(t.palette[2] == 0x123456);
    CHECK(t.colors_changed);
    feed(&t, "\x1b]4;3;rgb:ffff/0/8080\a");
    CHECK(t.palette[3] == 0xff0080);

    /* 10 with two arguments sets fg and bg */
    feed(&t, "\x1b]10;#ffffff;#101010\a");
    CHECK(t.default_fg == 0xffffff && t.default_bg == 0x101010);
    feed(&t, "\x1b]12;#00ff00\a");
    CHECK(t.cursor_color_set && t.cursor_color == 0x00ff00);

    feed(&t, "\x1b]104;1\a");
    CHECK(t.palette[1] == t.initial_palette[1] && t.palette[2] == 0x123456);
    feed(&t, "\x1b]104\a\x1b]110\a\x1b]111\a\x1b]112\a");
    CHECK(t.palette[2] == t.initial_palette[2]);
    CHECK(t.default_fg == t.initial_fg && t.default_bg == t.initial_bg);
    CHECK(!t.cursor_color_set);

    /* Malformed specs are ignored */
    feed(&t, "\x1b]4;1;rgb:zz/0/0\a\x1b]4;999;#ffffff\a");
    CHECK(t.palette[1] == t.initial_palette[1]);
    term_destroy(&t);
}

static void
test_clipboard(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    feed(&t, "\x1b]52;c;aGVsbG8gd29ybGQ=\a");
    CHECK(clip_target == 'c' && strcmp(clip, "hello world") == 0);
    feed(&t, "\x1b]52;p;Zm9v\x1b\\");
    CHECK(clip_target == 'p' && strcmp(clip, "foo") == 0);

    /* Reads are denied: no reply, clipboard untouched */
    feed(&t, "\x1b]52;c;?\a");
    CHECK(reply[0] == '\0' && strcmp(clip, "foo") == 0);
    feed(&t, "\x1b]52;c;!!!\a");
    CHECK(strcmp(clip, "foo") == 0);
    term_destroy(&t);
}

static const struct cell *
cell_at(struct term *t, int row, int col) {
    return &grid_row(t->grid, row)->cells[col];
}

static void
test_hyperlinks(void) {
    struct term t;
    term_init(&t, 10, 3, &host, NULL);

    /* Cells written inside the link carry its handle; cells outside carry none */
    feed(&t, "a\x1b]8;;https://a.example/x\x1b\\bc\x1b]8;;\x1b\\d");
    CHECK(cell_link(cell_at(&t, 0, 0)) == 0);
    uint16_t h = cell_link(cell_at(&t, 0, 1));
    CHECK(h != 0 && cell_link(cell_at(&t, 0, 2)) == h);
    CHECK(cell_link(cell_at(&t, 0, 3)) == 0);
    const struct hyperlink *l = hyperlink_get(&t.hyperlinks, h);
    CHECK(l != NULL && strcmp(l->uri, "https://a.example/x") == 0 && l->id[0] == '\0');

    /* Same (id, uri) dedups; a different id does not; URIs may hold ';' and BEL terminates */
    feed(&t, "\r\n\x1b]8;id=k:foo=bar;https://b.example/?q=1;2\aE\x1b]8;;\a");
    uint16_t h2 = cell_link(cell_at(&t, 1, 0));
    l = hyperlink_get(&t.hyperlinks, h2);
    CHECK(h2 != 0 && h2 != h && strcmp(l->id, "k") == 0 &&
          strcmp(l->uri, "https://b.example/?q=1;2") == 0);
    feed(&t, "\x1b]8;id=k;https://b.example/?q=1;2\aF\x1b]8;;\a");
    CHECK(cell_link(cell_at(&t, 1, 1)) == h2);
    feed(&t, "\x1b]8;id=j;https://b.example/?q=1;2\aG\x1b]8;;\a");
    CHECK(cell_link(cell_at(&t, 1, 2)) != h2);

    /* SGR reset does not close a link; erasing a cell removes it */
    feed(&t, "\r\n\x1b]8;;https://a.example/x\x1b\\\x1b[0mH\x1b[0m");
    CHECK(cell_link(cell_at(&t, 2, 0)) == h);
    feed(&t, "\x1b]8;;\x1b\\\r\x1b[K");
    CHECK(cell_link(cell_at(&t, 2, 0)) == 0);

    /* Survives scrolling into history */
    feed(&t, "\r\n");
    CHECK(cell_link(cell_at(&t, 0, 0)) == h2);
    CHECK(cell_link(cell_at(&t, -1, 1)) == h);

    /* An over-long URI is rejected and closes any open link */
    char *big = xmalloc(HYPERLINK_MAX_URI + 64);
    strcpy(big, "\x1b]8;;http://");
    size_t n = strlen(big);
    memset(big + n, 'x', HYPERLINK_MAX_URI);
    strcpy(big + n + HYPERLINK_MAX_URI, "\aZ");
    feed(&t, "\x1b]8;;http://ok\a");
    feed(&t, big);
    free(big);
    CHECK(cell_link(cell_at(&t, 2, 0)) == 0);

    /* Reset drops the table and the open link */
    feed(&t, "\x1b]8;;http://ok\a\x1b" "c");
    CHECK(t.link == 0 && t.hyperlinks.count == 0);
    term_destroy(&t);
}

static void
test_base64(void) {
    uint8_t out[16];
    CHECK(base64_decode("TWFu", 4, out) == 3 && memcmp(out, "Man", 3) == 0);
    CHECK(base64_decode("TWE=", 4, out) == 2 && memcmp(out, "Ma", 2) == 0);
    CHECK(base64_decode("TQ==", 4, out) == 1 && out[0] == 'M');
    CHECK(base64_decode("TQ=a", 4, out) == (size_t)-1);
    CHECK(base64_decode("T===", 4, out) == (size_t)-1);
    CHECK(base64_decode("TWF", 3, out) == (size_t)-1);

    char *s = base64_encode((const uint8_t *)"Ma", 2);
    CHECK(strcmp(s, "TWE=") == 0);
    free(s);
}

int main(void) {
    test_title();
    test_colors();
    test_clipboard();
    test_hyperlinks();
    test_base64();

    if (failures == 0)
        printf("test_osc: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
