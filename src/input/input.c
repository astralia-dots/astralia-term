#include "input/input.h"

#include <stdio.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "core/util.h"
#include "input/kitty_keys.h"

/* modifier parameter: 1 + shift(1) + alt(2) + ctrl(4) + super(8) */
static int
mod_param(unsigned mods) {
    return 1 + ((mods & MOD_SHIFT) ? 1 : 0) + ((mods & MOD_ALT) ? 2 : 0) +
           ((mods & MOD_CTRL) ? 4 : 0) + ((mods & MOD_SUPER) ? 8 : 0);
}

/* CSI/SS3 keys with a final letter: arrows, Home, End, F1-F4 */
static size_t
letter_key(char *out, char final, unsigned mods, bool ss3) {
    int m = mod_param(mods);
    if (m > 1)
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[1;%d%c", m, final);
    return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033%c%c", ss3 ? 'O' : '[', final);
}

/* CSI n ~ keys: Insert, Delete, PgUp, PgDn, F5-F12 */
static size_t
tilde_key(char *out, int n, unsigned mods) {
    int m = mod_param(mods);
    if (m > 1)
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[%d;%d~", n, m);
    return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[%d~", n);
}

static size_t
with_alt(char *out, unsigned mods, const char *s, size_t len) {
    size_t n = 0;
    if (mods & MOD_ALT)
        out[n++] = '\033';
    if (n + len > INPUT_MAX_SEQ)
        return 0;
    memcpy(out + n, s, len);
    return n + len;
}

/* ---- kitty keyboard protocol (CSI ... u), progressive enhancement ---- */

static const struct kitty_key_data *
kitty_lookup(xkb_keysym_t sym) {
    int lo = 0, hi = (int)ARRAY_LEN(kitty_keymap) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kitty_keymap[mid].sym == sym)
            return &kitty_keymap[mid];
        if (kitty_keymap[mid].sym < sym)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* First UTF-32 code point of a short UTF-8 string; 0 if empty or malformed. */
static uint32_t
utf8_first_cp(const char *s, int len) {
    if (len <= 0)
        return 0;
    uint8_t c = (uint8_t)s[0];
    uint32_t cp;
    int extra;
    if ((c & 0x80) == 0x00) {
        cp = c;
        extra = 0;
    } else if ((c & 0xe0) == 0xc0) {
        cp = c & 0x1f;
        extra = 1;
    } else if ((c & 0xf0) == 0xe0) {
        cp = c & 0x0f;
        extra = 2;
    } else if ((c & 0xf8) == 0xf0) {
        cp = c & 0x07;
        extra = 3;
    } else
        return 0;
    if (extra >= len)
        return 0;
    for (int i = 1; i <= extra; i++) {
        uint8_t cc = (uint8_t)s[i];
        if ((cc & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3f);
    }
    return cp;
}

static bool
is_printable_cp(uint32_t cp) {
    return cp >= 0x20 && cp != 0x7f;
}

static bool
is_printable_text(const struct key_event *ev) {
    return ev->utf8_len > 0 && is_printable_cp((uint8_t)ev->utf8[0]);
}

static void
kitty_event_suffix(char buf[3], bool report_event, int event) {
    if (report_event && event != 1) {
        buf[0] = ':';
        buf[1] = (char)('0' + event);
        buf[2] = '\0';
    } else
        buf[0] = '\0';
}

/* Encodes ev per the kitty keyboard protocol, using the flags active on the
 * current screen. Bare modifier-key presses/releases (Shift, Ctrl, ...) are
 * dropped unless KITTY_KBD_REPORT_ALL is set, matching the reference
 * implementations. Adapted from foot's kitty_kbd_protocol(). */
static size_t
kitty_encode(const struct key_event *ev, const struct term *t,
             char out[static INPUT_MAX_SEQ]) {
    uint8_t flags = t->grid->kitty_kbd.flags[t->grid->kitty_kbd.idx];
    bool report_event = flags & KITTY_KBD_REPORT_EVENT;
    bool report_all = flags & KITTY_KBD_REPORT_ALL;
    bool report_alt = flags & KITTY_KBD_REPORT_ALTERNATE;
    bool report_text = flags & KITTY_KBD_REPORT_ASSOCIATED;

    if (ev->action == KEY_RELEASE && !report_event)
        return 0;

    const struct kitty_key_data *info = kitty_lookup(ev->sym);
    if (info != NULL && info->is_modifier && !report_all)
        return 0;

    bool plain_mods = (ev->mods & ~MOD_SHIFT) == 0;
    if (!report_all && info == NULL && plain_mods && ev->action != KEY_RELEASE &&
        ev->utf8_len > 0) {
        memcpy(out, ev->utf8, (size_t)ev->utf8_len);
        return (size_t)ev->utf8_len;
    }

    char final = info != NULL ? info->final : 'u';
    int mods = mod_param(ev->mods);
    int event = ev->action == KEY_REPEAT ? 2 : ev->action == KEY_RELEASE ? 3
                                                                         : 1;
    bool emit_mods = mods > 1 || (report_event && event != 1);
    char event_buf[3];
    kitty_event_suffix(event_buf, report_event, event);

    char buf[80];
    size_t n;

    if (final != 'u' && final != '~') {
        /* Arrows, Home/End, F1/F2/F4: legacy CSI form, no numeric key code */
        n = emit_mods ? (size_t)snprintf(buf, sizeof(buf), "\033[1;%d%s%c", mods,
                                         event_buf, final)
                      : (size_t)snprintf(buf, sizeof(buf), "\033[%c", final);
    } else {
        /* ev->utf8 has Ctrl's transformation applied (see backend.h), which
         * would corrupt the reported code point (e.g. ctrl+a -> 1, not 97);
         * the keysym itself is untouched by Ctrl, so prefer converting it. */
        uint32_t shifted_cp = xkb_keysym_to_utf32(ev->sym);
        if (shifted_cp == 0 && ev->utf8_len > 0)
            shifted_cp = utf8_first_cp(ev->utf8, ev->utf8_len);
        uint32_t key = info != NULL ? info->code : shifted_cp;
        if (key == 0)
            return 0;

        if (info == NULL) {
            uint32_t unshifted_cp = xkb_keysym_to_utf32(ev->unshifted);
            if (unshifted_cp != 0 && is_printable_cp(unshifted_cp))
                key = unshifted_cp;
        }

        n = (size_t)snprintf(buf, sizeof(buf), "\033[%u", key);

        /* Only text keys have shifted/base alternates; xkb maps functional keys
         * such as Delete to control code points (U+007F) that would leak here. */
        if (report_alt && info == NULL) {
            uint32_t base_cp = xkb_keysym_to_utf32(ev->base);
            bool emit_alt = shifted_cp != 0 && shifted_cp != key;
            bool emit_base = base_cp != 0 && base_cp != key && base_cp != shifted_cp &&
                             is_printable_cp(base_cp);
            if (emit_alt)
                n += (size_t)snprintf(buf + n, sizeof(buf) - n, ":%u", shifted_cp);
            if (emit_base)
                n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s:%u",
                                      emit_alt ? "" : ":", base_cp);
        }

        uint32_t text_cp = report_text && ev->action != KEY_RELEASE && is_printable_text(ev)
                               ? utf8_first_cp(ev->utf8, ev->utf8_len)
                               : 0;

        if (emit_mods)
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, ";%d%s", mods, event_buf);
        if (text_cp != 0)
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s;%u",
                                  emit_mods ? "" : ";", text_cp);

        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%c", final);
    }

    if (n == 0 || n >= INPUT_MAX_SEQ)
        return 0;
    memcpy(out, buf, n);
    return n;
}

/* ---- mouse reporting (CSI M ... / CSI < ... M/m) ---- */

static int
mouse_mod_bits(unsigned mods) {
    return ((mods & MOD_SHIFT) ? 4 : 0) | ((mods & MOD_ALT) ? 8 : 0) |
           ((mods & MOD_CTRL) ? 16 : 0);
}

size_t
input_mouse_encode(int button, bool pressed, bool motion, int col, int row,
                   unsigned mods, bool sgr, char out[static INPUT_MAX_SEQ]) {
    /* button is 1-based (1=left, 2=middle, 3=right; 0=none); the wire
     * encoding is 0-based with 3 as its own "no button" sentinel. */
    int cb = (sgr || pressed) ? (button == 0 ? 3 : button - 1) : 3;
    cb |= (motion ? 32 : 0) | mouse_mod_bits(mods);

    if (sgr)
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[<%d;%d;%d%c", cb, col + 1,
                                row + 1, pressed ? 'M' : 'm');

    int cx = MIN(col + 1, 223), cy = MIN(row + 1, 223);
    out[0] = '\033';
    out[1] = '[';
    out[2] = 'M';
    out[3] = (char)(cb + 32);
    out[4] = (char)(cx + 32);
    out[5] = (char)(cy + 32);
    return 6;
}

size_t
input_wheel_encode(int dir, int col, int row, unsigned mods, bool sgr,
                   char out[static INPUT_MAX_SEQ]) {
    int cb = 64 + dir + mouse_mod_bits(mods);

    if (sgr)
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[<%d;%d;%dM", cb, col + 1, row + 1);

    int cx = MIN(col + 1, 223), cy = MIN(row + 1, 223);
    out[0] = '\033';
    out[1] = '[';
    out[2] = 'M';
    out[3] = (char)(cb + 32);
    out[4] = (char)(cx + 32);
    out[5] = (char)(cy + 32);
    return 6;
}

size_t
input_encode_arrow(bool up, bool app_cursor, char out[static INPUT_MAX_SEQ]) {
    return letter_key(out, up ? 'A' : 'B', 0, app_cursor);
}

size_t
input_encode(const struct key_event *ev, const struct term *t,
             char out[static INPUT_MAX_SEQ]) {
    if (t->grid->kitty_kbd.flags[t->grid->kitty_kbd.idx] != 0)
        return kitty_encode(ev, t, out);

    if (ev->action == KEY_RELEASE)
        return 0;

    unsigned mods = ev->mods;
    bool app_cursor = t->modes.app_cursor_keys;

    switch (ev->sym) {
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
        return letter_key(out, 'A', mods, app_cursor);
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
        return letter_key(out, 'B', mods, app_cursor);
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        return letter_key(out, 'C', mods, app_cursor);
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        return letter_key(out, 'D', mods, app_cursor);
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        return letter_key(out, 'H', mods, app_cursor);
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        return letter_key(out, 'F', mods, app_cursor);

    case XKB_KEY_F1:
        return letter_key(out, 'P', mods, true);
    case XKB_KEY_F2:
        return letter_key(out, 'Q', mods, true);
    case XKB_KEY_F3:
        return letter_key(out, 'R', mods, true);
    case XKB_KEY_F4:
        return letter_key(out, 'S', mods, true);

    case XKB_KEY_Insert:
    case XKB_KEY_KP_Insert:
        return tilde_key(out, 2, mods);
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        return tilde_key(out, 3, mods);
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
        return tilde_key(out, 5, mods);
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
        return tilde_key(out, 6, mods);
    case XKB_KEY_F5:
        return tilde_key(out, 15, mods);
    case XKB_KEY_F6:
        return tilde_key(out, 17, mods);
    case XKB_KEY_F7:
        return tilde_key(out, 18, mods);
    case XKB_KEY_F8:
        return tilde_key(out, 19, mods);
    case XKB_KEY_F9:
        return tilde_key(out, 20, mods);
    case XKB_KEY_F10:
        return tilde_key(out, 21, mods);
    case XKB_KEY_F11:
        return tilde_key(out, 23, mods);
    case XKB_KEY_F12:
        return tilde_key(out, 24, mods);

    case XKB_KEY_BackSpace:
        if (mods & MOD_CTRL)
            return with_alt(out, mods, "\b", 1);
        return with_alt(out, mods, "\x7f", 1);

    case XKB_KEY_ISO_Left_Tab:
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[Z");
    case XKB_KEY_Tab:
        if (mods & MOD_SHIFT)
            return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033[Z");
        return with_alt(out, mods, "\t", 1);

    case XKB_KEY_Return:
        return with_alt(out, mods, "\r", 1);
    case XKB_KEY_KP_Enter:
        if (t->modes.app_keypad)
            return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033OM");
        return with_alt(out, mods, "\r", 1);

    case XKB_KEY_Escape:
        return with_alt(out, mods, "\033", 1);

    case XKB_KEY_space:
        if (mods & MOD_CTRL)
            return with_alt(out, mods, "", 1); /* NUL */
        break;
    }

    if (ev->sym >= XKB_KEY_KP_0 && ev->sym <= XKB_KEY_KP_9 && t->modes.app_keypad &&
        !(mods & ~MOD_SHIFT))
        return (size_t)snprintf(out, INPUT_MAX_SEQ, "\033O%c",
                                'p' + (int)(ev->sym - XKB_KEY_KP_0));

    if (ev->utf8_len > 0)
        return with_alt(out, mods, ev->utf8, (size_t)ev->utf8_len);

    return 0;
}
