#include "term/graphics.h"

#include <fcntl.h>
#include <spng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define LOG_MODULE "graphics"
#include "core/base64.h"
#include "core/util.h"
#include "term/term.h"

#define GRAPHICS_BUCKETS 61
#define GRAPHICS_PLACEMENT_BUCKETS 31
#define GRAPHICS_MAX_FILE_BYTES (96u << 20) /* generous slack over the quota */

/* Parsed control data for one APC command (first chunk, or a single-shot
 * transmission). Continuation chunks only ever change more/quiet. */
struct cmd {
    char action;   /* 't', 'T', 'q', 'd', 'p', or 0 (not given: transmit) */
    char medium;   /* 'd' direct, 'f' file, 't' temp file, 's' shared memory */
    char compress; /* 'z' or 0 */
    char del;      /* 'd' key's value, for action == 'd' */
    int format;    /* 24, 32 or 100 */
    int width, height;
    uint32_t id;
    bool have_id;
    uint32_t number; /* 'I' key: image number, 0 if none */
    bool have_number;
    int quiet; /* 0, 1 or 2 */
    int more;  /* 'm' key: 0 or 1 */

    size_t data_size, data_offset; /* 'S=', 'O=': slice of a file / shm object */

    /* Placement: virtual (U=1) or at the cursor. */
    bool virtual_placement; /* 'U=1' */
    int cols, rows;         /* 'c=', 'r=': grid size in cells */
    uint32_t placement_id;  /* 'p='; 0 means unspecified */
    int x, y, w, h;         /* 'x=' 'y=' 'w=' 'h=': source rectangle; cell x/y for d=p/x/y */
    bool no_move;           /* 'C=1': leave the cursor where it was */
};

struct graphics_chunk {
    struct cmd cmd;
    char *b64;
    size_t b64_len, b64_cap;
};

void graphics_init(struct graphics_store *g) {
    *g = (struct graphics_store){
        .buckets = xcalloc(GRAPHICS_BUCKETS, sizeof(struct graphics_image *)),
        .bucket_count = GRAPHICS_BUCKETS,
        .placements = xcalloc(GRAPHICS_PLACEMENT_BUCKETS, sizeof(struct graphics_placement *)),
        .placement_bucket_count = GRAPHICS_PLACEMENT_BUCKETS,
        .next_internal_id = UINT32_MAX,
        .next_handle = 1,
    };
}

static void
free_image(struct graphics_image *img) {
    free(img->rgba);
    free(img);
}

static void
lru_unlink(struct graphics_store *g, struct graphics_image *img) {
    if (img->lru_prev != NULL)
        img->lru_prev->lru_next = img->lru_next;
    else
        g->lru_head = img->lru_next;
    if (img->lru_next != NULL)
        img->lru_next->lru_prev = img->lru_prev;
    else
        g->lru_tail = img->lru_prev;
}

static void
lru_push_front(struct graphics_store *g, struct graphics_image *img) {
    img->lru_prev = NULL;
    img->lru_next = g->lru_head;
    if (g->lru_head != NULL)
        g->lru_head->lru_prev = img;
    g->lru_head = img;
    if (g->lru_tail == NULL)
        g->lru_tail = img;
}

static struct graphics_image **
bucket_for(struct graphics_store *g, uint32_t id) {
    return &g->buckets[id % g->bucket_count];
}

static struct graphics_image *
store_find(struct graphics_store *g, uint32_t id) {
    for (struct graphics_image *img = *bucket_for(g, id); img != NULL; img = img->hash_next)
        if (img->id == id)
            return img;
    return NULL;
}

static void
store_unlink_hash(struct graphics_store *g, struct graphics_image *img) {
    struct graphics_image **slot = bucket_for(g, img->id);
    while (*slot != img)
        slot = &(*slot)->hash_next;
    *slot = img->hash_next;
}

/* ---- virtual placements ---- */

static size_t
placement_hash(uint32_t image_id, uint32_t placement_id) {
    return (size_t)(image_id * 2654435761u) ^ placement_id;
}

static struct graphics_placement **
placement_bucket(struct graphics_store *g, uint32_t image_id, uint32_t placement_id) {
    return &g->placements[placement_hash(image_id, placement_id) % g->placement_bucket_count];
}

struct graphics_placement *
graphics_placement_get(struct graphics_store *g, uint32_t image_id, uint32_t placement_id) {
    for (struct graphics_placement *p = *placement_bucket(g, image_id, placement_id); p != NULL;
         p = p->hash_next)
        if (!p->non_virtual && p->image_id == image_id && p->placement_id == placement_id)
            return p;
    return NULL;
}

struct graphics_placement *
graphics_placement_get_any(struct graphics_store *g, uint32_t image_id) {
    for (size_t b = 0; b < g->placement_bucket_count; b++)
        for (struct graphics_placement *p = g->placements[b]; p != NULL; p = p->hash_next)
            if (!p->non_virtual && p->image_id == image_id)
                return p;
    return NULL;
}

/* Resolves src against img: clamps the origin into the image and defaults a
 * missing width/height to the rest of the image. */
