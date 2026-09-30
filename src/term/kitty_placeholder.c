#include "term/kitty_placeholder.h"

#include "term/kitty_diacritics.h"
#include "term/term.h"

struct kitty_placeholder_cell
kitty_placeholder_decode(const struct term *t, uint32_t cp) {
    struct kitty_placeholder_cell pc = {0};
    const struct composed_chain *chain = composed_get(&t->composed, cp);
    uint32_t base = chain != NULL ? chain->cps[0] : cp;
    if (base != KITTY_PLACEHOLDER_CP)
        return pc;

    pc.is_placeholder = true;
    if (chain == NULL)
        return pc;

    for (int i = 1; i < chain->count && i <= 3; i++) {
        int idx = kitty_diacritic_index(chain->cps[i]);
        if (idx < 0)
            continue;
        switch (i) {
        case 1:
            pc.have_row = true;
            pc.row = idx;
            break;
        case 2:
            pc.have_col = true;
            pc.col = idx;
            break;
        case 3:
            pc.have_msb = true;
            pc.msb = idx;
            break;
        }
    }
    return pc;
}

bool
kitty_placeholder_resolve(struct kitty_placeholder_run *run,
                          const struct kitty_placeholder_cell *pc, uint32_t fg, uint32_t ul,
                          int *out_row, int *out_col, int *out_msb) {
    bool have_row = pc->have_row, have_col = pc->have_col, have_msb = pc->have_msb;
    int row = pc->row, col = pc->col, msb = pc->msb;
    bool same = run->valid && run->fg == fg && run->ul == ul;

    if (!have_row && !have_col && !have_msb && same) {
        row = run->row;
        col = run->col + 1;
        msb = run->msb;
        have_row = have_col = have_msb = true;
    } else if (have_row && !have_col && !have_msb) {
        if (same && run->row == row) {
            col = run->col + 1;
            msb = run->msb;
        } else {
            /* No matching predecessor to inherit from: a fresh run starting
             * at this row. The protocol's prose only documents inheriting
             * from a matching left neighbor, but its own worked example
             * (row diacritic given, column omitted, on the first cell of
             * each printed line) only renders if column defaults to 0
             * here; the prose doesn't call this out explicitly. */
            col = 0;
        }
        have_col = have_msb = true;
    } else if (have_row && have_col && !have_msb && same && run->row == row &&
              run->col + 1 == col) {
        msb = run->msb;
        have_msb = true;
    }

    if (!have_row || !have_col) {
        run->valid = false;
        return false;
    }

    *out_row = row;
    *out_col = col;
    *out_msb = have_msb ? msb : 0;
    run->valid = true;
    run->fg = fg;
    run->ul = ul;
    run->row = row;
    run->col = col;
    run->msb = *out_msb;
    return true;
}
