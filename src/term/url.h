#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "term/selection.h"

struct term;

/* Link covering live-row-relative (row, col): an OSC 8 run, else a plain-text
 * URL, soft-wrapped rows joined; none on the cursor's line (the shell input)
 * of the normal screen. range is inclusive. uri may be NULL; *uri is
 * the caller's to free. */
bool url_at(struct term *t, int row, int col, struct grid_range *range, char **uri);

/* mask[col] (t->cols entries): whether that cell of live-row-relative row
 * belongs to any link, for the always-on underline. */
void url_row_links(struct term *t, int row, bool *mask);

/* Marks every row of the soft-wrapped line around row dirty. */
void url_mark_line_dirty(struct term *t, int row);

/* Highlights the link at on-screen (row, col), marking changed rows dirty;
 * an out-of-screen position clears it. */
void url_hover(struct term *t, int row, int col);