static struct graphics_src_rect
normalize_src(const struct graphics_image *img, struct graphics_src_rect r) {
    r.x = CLAMP(r.x, 0, img->width - 1);
    r.y = CLAMP(r.y, 0, img->height - 1);
    if (r.w <= 0 || r.w > img->width - r.x)
        r.w = img->width - r.x;
    if (r.h <= 0 || r.h > img->height - r.y)
        r.h = img->height - r.y;
    return r;
}

/* Creates or redefines the virtual placement for (image_id, placement_id). */
static void
placement_insert(struct graphics_store *g, const struct graphics_image *img, uint32_t placement_id,
                 int cols, int rows, struct graphics_src_rect src) {
    src = normalize_src(img, src);
    struct graphics_placement *p = graphics_placement_get(g, img->id, placement_id);
    if (p == NULL) {
        p = xmalloc(sizeof(*p));
        *p = (struct graphics_placement){.image_id = img->id, .placement_id = placement_id};
        struct graphics_placement **slot = placement_bucket(g, img->id, placement_id);
        p->hash_next = *slot;
        *slot = p;
    }
    p->cols = cols;
    p->rows = rows;
    p->src_x = src.x;
    p->src_y = src.y;
    p->src_w = src.w;
    p->src_h = src.h;
}

/* Non-virtual placements are indexed under the (0, handle) bucket, a namespace
 * disjoint from any real (nonzero) image_id, so they can be found from an
 * ATTR_IMAGE cell's handle alone. p->image_id keeps the real image id
 * regardless of which bucket the entry hashes into, so
 * remove_placements_for_image() (which scans every bucket) still finds it. */
static struct graphics_placement *
placement_insert_nonvirtual(struct graphics_store *g, uint32_t image_id, uint32_t placement_id,
                            int cols, int rows, struct graphics_src_rect src) {
    uint32_t handle = g->next_handle;
    g->next_handle = handle + 1 != 0 ? handle + 1 : 1;

    struct graphics_placement *p = xmalloc(sizeof(*p));
    *p = (struct graphics_placement){.image_id = image_id,
                                     .placement_id = placement_id,
                                     .handle = handle,
                                     .cols = cols,
                                     .rows = rows,
                                     .src_x = src.x,
                                     .src_y = src.y,
                                     .src_w = src.w,
                                     .src_h = src.h,
                                     .non_virtual = true};
    struct graphics_placement **slot = placement_bucket(g, 0, handle);
    p->hash_next = *slot;
    *slot = p;
    return p;
}

struct graphics_placement *
graphics_placement_get_nonvirtual(struct graphics_store *g, uint32_t handle) {
    for (struct graphics_placement *p = *placement_bucket(g, 0, handle); p != NULL;
         p = p->hash_next)
        if (p->non_virtual && p->handle == handle)
            return p;
    return NULL;
}

/* The client's placement IDs are only unique per image, so this scans. */
static struct graphics_placement *
find_nonvirtual_by_client(struct graphics_store *g, uint32_t image_id, uint32_t placement_id) {
    for (size_t b = 0; b < g->placement_bucket_count; b++)
        for (struct graphics_placement *p = g->placements[b]; p != NULL; p = p->hash_next)
            if (p->non_virtual && p->image_id == image_id && p->placement_id == placement_id)
                return p;
    return NULL;
}

static void
placement_free(struct graphics_store *g, struct graphics_placement *p) {
    struct graphics_placement **slot = p->non_virtual ? placement_bucket(g, 0, p->handle)
                                                      : placement_bucket(g, p->image_id, p->placement_id);
    while (*slot != p)
        slot = &(*slot)->hash_next;
    *slot = p->hash_next;
    free(p);
}

/* Blanks every ATTR_IMAGE cell (in both screens' grids, scrollback
 * included) carrying handle, so deleting a placement is correct even for rows
 * currently scrolled out of view. */
static void
blank_cells_for_handle(struct term *t, uint32_t handle) {
    struct grid *grids[2] = {&t->normal, &t->alt};
    for (int s = 0; s < 2; s++) {
        struct grid *gr = grids[s];
        for (int i = 0; i < gr->num_lines; i++) {
            struct row *row = &gr->lines[i];
            if (row->cells == NULL)
                continue;
            for (int col = 0; col < gr->cols; col++) {
                struct cell *cell = &row->cells[col];
                if ((cell->attrs & ATTR_IMAGE) && cell->ul == handle) {
                    *cell = (struct cell){0};
                    row->dirty = true;
                }
            }
        }
    }
}

/* Defaults cols/rows (when either is <= 0) from the source rectangle's pixel
 * size and the terminal's cell size, then clamps to [1, 255]: tile_row/tile_col
 * are uint8_t, and a placement wider or taller than that has no real use case. */
static void
resolve_placement_size(const struct term *t, struct graphics_src_rect src, int in_cols, int in_rows,
                       int *out_cols, int *out_rows) {
    int cols = in_cols, rows = in_rows;
    if (cols <= 0)
        cols = (src.w + t->cell_width - 1) / t->cell_width;
    if (rows <= 0)
        rows = (src.h + t->cell_height - 1) / t->cell_height;
    *out_cols = CLAMP(cols, 1, 255);
    *out_rows = CLAMP(rows, 1, 255);
}

