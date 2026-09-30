#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "term/grid.h"

struct term;

enum selection_kind {
    SEL_CHAR,
    SEL_WORD,
    SEL_LINE,
};

struct grid_range {
    struct grid_point start, end;
};

struct selection {
    enum selection_kind kind;
    bool active;              /* coords holds something to highlight/extract */
    bool ongoing;             /* mouse button still held */
    struct grid_range pivot;  /* anchor: click point (char), or the word/line under it */
    struct grid_range coords; /* current effective range, start <= end in reading order */
};

void selection_start(struct term *t, int col, int row, enum selection_kind kind);
void selection_update(struct term *t, int col, int row); /* extend while dragging */
void selection_finish(struct term *t);                   /* button released */
void selection_clear(struct term *t);                    /* cancel outright */

/* Rows top..bottom are about to change: clear the selection if it touches them. */
void selection_on_rows(struct term *t, int top, int bottom);

/* Column span of live-row-relative row within the current selection. */
bool selection_row_span(const struct term *t, int row, int *from, int *to);

/* UTF-8 text of the current selection, or NULL if none; caller frees. */
char *selection_to_text(const struct term *t, size_t *len);

/* A full-screen scroll pushed n lines into scrollback: shift stored rows
 * by -n, clearing the selection once it scrolls out of the ring. */
void selection_scroll(struct term *t, int n);
