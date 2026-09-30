#include "term/hyperlink.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "hyperlink"
#include "core/util.h"

void hyperlink_init(struct hyperlink_table *tbl) {
    *tbl = (struct hyperlink_table){0};
}

static void
free_links(struct hyperlink_table *tbl) {
    for (uint32_t i = 0; i < tbl->count; i++) {
        free(tbl->links[i].id);
        free(tbl->links[i].uri);
    }
    tbl->count = 0;
}

void hyperlink_destroy(struct hyperlink_table *tbl) {
    free_links(tbl);
    free(tbl->links);
    free(tbl->slots);
    *tbl = (struct hyperlink_table){0};
}

void hyperlink_clear(struct hyperlink_table *tbl) {
    free_links(tbl);
    if (tbl->slots != NULL)
        memset(tbl->slots, 0, tbl->slot_cap * sizeof(tbl->slots[0]));
}

static uint32_t
link_hash(const char *id, const char *uri) {
    uint32_t h = 2166136261u;
    for (const char *p = id; *p != '\0'; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0xff) * 16777619u; /* separator: ("a","b") != ("ab","") */
    for (const char *p = uri; *p != '\0'; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static void
slots_insert(struct hyperlink_table *tbl, uint32_t index) {
    uint32_t mask = tbl->slot_cap - 1;
    uint32_t i = link_hash(tbl->links[index].id, tbl->links[index].uri) & mask;
    while (tbl->slots[i] != 0)
        i = (i + 1) & mask;
    tbl->slots[i] = index + 1;
}

static void
slots_grow(struct hyperlink_table *tbl) {
    free(tbl->slots);
    tbl->slot_cap = tbl->slot_cap ? tbl->slot_cap * 2 : 256;
    tbl->slots = xcalloc(tbl->slot_cap, sizeof(tbl->slots[0]));
    for (uint32_t i = 0; i < tbl->count; i++)
        slots_insert(tbl, i);
}

uint16_t hyperlink_intern(struct hyperlink_table *tbl, const char *id, const char *uri) {
    if (strlen(uri) > HYPERLINK_MAX_URI || strlen(id) > HYPERLINK_MAX_ID)
        return 0;

    if (tbl->slot_cap != 0) {
        uint32_t mask = tbl->slot_cap - 1;
        for (uint32_t i = link_hash(id, uri) & mask; tbl->slots[i] != 0; i = (i + 1) & mask) {
            const struct hyperlink *l = &tbl->links[tbl->slots[i] - 1];
            if (strcmp(l->id, id) == 0 && strcmp(l->uri, uri) == 0)
                return (uint16_t)tbl->slots[i];
        }
    }

    if (tbl->count >= HYPERLINK_MAX_LINKS)
        return 0;

    if (tbl->count == tbl->cap) {
        tbl->cap = tbl->cap ? tbl->cap * 2 : 64;
        tbl->links = xrealloc(tbl->links, tbl->cap * sizeof(tbl->links[0]));
    }
    tbl->links[tbl->count] = (struct hyperlink){.id = xstrdup(id), .uri = xstrdup(uri)};
    tbl->count++;

    /* Keep the hash at most half full */
    if (tbl->count * 2 > tbl->slot_cap)
        slots_grow(tbl);
    else
        slots_insert(tbl, tbl->count - 1);

    return (uint16_t)tbl->count;
}

const struct hyperlink *
hyperlink_get(const struct hyperlink_table *tbl, uint16_t handle) {
    if (handle == 0 || handle > tbl->count)
        return NULL;
    return &tbl->links[handle - 1];
}