void graphics_place_nonvirtual(struct term *t, uint32_t image_id, uint32_t placement_id, int cols,
                               int rows, struct graphics_src_rect src, bool move_cursor) {
    struct graphics_store *g = &t->graphics;
    struct graphics_image *img = store_find(g, image_id);
    if (img == NULL)
        return;

    if (placement_id != 0) {
        struct graphics_placement *old = find_nonvirtual_by_client(g, image_id, placement_id);
        if (old != NULL) {
            blank_cells_for_handle(t, old->handle);
            placement_free(g, old);
        }
    }
    uint32_t handle =
        placement_insert_nonvirtual(g, image_id, placement_id, cols, rows, normalize_src(img, src))
            ->handle;

    int start_col = t->cursor.col;
    for (int r = 0; r < rows; r++) {
        selection_on_rows(t, t->cursor.row, t->cursor.row);
        struct row *row = grid_row(t->grid, t->cursor.row);
        int end_col = MIN(start_col + cols, t->cols);
        for (int col = start_col; col < end_col; col++) {
            row->cells[col] = (struct cell){
                .ul = handle,
                .attrs = ATTR_IMAGE,
                .tile_row = (uint8_t)r,
                .tile_col = (uint8_t)(col - start_col),
            };
        }
        row->dirty = true;
        if (r < rows - 1)
            term_index(t);
    }

    if (move_cursor) {
        t->cursor.col = MIN(start_col + cols, t->cols - 1);
    } else {
        /* The last row is where scrolling (if any) left the cursor, so counting
         * back rows - 1 lands on the placement's first row either way. */
        t->cursor.row = MAX(t->cursor.row - (rows - 1), 0);
        t->cursor.col = start_col;
    }
    t->cursor.wrap_pending = false;
    t->cursor_dirty = true;
}

/* Frees every placement of image_id, e.g. when its image is deleted or
 * evicted. */
static void
remove_placements_for_image(struct graphics_store *g, uint32_t image_id) {
    for (size_t b = 0; b < g->placement_bucket_count; b++) {
        struct graphics_placement **slot = &g->placements[b];
        while (*slot != NULL) {
            if ((*slot)->image_id == image_id) {
                struct graphics_placement *dead = *slot;
                *slot = dead->hash_next;
                free(dead);
            } else
                slot = &(*slot)->hash_next;
        }
    }
}

static void
store_remove(struct graphics_store *g, struct graphics_image *img) {
    store_unlink_hash(g, img);
    lru_unlink(g, img);
    g->total_bytes -= img->bytes;
    remove_placements_for_image(g, img->id);
    free_image(img);
}

void graphics_clear(struct graphics_store *g) {
    while (g->lru_head != NULL)
        store_remove(g, g->lru_head);
}

void graphics_destroy(struct graphics_store *g) {
    graphics_clear(g);
    free(g->buckets);
    g->buckets = NULL;
    free(g->placements);
    g->placements = NULL;
    if (g->chunk != NULL) {
        free(g->chunk->b64);
        free(g->chunk);
        g->chunk = NULL;
    }
}

struct graphics_image *
graphics_get(struct graphics_store *g, uint32_t id) {
    struct graphics_image *img = store_find(g, id);
    if (img != NULL) {
        lru_unlink(g, img);
        lru_push_front(g, img);
    }
    return img;
}

/* Evicts least-recently-used images until total_bytes fits the quota, never
 * touching keep (the image just inserted, already at the LRU head). */
static void
evict_to_quota(struct graphics_store *g, const struct graphics_image *keep) {
    while (g->total_bytes > GRAPHICS_QUOTA_BYTES && g->lru_tail != NULL &&
           g->lru_tail != keep)
        store_remove(g, g->lru_tail);
}

static void
store_insert(struct graphics_store *g, struct graphics_image *img) {
    struct graphics_image *old = store_find(g, img->id);
    if (old != NULL)
        store_remove(g, old);

    img->seq = g->next_seq++;
    g->last_image_id = img->id;
    struct graphics_image **slot = bucket_for(g, img->id);
    img->hash_next = *slot;
    *slot = img;
    lru_push_front(g, img);
    g->total_bytes += img->bytes;
    evict_to_quota(g, img);
}

/* A fresh image id no stored image uses: for sixel and for kitty images the
 * client did not name (anonymous, or addressed by image number). */
static uint32_t
alloc_internal_id(struct graphics_store *g) {
    uint32_t id;
    do {
        id = g->next_internal_id;
        g->next_internal_id = id != 0 ? id - 1 : UINT32_MAX;
    } while (id == 0 || store_find(g, id) != NULL);
    return id;
}

uint32_t
graphics_store_insert(struct graphics_store *g, uint8_t *rgba, int width, int height) {
    uint32_t id = alloc_internal_id(g);
    struct graphics_image *img = xmalloc(sizeof(*img));
    *img = (struct graphics_image){
        .id = id, .width = width, .height = height, .rgba = rgba, .bytes = (size_t)width * height * 4};
    store_insert(g, img);
    return id;
}

/* ---- replies ---- */

