#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct term; /* term.h includes this header; avoid the cycle with a forward decl */

/* Decoded-pixel budget across all stored images (Goal 2's 64 MB target). */
#define GRAPHICS_QUOTA_BYTES ((size_t)64 << 20)

struct graphics_image {
    uint32_t id;
    uint32_t number; /* 'I=' image number, 0 if none */
    uint64_t seq;    /* insertion order, so a number resolves to the newest image */
    int width, height;
    uint8_t *rgba; /* width * height * 4 bytes, straight alpha, row-major */
    size_t bytes;  /* == (size_t)width * height * 4 */

    struct graphics_image *hash_next;
    struct graphics_image *lru_prev, *lru_next; /* lru_head is most recently used */
};

/* Source rectangle of an image, in pixels. w <= 0 or h <= 0 means the whole image. */
struct graphics_src_rect {
    int x, y, w, h;
};

/* A placement: either virtual (a=p,U=1), the cols x rows grid a
 * Unicode-placeholder run divides an image's pixels into; or non-virtual
 * (a=p/a=T without U=1, or a decoded sixel), whose cols x rows cells were
 * written directly into the grid at creation time (see
 * graphics_place_nonvirtual()). placement_id is the client's p= key (0 = none)
 * and is only unique per image. A non-virtual placement also gets a store-wide
 * unique handle, which its ATTR_IMAGE cells carry in place of an image ID (see
 * graphics_placement_get_nonvirtual). src_* is always a resolved, in-bounds
 * rectangle of the image. */
struct graphics_placement {
    uint32_t image_id, placement_id;
    uint32_t handle; /* non_virtual only */
    int cols, rows;
    int src_x, src_y, src_w, src_h;
    bool non_virtual;
    struct graphics_placement *hash_next;
};

struct graphics_chunk;

struct graphics_store {
    struct graphics_image **buckets;
    size_t bucket_count;
    size_t total_bytes;
    struct graphics_image *lru_head, *lru_tail;

    struct graphics_placement **placements;
    size_t placement_bucket_count;

    /* In-progress chunked (m=1) transmission; NULL when idle. */
    struct graphics_chunk *chunk;

    /* Counter for internally-allocated image ids (sixel has no
     * client-assigned id), walking down from UINT32_MAX. */
    uint32_t next_internal_id;

    uint32_t next_handle;  /* next non-virtual placement handle */
    uint64_t next_seq;     /* next image insertion sequence number */
    uint32_t last_image_id; /* most recently stored image, for a=p without i=/I= */
};

void graphics_init(struct graphics_store *g);
void graphics_destroy(struct graphics_store *g);
void graphics_clear(struct graphics_store *g); /* frees every stored image */

/* Delivers a fully-formatted reply body, e.g. "i=7;OK" or "i=7;EINVAL:...".
 * The caller wraps it with the APC "ESC _G" prefix and "ESC \" terminator. */
typedef void (*graphics_reply_fn)(void *user, const char *text);

/* data is the APC payload as vt_parser hands it to the apc callback: starts
 * with 'G', NUL-terminated at data[len]. */
void graphics_apc(struct graphics_store *g, const uint8_t *data, size_t len,
                  graphics_reply_fn reply, void *user);

/* NULL if id isn't stored. Moves the image to the front of the LRU list. */
struct graphics_image *graphics_get(struct graphics_store *g, uint32_t id);

/* NULL if no virtual placement with that (image_id, placement_id) exists. */
struct graphics_placement *graphics_placement_get(struct graphics_store *g, uint32_t image_id,
                                                   uint32_t placement_id);

/* First virtual placement found for image_id, regardless of placement_id,
 * or NULL if it has none. Used when a placeholder cell's underline color
 * (placement ID) is unspecified/zero, per the protocol's "the terminal may
 * choose any virtual placement of the given image" fallback. */
struct graphics_placement *graphics_placement_get_any(struct graphics_store *g, uint32_t image_id);

/* NULL if no non-virtual placement with that handle exists. An ATTR_IMAGE cell
 * carries only a handle (no image ID), so this is the lookup render.c uses to
 * recover the image and the placement's cols/rows/source rectangle. */
struct graphics_placement *graphics_placement_get_nonvirtual(struct graphics_store *g,
                                                              uint32_t handle);

/* Registers an internally-decoded image (sixel) under a fresh, collision-free
 * id and returns it. Takes ownership of rgba (width * height * 4 bytes,
 * straight alpha). */
uint32_t graphics_store_insert(struct graphics_store *g, uint8_t *rgba, int width, int height);

/* Creates the non-virtual placement (image_id, placement_id) and writes it
 * into t's grid at the cursor, row by row, advancing the cursor exactly as
 * term_print() would for printed text: through the existing newline/scroll-at-
 * bottom-margin path, landing one column past the placement's right edge on
 * its last row. A nonzero placement_id that already exists for image_id is
 * replaced (its old cells are blanked). move_cursor == false (C=1) restores
 * the cursor to where the placement started. cols/rows are the caller's
 * already-resolved (defaulted, clamped to 255) placement size; src is
 * normalized against the image (zero rect = whole image). */
void graphics_place_nonvirtual(struct term *t, uint32_t image_id, uint32_t placement_id,
                               int cols, int rows, struct graphics_src_rect src, bool move_cursor);
