#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "term/grid.h"

/* OSC 8 hyperlinks. Cells written while a link is open carry ATTR_LINK and
 * the link's handle in tile_row (high byte) and tile_col (low byte). Handle 0
 * means no link. */
#define HYPERLINK_MAX_LINKS 65535
#define HYPERLINK_MAX_URI 2048
#define HYPERLINK_MAX_ID 256

struct hyperlink {
    char *id; /* the `id=` parameter, "" when absent */
    char *uri;
};

struct hyperlink_table {
    struct hyperlink *links;
    uint32_t count, cap;
    uint32_t *slots; /* open-addressing hash: link index + 1, 0 = empty */
    uint32_t slot_cap;
};

void hyperlink_init(struct hyperlink_table *tbl);
void hyperlink_destroy(struct hyperlink_table *tbl);
void hyperlink_clear(struct hyperlink_table *tbl);

/* Handle for (id, uri), deduplicated; 0 when the strings are too long or the
 * table is full (the caller then writes cells without a link). */
uint16_t hyperlink_intern(struct hyperlink_table *tbl, const char *id, const char *uri);

/* NULL if handle is 0 or not from this table. */
const struct hyperlink *hyperlink_get(const struct hyperlink_table *tbl, uint16_t handle);

/* Handle of the link a cell carries, 0 for none (image cells reuse the bytes). */
static inline uint16_t
cell_link(const struct cell *c) {
    if (!(c->attrs & ATTR_LINK) || (c->attrs & ATTR_IMAGE))
        return 0;
    return (uint16_t)((c->tile_row << 8) | c->tile_col);
}

/* Writes handle into a cell being created; 0 leaves it link-free. */
static inline void
cell_set_link(struct cell *c, uint16_t handle) {
    if (handle == 0)
        return;
    c->attrs |= ATTR_LINK;
    c->tile_row = (uint8_t)(handle >> 8);
    c->tile_col = (uint8_t)(handle & 0xff);
}