static void
reply(graphics_reply_fn fn, void *user, int quiet, bool ok, uint32_t id, uint32_t number,
      const char *code_msg) {
    /* Neither an id nor a number means the client never asked for a reply
     * (no i=/I= key); the real kitty protocol never replies in that case,
     * success or failure, since there is nothing to report back under.
     * Replying anyway leaves an unread response on the pty for the next
     * reader to misinterpret. */
    if (id == 0 && number == 0)
        return;
    if (ok && quiet == 1)
        return;
    if (!ok && quiet == 2)
        return;
    char text[256];
    size_t n = 0;
    if (id != 0)
        n += (size_t)snprintf(text + n, sizeof(text) - n, "i=%u", id);
    if (number != 0)
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%sI=%u", n > 0 ? "," : "", number);
    snprintf(text + n, sizeof(text) - n, ";%s", ok ? "OK" : code_msg);
    if (fn != NULL)
        fn(user, text);
}

static void
reply_ok(graphics_reply_fn fn, void *user, const struct cmd *c) {
    reply(fn, user, c->quiet, true, c->id, c->number, NULL);
}

static void
reply_err(graphics_reply_fn fn, void *user, const struct cmd *c, const char *code, const char *msg) {
    char code_msg[224];
    snprintf(code_msg, sizeof(code_msg), "%s:%s", code, msg);
    reply(fn, user, c->quiet, false, c->id, c->number, code_msg);
}

/* ---- control data parsing ---- */

static uint32_t
parse_uint(const char *s, size_t len) {
    uint32_t v = 0;
    for (size_t i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(s[i] - '0');
    return v;
}

/* Parses "k=v,k=v,..." (no trailing ';'); continuation chunks only ever set
 * m and q, but parsing the rest is harmless since callers ignore it there. */
static void
parse_control(const uint8_t *data, size_t len, struct cmd *c) {
    size_t i = 0;
    while (i + 1 < len && data[i + 1] == '=') {
        char key = (char)data[i];
        size_t vstart = i + 2;
        size_t vend = vstart;
        while (vend < len && data[vend] != ',')
            vend++;
        const char *val = (const char *)data + vstart;
        size_t vlen = vend - vstart;

        switch (key) {
        case 'a':
            if (vlen > 0)
                c->action = val[0];
            break;
        case 't':
            if (vlen > 0)
                c->medium = val[0];
            break;
        case 'o':
            if (vlen > 0)
                c->compress = val[0];
            break;
        case 'd':
            if (vlen > 0)
                c->del = val[0];
            break;
        case 'f':
            c->format = (int)parse_uint(val, vlen);
            break;
        case 's':
            c->width = (int)parse_uint(val, vlen);
            break;
        case 'v':
            c->height = (int)parse_uint(val, vlen);
            break;
        case 'i':
            c->id = parse_uint(val, vlen);
            c->have_id = true;
            break;
        case 'I':
            c->number = parse_uint(val, vlen);
            c->have_number = c->number != 0;
            break;
        case 'S':
            c->data_size = parse_uint(val, vlen);
            break;
        case 'O':
            c->data_offset = parse_uint(val, vlen);
            break;
        case 'x':
            c->x = (int)parse_uint(val, vlen);
            break;
        case 'y':
            c->y = (int)parse_uint(val, vlen);
            break;
        case 'w':
            c->w = (int)parse_uint(val, vlen);
            break;
        case 'h':
            c->h = (int)parse_uint(val, vlen);
            break;
        case 'C':
            c->no_move = parse_uint(val, vlen) == 1;
            break;
        case 'q':
            c->quiet = (int)parse_uint(val, vlen);
            break;
        case 'm':
            c->more = (int)parse_uint(val, vlen);
            break;
        case 'U':
            c->virtual_placement = parse_uint(val, vlen) == 1;
            break;
        case 'c':
            c->cols = (int)parse_uint(val, vlen);
            break;
        case 'r':
            c->rows = (int)parse_uint(val, vlen);
            break;
        case 'p':
            c->placement_id = parse_uint(val, vlen);
            break;
        default:
            break; /* z-layering, relative placement and animation keys: unsupported */
        }

        i = vend < len ? vend + 1 : vend;
    }
}

/* ---- payload decoding ---- */

static bool
has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Reads a regular file the client named. Device and pseudo-filesystem paths
 * are refused: the path comes from the program writing to the terminal, and
 * e.g. /dev/zero or /proc/self/mem must never be read on its behalf. */
static bool
read_whole_file(const char *path, uint8_t **out, size_t *out_len) {
    if (has_prefix(path, "/proc/") || has_prefix(path, "/sys/") ||
        (has_prefix(path, "/dev/") && !has_prefix(path, "/dev/shm/")))
        return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    size_t cap = 1 << 16, len = 0;
    uint8_t *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            if (cap >= GRAPHICS_MAX_FILE_BYTES) {
                free(buf);
                fclose(f);
                return false;
            }
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    bool ok = feof(f) && !ferror(f);
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = len;
    return true;
}

/* Copies the object behind a POSIX shared memory name, then unlinks it: the
 * client hands ownership of the object to the terminal (t=s). */
static bool
read_shm(const char *name, uint8_t **out, size_t *out_len) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = false;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= GRAPHICS_MAX_FILE_BYTES) {
        void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            *out = xmalloc((size_t)st.st_size);
            memcpy(*out, map, (size_t)st.st_size);
            *out_len = (size_t)st.st_size;
            munmap(map, (size_t)st.st_size);
            ok = true;
        }
    }
    close(fd);
    shm_unlink(name);
    return ok;
}

