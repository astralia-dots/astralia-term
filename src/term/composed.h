#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "term/grid.h"

/* Cells holding a base character plus combining characters store
 * COMPOSED_BASE + index into a struct composed_table. */
#define COMPOSED_BASE (CELL_SPACER + 1)
#define COMPOSED_MAX_CPS 8
#define COMPOSED_MAX_CHAINS 65535

struct composed_chain {
    uint32_t cps[COMPOSED_MAX_CPS];
    uint8_t count;
};

struct composed_table {
    struct composed_chain *chains;
    uint32_t count, cap;
    uint32_t *slots; /* open-addressing hash: chain index + 1, 0 = empty */
    uint32_t slot_cap;
};

static inline bool
cp_is_composed(uint32_t cp) {
    return cp >= COMPOSED_BASE;
}

void composed_init(struct composed_table *tbl);
void composed_destroy(struct composed_table *tbl);
void composed_clear(struct composed_table *tbl);

/* Returns the cell value for cell_cp followed by comb, or 0 when the chain
 * is full or the table is exhausted (the caller then drops comb). */
uint32_t composed_add(struct composed_table *tbl, uint32_t cell_cp, uint32_t comb);

/* NULL if cp is not a composed value from this table. */
const struct composed_chain *composed_get(const struct composed_table *tbl, uint32_t cp);
