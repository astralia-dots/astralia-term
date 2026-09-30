#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

/* Kitty keyboard protocol key codes and legacy final letters, adapted from
 * foot's kitty-keymap.h (Copyright (c) 2019 Daniel Eklöf, MIT; see LICENSE).
 * `final` is the CSI final byte kept for backward compatibility ('~' or a
 * letter); 'u' means the key has no legacy form. Must stay sorted on `sym`
 * for the bsearch() in input.c. */
struct kitty_key_data {
    xkb_keysym_t sym;
    uint16_t code;
    char final;
    bool is_modifier;
};

static const struct kitty_key_data kitty_keymap[] = {
    {XKB_KEY_ISO_Left_Tab, 9, 'u', false},
    {XKB_KEY_BackSpace, 127, 'u', false},
    {XKB_KEY_Tab, 9, 'u', false},
    {XKB_KEY_Return, 13, 'u', false},
    {XKB_KEY_Escape, 27, 'u', false},
    {XKB_KEY_Home, 1, 'H', false},
    {XKB_KEY_Left, 1, 'D', false},
    {XKB_KEY_Up, 1, 'A', false},
    {XKB_KEY_Right, 1, 'C', false},
    {XKB_KEY_Down, 1, 'B', false},
    {XKB_KEY_Page_Up, 5, '~', false},
    {XKB_KEY_Page_Down, 6, '~', false},
    {XKB_KEY_End, 1, 'F', false},
    {XKB_KEY_Insert, 2, '~', false},

    {XKB_KEY_KP_Enter, 57414, 'u', false},
    {XKB_KEY_KP_Home, 57423, 'u', false},
    {XKB_KEY_KP_Left, 57417, 'u', false},
    {XKB_KEY_KP_Up, 57419, 'u', false},
    {XKB_KEY_KP_Right, 57418, 'u', false},
    {XKB_KEY_KP_Down, 57420, 'u', false},
    {XKB_KEY_KP_Page_Up, 57421, 'u', false},
    {XKB_KEY_KP_Page_Down, 57422, 'u', false},
    {XKB_KEY_KP_End, 57424, 'u', false},
    {XKB_KEY_KP_Insert, 57425, 'u', false},
    {XKB_KEY_KP_Delete, 57426, 'u', false},
    {XKB_KEY_KP_Multiply, 57411, 'u', false},
    {XKB_KEY_KP_Add, 57413, 'u', false},
    {XKB_KEY_KP_Subtract, 57412, 'u', false},
    {XKB_KEY_KP_Decimal, 57409, 'u', false},
    {XKB_KEY_KP_Divide, 57410, 'u', false},
    {XKB_KEY_KP_0, 57399, 'u', false},
    {XKB_KEY_KP_1, 57400, 'u', false},
    {XKB_KEY_KP_2, 57401, 'u', false},
    {XKB_KEY_KP_3, 57402, 'u', false},
    {XKB_KEY_KP_4, 57403, 'u', false},
    {XKB_KEY_KP_5, 57404, 'u', false},
    {XKB_KEY_KP_6, 57405, 'u', false},
    {XKB_KEY_KP_7, 57406, 'u', false},
    {XKB_KEY_KP_8, 57407, 'u', false},
    {XKB_KEY_KP_9, 57408, 'u', false},

    {XKB_KEY_F1, 1, 'P', false},
    {XKB_KEY_F2, 1, 'Q', false},
    {XKB_KEY_F3, 13, '~', false},
    {XKB_KEY_F4, 1, 'S', false},
    {XKB_KEY_F5, 15, '~', false},
    {XKB_KEY_F6, 17, '~', false},
    {XKB_KEY_F7, 18, '~', false},
    {XKB_KEY_F8, 19, '~', false},
    {XKB_KEY_F9, 20, '~', false},
    {XKB_KEY_F10, 21, '~', false},
    {XKB_KEY_F11, 23, '~', false},
    {XKB_KEY_F12, 24, '~', false},

    {XKB_KEY_Shift_L, 57441, 'u', true},
    {XKB_KEY_Shift_R, 57447, 'u', true},
    {XKB_KEY_Control_L, 57442, 'u', true},
    {XKB_KEY_Control_R, 57448, 'u', true},
    {XKB_KEY_Caps_Lock, 57358, 'u', true},
    {XKB_KEY_Alt_L, 57443, 'u', true},
    {XKB_KEY_Alt_R, 57449, 'u', true},
    {XKB_KEY_Super_L, 57444, 'u', true},
    {XKB_KEY_Super_R, 57450, 'u', true},

    {XKB_KEY_Delete, 3, '~', false}, /* 0xffff: the largest keysym value here */
};