/* The temp-file medium (t=t) lets the terminal delete the file once read, so
 * only files that a client plainly made for this purpose may be deleted: in a
 * temp directory and named with the protocol's marker. */
static bool
is_deletable_temp_file(const char *path) {
    if (strstr(path, "tty-graphics-protocol") == NULL)
        return false;
    const char *tmpdir = getenv("TMPDIR");
    return has_prefix(path, "/tmp/") || has_prefix(path, "/dev/shm/") ||
           (tmpdir != NULL && tmpdir[0] == '/' && has_prefix(path, tmpdir));
}

/* Keeps only the [offset, offset + size) slice of buf (size 0: to the end),
 * as selected by the S=/O= keys. */
static bool
slice_data(uint8_t *buf, size_t *len, size_t offset, size_t size) {
    if (offset > *len)
        return false;
    size_t avail = *len - offset;
    size_t take = size != 0 && size < avail ? size : avail;
    if (offset != 0)
        memmove(buf, buf + offset, take);
    *len = take;
    return true;
}

/* Inflates a zlib (RFC 1950) stream into a buffer of exactly expected_len,
 * as used for raw pixel formats where the decompressed size is known
 * upfront from width/height/format. */
static bool
zlib_inflate_exact(const uint8_t *src, size_t src_len, uint8_t *dst, size_t expected_len) {
    uLongf dest_len = expected_len;
    int r = uncompress(dst, &dest_len, src, src_len);
    return r == Z_OK && dest_len == expected_len;
}

/* Inflates a zlib stream whose decompressed size is not known upfront
 * (compressed PNG payloads), refusing to grow past the file size cap. */
static bool
zlib_inflate_dynamic(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    z_stream zs = {.next_in = (Bytef *)src, .avail_in = (uInt)src_len};
    if (inflateInit(&zs) != Z_OK)
        return false;
    size_t cap = src_len * 4 + 4096, len = 0;
    uint8_t *buf = xmalloc(cap);
    int r;
    do {
        if (len == cap) {
            if (cap >= GRAPHICS_MAX_FILE_BYTES) {
                inflateEnd(&zs);
                free(buf);
                return false;
            }
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        zs.next_out = buf + len;
        zs.avail_out = (uInt)(cap - len);
        r = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
    } while (r == Z_OK);
    inflateEnd(&zs);
    if (r != Z_STREAM_END) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = len;
    return true;
}

static bool
decode_png(const uint8_t *data, size_t len, uint8_t **rgba, int *width, int *height) {
    spng_ctx *ctx = spng_ctx_new(0);
    if (ctx == NULL)
        return false;

    bool ok = false;
    if (spng_set_png_buffer(ctx, data, len) == 0) {
        struct spng_ihdr ihdr;
        size_t out_len;
        if (spng_get_ihdr(ctx, &ihdr) == 0 &&
            spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_len) == 0) {
            uint8_t *buf = xmalloc(out_len);
            if (spng_decode_image(ctx, buf, out_len, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS) == 0) {
                *rgba = buf;
                *width = (int)ihdr.width;
                *height = (int)ihdr.height;
                ok = true;
            } else
                free(buf);
        }
    }
    spng_ctx_free(ctx);
    return ok;
}

/* Expands packed 24-bit RGB into the straight-alpha RGBA the store keeps. */
static uint8_t *
expand_rgb(const uint8_t *rgb, int width, int height) {
    size_t n = (size_t)width * height;
    uint8_t *rgba = xmalloc(n * 4);
    for (size_t i = 0; i < n; i++) {
        rgba[4 * i + 0] = rgb[3 * i + 0];
        rgba[4 * i + 1] = rgb[3 * i + 1];
        rgba[4 * i + 2] = rgb[3 * i + 2];
        rgba[4 * i + 3] = 0xff;
    }
    return rgba;
}

/* Decodes enc/enc_len (already de-base64'd and, for t=f, read from disk)
 * per cmd's format/compression into a fresh RGBA buffer. */
