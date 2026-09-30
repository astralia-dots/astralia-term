#include "term/composed.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "composed"
#include "core/util.h"

void composed_init(struct composed_table *tbl) {
    *tbl = (struct composed_table){0};
}

void composed_destroy(struct composed_table *tbl) {
    free(tbl->chains);
    free(tbl->slots);
    *tbl = (struct composed_table){0};
}

void composed_clear(struct composed_table *tbl) {
    tbl->count = 0;
    if (tbl->slots != NULL)
        memset(tbl->slots, 0, tbl->slot_cap * sizeof(tbl->slots[0]));
}

static uint32_t
chain_hash(const uint32_t *cps, int count) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < count; i++) {
        h ^= cps[i];
        h *= 16777619u;
    }
    return h;
}

static void
slots_insert(struct composed_table *tbl, uint32_t index) {
    const struct composed_chain *c = &tbl->chains[index];
    uint32_t mask = tbl->slot_cap - 1;
    uint32_t i = chain_hash(c->cps, c->count) & mask;
    while (tbl->slots[i] != 0)
        i = (i + 1) & mask;
    tbl->slots[i] = index + 1;
}

static void
slots_grow(struct composed_table *tbl) {
    free(tbl->slots);
    tbl->slot_cap = tbl->slot_cap ? tbl->slot_cap * 2 : 256;
    tbl->slots = xcalloc(tbl->slot_cap, sizeof(tbl->slots[0]));
    for (uint32_t i = 0; i < tbl->count; i++)
        slots_insert(tbl, i);
}

uint32_t composed_add(struct composed_table *tbl, uint32_t cell_cp, uint32_t comb) {
    uint32_t cps[COMPOSED_MAX_CPS];
    int count;

    const struct composed_chain *prev = composed_get(tbl, cell_cp);
    if (prev != NULL) {
        if (prev->count >= COMPOSED_MAX_CPS)
            return 0;
        memcpy(cps, prev->cps, prev->count * sizeof(cps[0]));
        count = prev->count;
    } else {
        cps[0] = cell_cp;
        count = 1;
    }
    cps[count++] = comb;

    if (tbl->slot_cap != 0) {
        uint32_t mask = tbl->slot_cap - 1;
        for (uint32_t i = chain_hash(cps, count) & mask; tbl->slots[i] != 0; i = (i + 1) & mask) {
            const struct composed_chain *c = &tbl->chains[tbl->slots[i] - 1];
            if (c->count == count && memcmp(c->cps, cps, count * sizeof(cps[0])) == 0)
                return COMPOSED_BASE + tbl->slots[i] - 1;
        }
    }

    if (tbl->count >= COMPOSED_MAX_CHAINS)
        return 0;

    if (tbl->count == tbl->cap) {
        tbl->cap = tbl->cap ? tbl->cap * 2 : 64;
        tbl->chains = xrealloc(tbl->chains, tbl->cap * sizeof(tbl->chains[0]));
    }
    struct composed_chain *c = &tbl->chains[tbl->count];
    memcpy(c->cps, cps, count * sizeof(cps[0]));
    c->count = (uint8_t)count;
    tbl->count++;

    /* Keep the hash at most half full */
    if (tbl->count * 2 > tbl->slot_cap)
        slots_grow(tbl);
    else
        slots_insert(tbl, tbl->count - 1);

    return COMPOSED_BASE + tbl->count - 1;
}

const struct composed_chain *
composed_get(const struct composed_table *tbl, uint32_t cp) {
    if (!cp_is_composed(cp) || cp - COMPOSED_BASE >= tbl->count)
        return NULL;
    return &tbl->chains[cp - COMPOSED_BASE];
}
