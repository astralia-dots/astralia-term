#pragma once

#include <stdbool.h>
#include <stdint.h>

struct term;

/* Kitty graphics protocol: Unicode placeholder base codepoint (U+10EEEE). */
#define KITTY_PLACEHOLDER_CP 0x10EEEEu

/* Decoded row/col/MSB diacritics for one placeholder cell (the placeholder
 * base plus up to 3 combining marks). have_row/have_col/have_msb are false
 * when that diacritic was omitted on this cell (see the left-neighbor
 * inheritance rule in kitty_placeholder_resolve()). */
struct kitty_placeholder_cell {
    bool is_placeholder;
    bool have_row, have_col, have_msb;
    int row, col, msb;
};

/* Decodes cp (a cell's struct cell.cp) as a placeholder base codepoint plus
 * any attached row/column/MSB diacritics, via the term's composed-character
 * table. is_placeholder is false, and every other field zero, if cp isn't
 * U+10EEEE (with or without diacritics attached). */
struct kitty_placeholder_cell kitty_placeholder_decode(const struct term *t, uint32_t cp);

/* Left-to-right scan state used to fill in diacritics a placeholder cell
 * omitted, per the graphics protocol's inheritance rule (missing diacritics
 * inherit from the placeholder cell to the left when its foreground and
 * underline colors match). Zero-initialize before the first cell of a row. */
struct kitty_placeholder_run {
    bool valid;
    uint32_t fg, ul;
    int row, col, msb;
};

/* Resolves pc against run (the previous placeholder cell in this row, if
 * any), applying the graphics protocol's diacritic-omission rules, and
 * advances run to reflect this cell. fg/ul are the cell's raw (unresolved)
 * foreground/underline colors. Returns false if row/col still can't be
 * determined (in which case run is invalidated, so a broken run doesn't
 * propagate bad inheritance further right). */
bool kitty_placeholder_resolve(struct kitty_placeholder_run *run,
                               const struct kitty_placeholder_cell *pc, uint32_t fg, uint32_t ul,
                               int *out_row, int *out_col, int *out_msb);