static bool
decode_pixels(const struct cmd *c, const uint8_t *enc, size_t enc_len,
              uint8_t **rgba, int *width, int *height, const char **err_code, const char **err_msg) {
    if (c->format == 100) {
        uint8_t *inflated = NULL;
        size_t inflated_len = 0;
        if (c->compress == 'z') {
            if (!zlib_inflate_dynamic(enc, enc_len, &inflated, &inflated_len)) {
                *err_code = "EINVAL";
                *err_msg = "zlib inflate failed";
                return false;
            }
            enc = inflated;
            enc_len = inflated_len;
        }
        bool ok = decode_png(enc, enc_len, rgba, width, height);
        free(inflated);
        if (!ok) {
            *err_code = "EBADF";
            *err_msg = "invalid PNG data";
            return false;
        }
        return true;
    }

    if (c->format != 24 && c->format != 32) {
        *err_code = "EINVAL";
        *err_msg = "unsupported format";
        return false;
    }
    if (c->width <= 0 || c->height <= 0) {
        *err_code = "EINVAL";
        *err_msg = "missing width/height";
        return false;
    }

    int channels = c->format == 24 ? 3 : 4;
    size_t expected = (size_t)c->width * c->height * channels;

    uint8_t *raw;
    bool owns_raw;
    if (c->compress == 'z') {
        raw = xmalloc(expected);
        if (!zlib_inflate_exact(enc, enc_len, raw, expected)) {
            free(raw);
            *err_code = "EINVAL";
            *err_msg = "zlib inflate failed or size mismatch";
            return false;
        }
        owns_raw = true;
    } else {
        if (enc_len != expected) {
            *err_code = "EINVAL";
            *err_msg = "pixel data size mismatch";
            return false;
        }
        raw = (uint8_t *)enc;
        owns_raw = false;
    }

    *width = c->width;
    *height = c->height;
    *rgba = channels == 4 ? xmalloc(expected) : expand_rgb(raw, c->width, c->height);
    if (channels == 4)
        memcpy(*rgba, raw, expected);
    if (owns_raw)
        free(raw);
    return true;
}

/* ---- command execution ---- */

/* Which placements a delete command targets. */
struct placement_filter {
    bool all;
    uint32_t image_id; /* used unless all or have_handle */
    bool have_pid;
    uint32_t pid;
    bool have_handle; /* a single non-virtual placement, by handle */
    uint32_t handle;
};

static bool
filter_matches(const struct placement_filter *f, const struct graphics_placement *p) {
    if (f->all)
        return true;
    if (f->have_handle)
        return p->non_virtual && p->handle == f->handle;
    return p->image_id == f->image_id && (!f->have_pid || p->placement_id == f->pid);
}

static bool
image_has_placements(const struct graphics_store *g, uint32_t image_id) {
    for (size_t b = 0; b < g->placement_bucket_count; b++)
        for (const struct graphics_placement *p = g->placements[b]; p != NULL; p = p->hash_next)
            if (p->image_id == image_id)
                return true;
    return false;
}

/* Deletes every placement f matches, blanking the grid cells of non-virtual
 * ones. With free_images, an image that no placement references afterwards is
 * freed too (the uppercase d= variants). */
static void
delete_placements(struct term *t, const struct placement_filter *f, bool free_images) {
    struct graphics_store *g = &t->graphics;
    uint32_t *ids = NULL;
    size_t n = 0, cap = 0;

    for (size_t b = 0; b < g->placement_bucket_count; b++) {
        struct graphics_placement **slot = &g->placements[b];
        while (*slot != NULL) {
            struct graphics_placement *p = *slot;
            if (!filter_matches(f, p)) {
                slot = &p->hash_next;
                continue;
            }
            if (p->non_virtual)
                blank_cells_for_handle(t, p->handle);
            if (free_images) {
                if (n == cap) {
                    cap = cap != 0 ? cap * 2 : 8;
                    ids = xrealloc(ids, cap * sizeof(*ids));
                }
                ids[n++] = p->image_id;
            }
            *slot = p->hash_next;
            free(p);
        }
    }

    for (size_t i = 0; i < n; i++) {
        struct graphics_image *img = store_find(g, ids[i]);
        if (img != NULL && !image_has_placements(g, ids[i]))
            store_remove(g, img);
    }
    free(ids);
}

/* Deletes every non-virtual placement with a cell inside the given screen
 * rectangle (inclusive, 0-based). */
static void
delete_placements_at(struct term *t, int col_lo, int col_hi, int row_lo, int row_hi,
                     bool free_images) {
    uint32_t *handles = NULL;
    size_t n = 0, cap = 0;

    row_lo = MAX(row_lo, 0);
    row_hi = MIN(row_hi, t->rows - 1);
    col_lo = MAX(col_lo, 0);
    col_hi = MIN(col_hi, t->cols - 1);
    for (int r = row_lo; r <= row_hi; r++) {
        struct row *row = grid_row(t->grid, r);
        for (int col = col_lo; col <= col_hi; col++) {
            const struct cell *cell = &row->cells[col];
            if (!(cell->attrs & ATTR_IMAGE))
                continue;
            bool seen = false;
            for (size_t i = 0; i < n; i++)
                seen = seen || handles[i] == cell->ul;
            if (seen)
                continue;
            if (n == cap) {
                cap = cap != 0 ? cap * 2 : 8;
                handles = xrealloc(handles, cap * sizeof(*handles));
            }
            handles[n++] = cell->ul;
        }
    }

    for (size_t i = 0; i < n; i++) {
        struct placement_filter f = {.have_handle = true, .handle = handles[i]};
        delete_placements(t, &f, free_images);
    }
    free(handles);
}

/* The newest stored image with the given number, or NULL. */
static struct graphics_image *
find_image_by_number(struct graphics_store *g, uint32_t number) {
    struct graphics_image *best = NULL;
    for (size_t b = 0; b < g->bucket_count; b++)
        for (struct graphics_image *img = g->buckets[b]; img != NULL; img = img->hash_next)
            if (img->number == number && (best == NULL || img->seq > best->seq))
                best = img;
    return best;
}

/* The image a command addresses: by id, else by number, else (anonymous
 * a=p) the one stored last. */
