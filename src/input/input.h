#pragma once

#include <stddef.h>

#include "backend/backend.h"
#include "term/term.h"

#define INPUT_MAX_SEQ 64

/* Encodes a key event for the application: legacy encoding, or the
 * kitty keyboard protocol when t->grid->kitty_kbd has flags pushed/set.
 * Returns the number of bytes written to out; 0 if the key sends nothing. */
size_t input_encode(const struct key_event *ev, const struct term *t,
                    char out[static INPUT_MAX_SEQ]);

/* Mouse press/release/motion: X10 (CSI M ...) or SGR (CSI < ... M/m) encoding.
 * button: 0 = none (a no-button motion report), 1-3 = left/middle/right. */
size_t input_mouse_encode(int button, bool pressed, bool motion, int col, int row,
                          unsigned mods, bool sgr, char out[static INPUT_MAX_SEQ]);

/* Wheel: buttons 64-67. dir: 0=up, 1=down, 2=left, 3=right. */
size_t input_wheel_encode(int dir, int col, int row, unsigned mods, bool sgr,
                          char out[static INPUT_MAX_SEQ]);

/* Plain Up/Down arrow, for 1007 alternate-scroll's fallback. */
size_t input_encode_arrow(bool up, bool app_cursor, char out[static INPUT_MAX_SEQ]);
