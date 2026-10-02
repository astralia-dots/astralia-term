/* Legacy and kitty keyboard protocol (CSI ... u) encoding. */
#include <stdio.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/util.h"
#include "input/input.h"
#include "input/kitty_keys.h"
#include "term/term.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

#define CHECK_SEQ(ev, t, expect)                                                                \
    do {                                                                                        \
        char out[INPUT_MAX_SEQ];                                                                \
        size_t n = input_encode(&(ev), (t), out);                                               \
        size_t explen = sizeof(expect) - 1;                                                     \
        if (n != explen || memcmp(out, expect, explen) != 0) {                                  \
            fprintf(stderr, "%s:%d: CHECK_SEQ failed: got %zu bytes\n", __FILE__, __LINE__, n); \
            failures++;                                                                         \
        }                                                                                       \
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

int main(void) {
    /* kitty_lookup() in input.c relies on kitty_keymap[] staying sorted by
     * sym for its binary search; a single out-of-order entry silently
     * breaks lookups for unrelated keys elsewhere in the table. */
    for (size_t i = 1; i < ARRAY_LEN(kitty_keymap); i++)
        CHECK(kitty_keymap[i - 1].sym < kitty_keymap[i].sym);

    struct term t;
    term_init(&t, 80, 24, &host, NULL);

    /* ---- legacy (kitty protocol off) ---- */

    struct key_event a_press = {.sym = XKB_KEY_a, .utf8 = "a", .utf8_len = 1};
    CHECK_SEQ(a_press, &t, "a");

    struct key_event ctrl_a = {
        .sym = XKB_KEY_a, .mods = MOD_CTRL, .utf8 = "\x01", .utf8_len = 1};
    CHECK_SEQ(ctrl_a, &t, "\x01");

    struct key_event up = {.sym = XKB_KEY_Up};
    CHECK_SEQ(up, &t, "\033[A");

    t.modes.app_cursor_keys = true;
    CHECK_SEQ(up, &t, "\033OA");
    t.modes.app_cursor_keys = false;

    struct key_event shift_up = {.sym = XKB_KEY_Up, .mods = MOD_SHIFT};
    CHECK_SEQ(shift_up, &t, "\033[1;2A");

    struct key_event a_release = {
        .sym = XKB_KEY_a, .utf8 = "a", .utf8_len = 1, .action = KEY_RELEASE};
    CHECK_SEQ(a_release, &t, "");

    /* ---- kitty protocol: CSI > / < / = / ? u wired through the real parser ---- */

    reply[0] = '\0';
    term_feed(&t, (const uint8_t *)"\033[>1u", 5); /* push, disambiguate */
    CHECK(t.grid->kitty_kbd.flags[t.grid->kitty_kbd.idx] == KITTY_KBD_DISAMBIGUATE);

    term_feed(&t, (const uint8_t *)"\033[?u", 4); /* query */
    CHECK(strcmp(reply, "\033[?1u") == 0);

    term_feed(&t, (const uint8_t *)"\033[=6;2u", 7); /* set: OR in report-event|report-alt */
    CHECK(t.grid->kitty_kbd.flags[t.grid->kitty_kbd.idx] ==
          (KITTY_KBD_DISAMBIGUATE | KITTY_KBD_REPORT_EVENT | KITTY_KBD_REPORT_ALTERNATE));

    term_feed(&t, (const uint8_t *)"\033[=1u", 5); /* set: replace with disambiguate only */
    CHECK(t.grid->kitty_kbd.flags[t.grid->kitty_kbd.idx] == KITTY_KBD_DISAMBIGUATE);

    /* ---- kitty encoding, disambiguate only ---- */

    CHECK_SEQ(a_press, &t, "a"); /* plain text still passes through as text */
    CHECK_SEQ(ctrl_a, &t, "\033[97;5u");
    CHECK_SEQ(up, &t, "\033[A"); /* legacy final, no params needed */

    struct key_event ctrl_up = {.sym = XKB_KEY_Up, .mods = MOD_CTRL};
    CHECK_SEQ(ctrl_up, &t, "\033[1;5A");

    struct key_event enter = {.sym = XKB_KEY_Return, .utf8 = "\r", .utf8_len = 1};
    CHECK_SEQ(enter, &t, "\033[13u"); /* disambiguated from a literal CR */

    struct key_event ctrl_enter = {
        .sym = XKB_KEY_Return, .mods = MOD_CTRL, .utf8 = "\r", .utf8_len = 1};
    CHECK_SEQ(ctrl_enter, &t, "\033[13;5u");

    CHECK_SEQ(a_release, &t, ""); /* report-event is off: releases are silent */

    /* ---- kitty encoding, disambiguate + report events ---- */

    term_feed(&t, (const uint8_t *)"\033[=3u", 5); /* replace: disambiguate|report-event */

    CHECK_SEQ(ctrl_a, &t, "\033[97;5u"); /* press: event type 1 stays implicit */

    struct key_event ctrl_a_release = {
        .sym = XKB_KEY_a, .mods = MOD_CTRL, .utf8 = "\x01", .utf8_len = 1, .action = KEY_RELEASE};
    CHECK_SEQ(ctrl_a_release, &t, "\033[97;5:3u");

    struct key_event ctrl_a_repeat = {
        .sym = XKB_KEY_a, .mods = MOD_CTRL, .utf8 = "\x01", .utf8_len = 1, .action = KEY_REPEAT};
    CHECK_SEQ(ctrl_a_repeat, &t, "\033[97;5:2u");

    struct key_event ctrl_shift_e = {.sym = XKB_KEY_E,
                                     .unshifted = XKB_KEY_e,
                                     .mods = MOD_CTRL | MOD_SHIFT,
                                     .utf8 = "\x05",
                                     .utf8_len = 1};
    CHECK_SEQ(ctrl_shift_e, &t, "\033[101;6u"); /* nvim's flags: must differ from ctrl+e */

    /* a window resize (tiling a neighbour) must not wipe the keyboard mode */
    term_resize(&t, 60, 20);
    CHECK_SEQ(ctrl_shift_e, &t, "\033[101;6u");
    term_resize(&t, 80, 24);

    /* ---- kitty encoding, report-alternate: functional keys carry no alternates ---- */

    term_feed(&t, (const uint8_t *)"\033[=7u", 5); /* disambiguate|report-event|report-alt */

    /* xkb maps Delete/BackSpace to U+007F/U+0008, which must not leak as ":127" */
    struct key_event del = {.sym = XKB_KEY_Delete, .base = XKB_KEY_Delete,
                            .unshifted = XKB_KEY_Delete, .utf8 = "\x7f", .utf8_len = 1};
    CHECK_SEQ(del, &t, "\033[3~");

    struct key_event bksp = {.sym = XKB_KEY_BackSpace, .base = XKB_KEY_BackSpace,
                             .unshifted = XKB_KEY_BackSpace, .utf8 = "\b", .utf8_len = 1};
    CHECK_SEQ(bksp, &t, "\033[127u");

    term_feed(&t, (const uint8_t *)"\033[<u", 4); /* pop back to the never-touched base level */
    CHECK(t.grid->kitty_kbd.flags[t.grid->kitty_kbd.idx] == 0);

    /* RIS clears the whole stack */
    term_reset(&t);
    CHECK(t.grid->kitty_kbd.flags[t.grid->kitty_kbd.idx] == 0);
    CHECK_SEQ(a_press, &t, "a");

    /* ---- mouse: SGR and X10 encodings ---- */

#define CHECK_MOUSE(button, pressed, motion, col, row, mods, sgr, expect)                  \
    do {                                                                                   \
        char out[INPUT_MAX_SEQ];                                                           \
        size_t n = input_mouse_encode((button), (pressed), (motion), (col), (row), (mods), \
                                      (sgr), out);                                         \
        size_t explen = sizeof(expect) - 1;                                                \
        if (n != explen || memcmp(out, expect, explen) != 0) {                             \
            fprintf(stderr, "%s:%d: CHECK_MOUSE failed: got %.*s\n", __FILE__, __LINE__,   \
                    (int)n, out);                                                          \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

    /* SGR: left press/release/drag at col 5, row 10 (0-based -> 6;11 1-based);
     * xterm's button numbering is 0-based (left=0), unlike this API's 1-based. */
    CHECK_MOUSE(1, true, false, 5, 10, 0, true, "\033[<0;6;11M");
    CHECK_MOUSE(1, false, false, 5, 10, 0, true, "\033[<0;6;11m");
    CHECK_MOUSE(1, true, true, 5, 10, 0, true, "\033[<32;6;11M");
    CHECK_MOUSE(2, true, false, 0, 0, MOD_SHIFT | MOD_CTRL, true, "\033[<21;1;1M");
    /* no-button motion (any-event mode, nothing held): cb forced to xterm's 3 */
    CHECK_MOUSE(0, true, true, 0, 0, 0, true, "\033[<35;1;1M");

    /* X10: cb+32/col+32/row+32 as raw bytes; release always reports cb=3 */
    CHECK_MOUSE(1, true, false, 0, 0, 0, false, "\033[M !!");
    CHECK_MOUSE(1, false, false, 0, 0, 0, false, "\033[M#!!");
    /* clamped at 223 to stay a single byte */
    CHECK_MOUSE(1, true, false, 500, 500, 0, false, "\033[M \xff\xff");

    /* ---- wheel: xterm buttons 64-67 ---- */

#define CHECK_WHEEL(dir, col, row, mods, sgr, expect)                                    \
    do {                                                                                 \
        char out[INPUT_MAX_SEQ];                                                         \
        size_t n = input_wheel_encode((dir), (col), (row), (mods), (sgr), out);          \
        size_t explen = sizeof(expect) - 1;                                              \
        if (n != explen || memcmp(out, expect, explen) != 0) {                           \
            fprintf(stderr, "%s:%d: CHECK_WHEEL failed: got %.*s\n", __FILE__, __LINE__, \
                    (int)n, out);                                                        \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

    CHECK_WHEEL(0, 0, 0, 0, true, "\033[<64;1;1M"); /* up */
    CHECK_WHEEL(1, 0, 0, 0, true, "\033[<65;1;1M"); /* down */
    CHECK_WHEEL(2, 0, 0, 0, true, "\033[<66;1;1M"); /* left */
    CHECK_WHEEL(3, 0, 0, MOD_SHIFT, true, "\033[<71;1;1M");
    CHECK_WHEEL(0, 0, 0, 0, false, "\033[M`!!");

    /* ---- arrow (1007 alternate-scroll fallback) ---- */

    char out[INPUT_MAX_SEQ];
    size_t n = input_encode_arrow(true, false, out);
    CHECK(n == 3 && memcmp(out, "\033[A", 3) == 0);
    n = input_encode_arrow(false, true, out);
    CHECK(n == 3 && memcmp(out, "\033OB", 3) == 0);

    term_destroy(&t);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