static struct graphics_image *
resolve_image(struct graphics_store *g, const struct cmd *c) {
    if (c->id != 0)
        return store_find(g, c->id);
    if (c->have_number)
        return find_image_by_number(g, c->number);
    return g->last_image_id != 0 ? store_find(g, g->last_image_id) : NULL;
}

/* a=d: lowercase keys delete placements only, uppercase also free image data
 * that no placement references any more. */
static void
handle_delete(struct graphics_store *g, const struct cmd *c, graphics_reply_fn fn, void *user) {
    struct term *t = user;
    char d = c->del != 0 ? c->del : 'a';
    bool upper = d >= 'A' && d <= 'Z';
    char key = upper ? (char)(d + ('a' - 'A')) : d;

    switch (key) {
    case 'a': {
        struct placement_filter f = {.all = true};
        delete_placements(t, &f, upper);
        if (upper)
            graphics_clear(g);
        break;
    }
    case 'i':
    case 'n': {
        struct graphics_image *img = NULL;
        if (key == 'i')
            img = store_find(g, c->id);
        else if (c->have_number)
            img = find_image_by_number(g, c->number);
        if (img == NULL)
            break;
        struct placement_filter f = {
            .image_id = img->id, .have_pid = c->placement_id != 0, .pid = c->placement_id};
        uint32_t image_id = img->id;
        delete_placements(t, &f, upper);
        if (upper && !f.have_pid) {
            img = store_find(g, image_id);
            if (img != NULL)
                store_remove(g, img);
        }
        break;
    }
    case 'c':
        delete_placements_at(t, t->cursor.col, t->cursor.col, t->cursor.row, t->cursor.row, upper);
        break;
    case 'p': {
        int col = MAX(c->x, 1) - 1, row = MAX(c->y, 1) - 1;
        delete_placements_at(t, col, col, row, row, upper);
        break;
    }
    case 'x': {
        int col = MAX(c->x, 1) - 1;
        delete_placements_at(t, col, col, 0, t->rows - 1, upper);
        break;
    }
    case 'y': {
        int row = MAX(c->y, 1) - 1;
        delete_placements_at(t, 0, t->cols - 1, row, row, upper);
        break;
    }
    default:
        reply_err(fn, user, c, "EINVAL", "unsupported delete target");
        return;
    }
    reply_ok(fn, user, c);
}

/* a=p: creates a placement referencing an already-transmitted image, virtual
 * (U=1) or non-virtual (its own on-screen position, at the cursor). */
static void
handle_placement(struct graphics_store *g, const struct cmd *c, graphics_reply_fn fn, void *user) {
    struct graphics_image *img = resolve_image(g, c);
    if (img == NULL) {
        reply_err(fn, user, c, "ENOENT", "no image with that id");
        return;
    }
    struct cmd rc = *c;
    if (c->id == 0 && c->have_number)
        rc.id = img->id;
    struct graphics_src_rect src = {c->x, c->y, c->w, c->h};

    struct term *t = user;
    int cols, rows;
    resolve_placement_size(t, normalize_src(img, src), c->cols, c->rows, &cols, &rows);
    if (c->virtual_placement) {
        placement_insert(g, img, c->placement_id, cols, rows, src);
        reply_ok(fn, user, &rc);
        return;
    }
    graphics_place_nonvirtual(t, img->id, c->placement_id, cols, rows, src, !c->no_move);
    reply_ok(fn, user, &rc);
}

static void
handle_transmit(struct graphics_store *g, const struct cmd *c, const char *payload, size_t payload_len,
                graphics_reply_fn fn, void *user) {
    if (c->medium != 'd' && c->medium != 'f' && c->medium != 't' && c->medium != 's') {
        reply_err(fn, user, c, "EINVAL", "unknown transmission medium");
        return;
    }
    if (payload_len % 4 != 0) {
        reply_err(fn, user, c, "EINVAL", "malformed base64 payload");
        return;
    }

    size_t decoded_cap = payload_len / 4 * 3;
    uint8_t *decoded = xmalloc(decoded_cap ? decoded_cap : 1);
    size_t decoded_len = base64_decode(payload, payload_len, decoded);
    if (decoded_len == (size_t)-1) {
        free(decoded);
        reply_err(fn, user, c, "EINVAL", "malformed base64 payload");
        return;
    }

    uint8_t *data = decoded;
    size_t data_len = decoded_len;
    if (c->medium != 'd') {
        /* The base64 payload is the file path or shared memory name, not data. */
        char *path = xmalloc(decoded_len + 1);
        memcpy(path, decoded, decoded_len);
        path[decoded_len] = '\0';
        free(decoded);

        bool ok;
        if (c->medium == 's') {
            ok = read_shm(path, &data, &data_len);
        } else {
            ok = read_whole_file(path, &data, &data_len);
            if (c->medium == 't' && is_deletable_temp_file(path))
                unlink(path);
        }
        free(path);
        if (!ok) {
            reply_err(fn, user, c, "EBADF", "cannot read image data");
            return;
        }
        if (!slice_data(data, &data_len, c->data_offset, c->data_size)) {
            free(data);
            reply_err(fn, user, c, "EINVAL", "data offset out of range");
            return;
        }
    }

    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    const char *err_code = NULL, *err_msg = NULL;
    bool decoded_ok = decode_pixels(c, data, data_len, &rgba, &width, &height, &err_code, &err_msg);
    free(data);

    if (!decoded_ok) {
        reply_err(fn, user, c, err_code, err_msg);
        return;
    }

    size_t bytes = (size_t)width * height * 4;
    if (bytes > GRAPHICS_QUOTA_BYTES) {
        free(rgba);
        reply_err(fn, user, c, "ENOSPC", "image exceeds the graphics memory quota");
        return;
    }

    if (c->action == 'q') {
        /* Query: report whether this would have worked, without storing it. */
        free(rgba);
        reply_ok(fn, user, c);
        return;
    }

    /* An image the client did not give an id (anonymous, or addressed by
     * number) gets a fresh internal one, so it never overwrites another. */
    uint32_t id = c->id != 0 ? c->id : alloc_internal_id(g);
    struct graphics_image *img = xmalloc(sizeof(*img));
    *img = (struct graphics_image){.id = id,
                                   .number = c->id == 0 ? c->number : 0,
                                   .width = width,
                                   .height = height,
                                   .rgba = rgba,
                                   .bytes = bytes};
    store_insert(g, img);

    struct graphics_src_rect src = {c->x, c->y, c->w, c->h};
    if (c->virtual_placement || c->action == 'T') {
        /* Without c=/r= the placement is sized from the image and the cell
         * size, for virtual placements too (yazi sends a=T,U=1 with neither). */
        struct term *t = user;
        int cols, rows;
        resolve_placement_size(t, normalize_src(img, src), c->cols, c->rows, &cols, &rows);
        if (c->virtual_placement) {
            /* a=T combined with U=1: create the virtual placement inline. Per
             * spec this need not be a separate a=p command. */
            placement_insert(g, img, c->placement_id, cols, rows, src);
        } else {
            /* a=T without U=1: display it non-virtually at the cursor, right
             * away. Unlike the virtual case, this has a visible grid/cursor
             * effect, so it is gated on action == 'T' specifically -- a plain
             * a=t transmit must not. */
            graphics_place_nonvirtual(t, id, c->placement_id, cols, rows, src, !c->no_move);
        }
    }

    struct cmd rc = *c;
    rc.id = (c->id != 0 || c->have_number) ? id : 0;
    reply_ok(fn, user, &rc);
}

static void
dispatch(struct graphics_store *g, const struct cmd *c, const char *payload, size_t payload_len,
         graphics_reply_fn fn, void *user) {
    switch (c->action) {
    case 'd':
        handle_delete(g, c, fn, user);
        return;
    case 'p':
        handle_placement(g, c, fn, user);
        return;
    case 'q':
    case 't':
    case 'T':
    case 0: /* default: plain transmit */
        handle_transmit(g, c, payload, payload_len, fn, user);
        return;
    default:
        reply_err(fn, user, c, "EINVAL", "unsupported action");
        return;
    }
}

static void
chunk_append(struct graphics_chunk *ch, const char *payload, size_t len) {
    if (ch->b64_len + len > ch->b64_cap) {
        ch->b64_cap = MAX(ch->b64_cap ? ch->b64_cap * 2 : 4096, ch->b64_len + len);
        ch->b64 = xrealloc(ch->b64, ch->b64_cap);
    }
    memcpy(ch->b64 + ch->b64_len, payload, len);
    ch->b64_len += len;
}

void graphics_apc(struct graphics_store *g, const uint8_t *data, size_t len,
                  graphics_reply_fn fn, void *user) {
    if (len == 0 || data[0] != 'G')
        return;

    const uint8_t *body = data + 1;
    size_t body_len = len - 1;
    size_t semi = 0;
    while (semi < body_len && body[semi] != ';')
        semi++;

    const char *payload = semi < body_len ? (const char *)body + semi + 1 : "";
    size_t payload_len = semi < body_len ? body_len - semi - 1 : 0;

    struct cmd c = {.format = 32, .medium = 'd'};
    parse_control(body, semi, &c);

    if (g->chunk != NULL) {
        /* Continuation chunk: only m/q are meaningful; everything else in
         * cmd was already captured from the first chunk. */
        struct graphics_chunk *ch = g->chunk;
        ch->cmd.quiet = c.quiet;
        chunk_append(ch, payload, payload_len);
        if (c.more != 1) {
            g->chunk = NULL;
            dispatch(g, &ch->cmd, ch->b64, ch->b64_len, fn, user);
            free(ch->b64);
            free(ch);
        }
        return;
    }

    if (c.more == 1) {
        struct graphics_chunk *ch = xcalloc(1, sizeof(*ch));
        ch->cmd = c;
        chunk_append(ch, payload, payload_len);
        g->chunk = ch;
        return; /* kitty replies only once, after the final chunk */
    }

    dispatch(g, &c, payload, payload_len, fn, user);
}

/* ---- term.h glue ---- */

static void
reply_via_term(void *user, const char *text) {
    term_reply_apc(user, "%s", text);
}

void term_apc(struct term *t, const uint8_t *data, size_t len) {
    graphics_apc(&t->graphics, data, len, reply_via_term, t);
}
