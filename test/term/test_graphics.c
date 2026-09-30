/* Kitty graphics protocol transport: control-data parsing, chunk
 * reassembly, raw/zlib/PNG payloads, quota eviction, query and delete. */
#include <spng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include "core/base64.h"
#include "term/graphics.h"
#include "term/term.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static char reply[256];

static void
on_write(void *user, const void *data, size_t len) {
    size_t cur = strlen(reply);
    if (cur + len < sizeof(reply)) {
        memcpy(reply + cur, data, len);
        reply[cur + len] = '\0';
    }
}

static const struct term_host host = {.write = on_write};

/* One full "ESC _G <ctrl>;<payload> ESC \" command in one shot. */
static void
send_apc(struct term *t, const char *ctrl, const uint8_t *payload, size_t payload_len) {
    reply[0] = '\0';
    char head[160];
    snprintf(head, sizeof(head), "\x1b_G%s;", ctrl);
    term_feed(t, (const uint8_t *)head, strlen(head));
    if (payload_len > 0) {
        char *b64 = base64_encode(payload, payload_len);
        term_feed(t, (const uint8_t *)b64, strlen(b64));
        free(b64);
    }
    term_feed(t, (const uint8_t *)"\x1b\\", 2);
}

static void
test_transmit_rgba(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[2 * 2 * 4];
    for (size_t i = 0; i < sizeof(px); i++)
        px[i] = (uint8_t)i;

    send_apc(&t, "f=32,s=2,v=2,i=1,a=T", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=1;OK\x1b\\") == 0);

    struct graphics_image *img = graphics_get(&t.graphics, 1);
    CHECK(img != NULL && img->width == 2 && img->height == 2);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);

    term_destroy(&t);
}

static void
test_transmit_rgb24_expands_alpha(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[2 * 2 * 3] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    send_apc(&t, "f=24,s=2,v=2,i=2,a=T", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=2;OK\x1b\\") == 0);

    struct graphics_image *img = graphics_get(&t.graphics, 2);
    CHECK(img != NULL);
    if (img != NULL) {
        for (int i = 0; i < 4; i++) {
            CHECK(img->rgba[4 * i + 0] == px[3 * i + 0]);
            CHECK(img->rgba[4 * i + 1] == px[3 * i + 1]);
            CHECK(img->rgba[4 * i + 2] == px[3 * i + 2]);
            CHECK(img->rgba[4 * i + 3] == 0xff);
        }
    }
    term_destroy(&t);
}

static void
test_quiet_modes(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[1 * 1 * 4] = {1, 2, 3, 4};

    /* q=1 suppresses the OK reply on success. */
    send_apc(&t, "f=32,s=1,v=1,i=3,a=T,q=1", px, sizeof(px));
    CHECK(reply[0] == '\0');
    CHECK(graphics_get(&t.graphics, 3) != NULL);

    /* q=2 suppresses the error reply on failure (bad format here). */
    send_apc(&t, "f=7,s=1,v=1,i=4,a=T,q=2", px, sizeof(px));
    CHECK(reply[0] == '\0');
    CHECK(graphics_get(&t.graphics, 4) == NULL);

    /* Default (q=0) reports both. */
    send_apc(&t, "f=7,s=1,v=1,i=4,a=T", px, sizeof(px));
    CHECK(strncmp(reply, "\x1b_Gi=4;EINVAL", 13) == 0);

    term_destroy(&t);
}

static void
test_query_does_not_store(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[1 * 1 * 4] = {5, 6, 7, 8};
    send_apc(&t, "f=32,s=1,v=1,i=9,a=q", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=9;OK\x1b\\") == 0);
    CHECK(graphics_get(&t.graphics, 9) == NULL);

    term_destroy(&t);
}

static void
test_delete(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[1 * 1 * 4] = {0};
    send_apc(&t, "f=32,s=1,v=1,i=11,a=T", px, sizeof(px));
    send_apc(&t, "f=32,s=1,v=1,i=12,a=T", px, sizeof(px));
    CHECK(graphics_get(&t.graphics, 11) != NULL);
    CHECK(graphics_get(&t.graphics, 12) != NULL);

    /* Lowercase deletes placements only; the image data stays. */
    send_apc(&t, "a=d,d=i,i=11", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=11;OK\x1b\\") == 0);
    CHECK(graphics_get(&t.graphics, 11) != NULL);
    CHECK(!(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE));

    send_apc(&t, "a=d,d=I,i=11", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=11;OK\x1b\\") == 0);
    CHECK(graphics_get(&t.graphics, 11) == NULL);
    CHECK(graphics_get(&t.graphics, 12) != NULL);

    send_apc(&t, "a=d,d=a", NULL, 0);
    CHECK(graphics_get(&t.graphics, 12) != NULL);

    send_apc(&t, "a=d,d=A", NULL, 0);
    CHECK(graphics_get(&t.graphics, 12) == NULL);

    term_destroy(&t);
}

static void
test_chunked_transmission(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[2 * 2 * 4];
    for (size_t i = 0; i < sizeof(px); i++)
        px[i] = (uint8_t)(i * 3);
    char *b64 = base64_encode(px, sizeof(px));
    size_t b64_len = strlen(b64);
    size_t split = (b64_len / 2) & ~(size_t)3; /* keep the first part a multiple of 4 */

    reply[0] = '\0';
    char head[128];
    snprintf(head, sizeof(head), "\x1b_Gf=32,s=2,v=2,i=20,a=T,m=1;");
    term_feed(&t, (const uint8_t *)head, strlen(head));
    term_feed(&t, (const uint8_t *)b64, split);
    term_feed(&t, (const uint8_t *)"\x1b\\", 2);
    CHECK(reply[0] == '\0'); /* no reply until the final chunk */

    term_feed(&t, (const uint8_t *)"\x1b_Gm=0;", 7);
    term_feed(&t, (const uint8_t *)b64 + split, b64_len - split);
    term_feed(&t, (const uint8_t *)"\x1b\\", 2);
    CHECK(strcmp(reply, "\x1b_Gi=20;OK\x1b\\") == 0);

    struct graphics_image *img = graphics_get(&t.graphics, 20);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);

    free(b64);
    term_destroy(&t);
}

static void
test_zlib_compressed(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[4 * 4 * 4];
    for (size_t i = 0; i < sizeof(px); i++)
        px[i] = (uint8_t)(i * 7 + 1);

    uLongf comp_cap = compressBound(sizeof(px));
    uint8_t *comp = malloc(comp_cap);
    uLongf comp_len = comp_cap;
    CHECK(compress(comp, &comp_len, px, sizeof(px)) == Z_OK);

    send_apc(&t, "f=32,s=4,v=4,i=30,a=T,o=z", comp, comp_len);
    CHECK(strcmp(reply, "\x1b_Gi=30;OK\x1b\\") == 0);

    struct graphics_image *img = graphics_get(&t.graphics, 30);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);

    free(comp);
    term_destroy(&t);
}

static void
test_png_roundtrip(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    const int w = 3, h = 2;
    uint8_t px[3 * 2 * 4];
    for (size_t i = 0; i < sizeof(px); i++)
        px[i] = (uint8_t)(i * 11 + 5);

    spng_ctx *enc = spng_ctx_new(SPNG_CTX_ENCODER);
    CHECK(spng_set_option(enc, SPNG_ENCODE_TO_BUFFER, 1) == 0);
    struct spng_ihdr ihdr = {
        .width = w, .height = h, .bit_depth = 8, .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA};
    CHECK(spng_set_ihdr(enc, &ihdr) == 0);
    CHECK(spng_encode_image(enc, px, sizeof(px), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) == 0);
    int spng_err = 0;
    size_t png_len = 0;
    void *png = spng_get_png_buffer(enc, &png_len, &spng_err);
    CHECK(png != NULL && spng_err == 0);

    send_apc(&t, "f=100,i=40,a=T", png, png_len);
    CHECK(strcmp(reply, "\x1b_Gi=40;OK\x1b\\") == 0);

    struct graphics_image *img = graphics_get(&t.graphics, 40);
    CHECK(img != NULL && img->width == w && img->height == h);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);

    free(png);
    spng_ctx_free(enc);
    term_destroy(&t);
}

/* A single image over the 64 MB quota is rejected outright (ENOSPC), and
 * the least-recently-used image is evicted once the total exceeds it. */
static void
test_quota(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    /* Over quota: compressible all-zero data keeps the wire payload tiny. */
    size_t over_w = 8192, over_h = 2050; /* 8192*2050*4 = 64 MiB + 64 KiB */
    size_t over_len = over_w * over_h * 4;
    uint8_t *zeros = calloc(1, over_len);
    uLongf comp_cap = compressBound(over_len);
    uint8_t *comp = malloc(comp_cap);
    uLongf comp_len = comp_cap;
    CHECK(compress(comp, &comp_len, zeros, over_len) == Z_OK);

    char ctrl[128];
    snprintf(ctrl, sizeof(ctrl), "f=32,s=%zu,v=%zu,i=50,a=T,o=z", over_w, over_h);
    send_apc(&t, ctrl, comp, comp_len);
    CHECK(strncmp(reply, "\x1b_Gi=50;ENOSPC", 14) == 0);
    CHECK(graphics_get(&t.graphics, 50) == NULL);
    free(zeros);
    free(comp);

    /* Two images together (40 MiB + 30 MiB) exceed the quota: the older
     * (image 51) is evicted once the newer (image 52) is stored. */
    size_t a_w = 4096, a_h = 2560; /* 40 MiB */
    size_t b_w = 4096, b_h = 1920; /* 30 MiB */
    size_t a_len = a_w * a_h * 4, b_len = b_w * b_h * 4;
    uint8_t *a = calloc(1, a_len);
    uint8_t *b = calloc(1, b_len);

    uLongf a_comp_cap = compressBound(a_len), b_comp_cap = compressBound(b_len);
    uint8_t *a_comp = malloc(a_comp_cap), *b_comp = malloc(b_comp_cap);
    uLongf a_comp_len = a_comp_cap, b_comp_len = b_comp_cap;
    CHECK(compress(a_comp, &a_comp_len, a, a_len) == Z_OK);
    CHECK(compress(b_comp, &b_comp_len, b, b_len) == Z_OK);

    snprintf(ctrl, sizeof(ctrl), "f=32,s=%zu,v=%zu,i=51,a=T,o=z", a_w, a_h);
    send_apc(&t, ctrl, a_comp, a_comp_len);
    CHECK(strcmp(reply, "\x1b_Gi=51;OK\x1b\\") == 0);
    CHECK(graphics_get(&t.graphics, 51) != NULL);

    snprintf(ctrl, sizeof(ctrl), "f=32,s=%zu,v=%zu,i=52,a=T,o=z", b_w, b_h);
    send_apc(&t, ctrl, b_comp, b_comp_len);
    CHECK(strcmp(reply, "\x1b_Gi=52;OK\x1b\\") == 0);

    CHECK(graphics_get(&t.graphics, 51) == NULL); /* evicted */
    CHECK(graphics_get(&t.graphics, 52) != NULL);
    CHECK(t.graphics.total_bytes <= GRAPHICS_QUOTA_BYTES);

    free(a);
    free(b);
    free(a_comp);
    free(b_comp);
    term_destroy(&t);
}

static void
test_virtual_placement(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,i=60,a=T", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=60;OK\x1b\\") == 0);

    send_apc(&t, "a=p,U=1,i=60,c=3,r=2", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=60;OK\x1b\\") == 0);

    struct graphics_placement *p = graphics_placement_get(&t.graphics, 60, 0);
    CHECK(p != NULL && p->cols == 3 && p->rows == 2);

    /* Combined a=T + U=1: transmit and place in one command. */
    send_apc(&t, "f=32,s=2,v=2,i=61,a=T,U=1,c=4,r=5,p=9", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=61;OK\x1b\\") == 0);
    struct graphics_placement *p2 = graphics_placement_get(&t.graphics, 61, 9);
    CHECK(p2 != NULL && p2->cols == 4 && p2->rows == 5);
    CHECK(graphics_placement_get_any(&t.graphics, 61) == p2);

    term_destroy(&t);
}

static void
test_virtual_placement_defaults(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[1 * 1 * 4] = {0};
    send_apc(&t, "f=32,s=1,v=1,i=70,a=T", px, sizeof(px));

    /* Unknown image id. */
    send_apc(&t, "a=p,U=1,i=71,c=1,r=1", NULL, 0);
    CHECK(strncmp(reply, "\x1b_Gi=71;ENOENT", 14) == 0);

    /* Missing columns/rows default from the image's pixel size, as for
     * non-virtual placements, so yazi's bare "a=T,U=1" transmit works. */
    term_set_cell_size(&t, 10, 20);
    uint8_t big[25 * 41 * 4] = {0}; /* ceil(25/10)=3 cols, ceil(41/20)=3 rows */
    int cursor_row = t.cursor.row, cursor_col = t.cursor.col;
    send_apc(&t, "f=32,s=25,v=41,i=72,a=T,U=1,p=4", big, sizeof(big));
    CHECK(strcmp(reply, "\x1b_Gi=72;OK\x1b\\") == 0);
    struct graphics_placement *v = graphics_placement_get(&t.graphics, 72, 4);
    CHECK(v != NULL && v->cols == 3 && v->rows == 3);
    CHECK(t.cursor.row == cursor_row && t.cursor.col == cursor_col); /* virtual: nothing drawn */

    send_apc(&t, "a=p,U=1,i=70", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=70;OK\x1b\\") == 0);
    CHECK(graphics_placement_get(&t.graphics, 70, 0) != NULL);

    term_destroy(&t);
}

/* a=p without U=1: creates a non-virtual placement at the cursor, writing
 * ATTR_IMAGE cells row by row and advancing the cursor exactly as printed
 * text would -- landing one column past the placement's right edge, on the
 * row the last cell was written to. */
static void
test_nonvirtual_placement(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 20);

    uint8_t px[4 * 4 * 4] = {0};
    send_apc(&t, "f=32,s=4,v=4,i=90,a=T", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=90;OK\x1b\\") == 0);

    t.cursor.row = 1;
    t.cursor.col = 2;
    send_apc(&t, "a=p,i=90,c=3,r=2,p=5", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=90;OK\x1b\\") == 0);

    /* Cells carry a store-wide handle, not the client's p= key. */
    uint32_t handle = grid_row(t.grid, 1)->cells[2].ul;
    struct graphics_placement *p = graphics_placement_get_nonvirtual(&t.graphics, handle);
    CHECK(p != NULL && p->image_id == 90 && p->placement_id == 5 && p->cols == 3 && p->rows == 2);

    for (int col = 2; col < 5; col++) {
        struct cell *c = &grid_row(t.grid, 1)->cells[col];
        CHECK(c->attrs & ATTR_IMAGE);
        CHECK(c->ul == handle);
        CHECK(c->tile_row == 0 && c->tile_col == col - 2);
    }
    for (int col = 2; col < 5; col++) {
        struct cell *c = &grid_row(t.grid, 2)->cells[col];
        CHECK(c->attrs & ATTR_IMAGE);
        CHECK(c->tile_row == 1 && c->tile_col == col - 2);
    }

    CHECK(t.cursor.row == 2); /* start row (1) + rows (2) - 1 */
    CHECK(t.cursor.col == 5); /* start col (2) + cols (3) */

    term_destroy(&t);
}

/* Without c=/r=, cols/rows default from the image's pixel size divided by
 * the terminal's cell size (rounded up). */
static void
test_nonvirtual_placement_default_size(void) {
    struct term t;
    term_init(&t, 20, 5, &host, NULL);
    term_set_cell_size(&t, 8, 16);

    uint8_t px[20 * 33 * 4] = {0}; /* 20x33 px -> ceil(20/8)=3 cols, ceil(33/16)=3 rows */
    send_apc(&t, "f=32,s=20,v=33,i=91,a=T", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=91;OK\x1b\\") == 0);

    send_apc(&t, "a=p,i=91,p=6", NULL, 0);
    CHECK(strcmp(reply, "\x1b_Gi=91;OK\x1b\\") == 0);

    struct graphics_placement *p =
        graphics_placement_get_nonvirtual(&t.graphics, grid_row(t.grid, 0)->cells[0].ul);
    CHECK(p != NULL && p->cols == 3 && p->rows == 3);

    term_destroy(&t);
}

/* a=T without U=1: transmits and displays non-virtually in one command. A
 * plain a=t (no display) with the same c=/r=/p= must not touch the grid. */
static void
test_nonvirtual_placement_via_transmit(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,i=92,a=T,c=2,r=2,p=7", px, sizeof(px));
    CHECK(strcmp(reply, "\x1b_Gi=92;OK\x1b\\") == 0);

    struct cell *c0 = &grid_row(t.grid, 0)->cells[0];
    CHECK(c0->attrs & ATTR_IMAGE);
    CHECK(graphics_placement_get_nonvirtual(&t.graphics, c0->ul) != NULL);
    CHECK(t.cursor.row == 1 && t.cursor.col == 2);

    send_apc(&t, "f=32,s=2,v=2,i=93,c=2,r=2,p=8", px, sizeof(px)); /* a omitted: plain transmit */
    CHECK(!(grid_row(t.grid, 2)->cells[0].attrs & ATTR_IMAGE));
    CHECK(graphics_get(&t.graphics, 93) != NULL);

    term_destroy(&t);
}

/* a=d,d=i additionally blanks the ATTR_IMAGE cells a non-virtual placement
 * occupies, not just the placement/image bookkeeping. */
static void
test_delete_erases_nonvirtual_cells(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,i=94,a=T,c=3,r=2,p=11", px, sizeof(px));
    CHECK(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE);

    uint32_t handle = grid_row(t.grid, 0)->cells[0].ul;
    send_apc(&t, "a=d,d=i,i=94", NULL, 0);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            CHECK(!(grid_row(t.grid, r)->cells[c].attrs & ATTR_IMAGE));
    CHECK(graphics_placement_get_nonvirtual(&t.graphics, handle) == NULL);

    term_destroy(&t);
}

static void
test_placement_cleared_with_image(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    uint8_t px[1 * 1 * 4] = {0};
    send_apc(&t, "f=32,s=1,v=1,i=80,a=T,U=1,c=1,r=1", px, sizeof(px));
    CHECK(graphics_placement_get(&t.graphics, 80, 0) != NULL);

    send_apc(&t, "a=d,d=I,i=80", NULL, 0);
    CHECK(graphics_get(&t.graphics, 80) == NULL);
    CHECK(graphics_placement_get(&t.graphics, 80, 0) == NULL);
    CHECK(graphics_placement_get_any(&t.graphics, 80) == NULL);

    term_destroy(&t);
}

/* a=T with no i= key: a spec-legal "anonymous" (id=0) one-shot transmit and
 * display, used by fastfetch's kitty logo. Real kitty never replies in this
 * case (no id to report back), and neither must astralia-term -- an
 * unsolicited reply here previously leaked onto the pty and corrupted the
 * next shell prompt when the client, having asked for nothing, never read
 * it (see local/plan/bug-fastfetch-kitty-image-id.md). */
static void
test_anonymous_transmit_no_reply(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,a=T,c=2,r=2,p=7", px, sizeof(px));
    CHECK(reply[0] == '\0');

    struct cell *c0 = &grid_row(t.grid, 0)->cells[0];
    CHECK(c0->attrs & ATTR_IMAGE);
    uint32_t first = c0->ul;
    CHECK(t.cursor.row == 1 && t.cursor.col == 2);

    /* fastfetch runs once per prompt: a second anonymous transmit must
     * replace the first one silently, not accumulate or error. */
    send_apc(&t, "f=32,s=2,v=2,a=T,c=2,r=2,p=8", px, sizeof(px));
    CHECK(reply[0] == '\0');
    CHECK(grid_row(t.grid, 1)->cells[0].attrs & ATTR_IMAGE);

    /* Each anonymous image is its own image with its own placement: the first
     * one's cells (which may be in scrollback by now) keep showing it. */
    CHECK(grid_row(t.grid, 1)->cells[2].ul != first); /* second image starts at the cursor */
    CHECK(graphics_placement_get_nonvirtual(&t.graphics, first) != NULL);

    term_destroy(&t);
}

/* a=p with no i= key: places whatever image was last transmitted anonymously
 * (id=0), same silent-reply rule as the transmit case above. */
static void
test_anonymous_placement_no_reply(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2", px, sizeof(px)); /* a omitted: plain transmit, no display */
    CHECK(reply[0] == '\0');

    send_apc(&t, "a=p,c=2,r=2,p=9", NULL, 0);
    CHECK(reply[0] == '\0');

    struct cell *c0 = &grid_row(t.grid, 0)->cells[0];
    CHECK(c0->attrs & ATTR_IMAGE);

    term_destroy(&t);
}

/* Placement IDs are only unique per image: two images may both use p=1. */
static void
test_placement_ids_are_per_image(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,i=1,a=T,c=1,r=1,p=1", px, sizeof(px));
    send_apc(&t, "f=32,s=2,v=2,i=2,a=T,c=1,r=1,p=1", px, sizeof(px));

    uint32_t h1 = grid_row(t.grid, 0)->cells[0].ul;
    uint32_t h2 = grid_row(t.grid, 0)->cells[1].ul; /* the cursor moved one column right */
    CHECK(h1 != h2);
    struct graphics_placement *p1 = graphics_placement_get_nonvirtual(&t.graphics, h1);
    struct graphics_placement *p2 = graphics_placement_get_nonvirtual(&t.graphics, h2);
    CHECK(p1 != NULL && p1->image_id == 1);
    CHECK(p2 != NULL && p2->image_id == 2);

    /* Re-placing (image 1, p=1) replaces the old placement and blanks its cells. */
    t.cursor.row = 3;
    t.cursor.col = 0;
    send_apc(&t, "a=p,i=1,p=1,c=1,r=1", NULL, 0);
    CHECK(!(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE));
    CHECK(graphics_placement_get_nonvirtual(&t.graphics, h1) == NULL);
    CHECK(grid_row(t.grid, 0)->cells[1].attrs & ATTR_IMAGE); /* image 2 untouched */
    CHECK(grid_row(t.grid, 3)->cells[0].attrs & ATTR_IMAGE);

    term_destroy(&t);
}

static void
test_image_numbers(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[1 * 1 * 4] = {1, 2, 3, 4};
    send_apc(&t, "f=32,s=1,v=1,I=7", px, sizeof(px));
    /* The terminal picks the id and reports it together with the number. */
    unsigned id = 0, num = 0;
    CHECK(sscanf(reply, "\x1b_Gi=%u,I=%u;OK", &id, &num) == 2);
    CHECK(id != 0 && num == 7);
    CHECK(graphics_get(&t.graphics, id) != NULL);

    /* A newer image with the same number takes it over. */
    send_apc(&t, "f=32,s=1,v=1,I=7", px, sizeof(px));
    unsigned id2 = 0;
    CHECK(sscanf(reply, "\x1b_Gi=%u,I=%u;OK", &id2, &num) == 2);
    CHECK(id2 != id);

    send_apc(&t, "a=p,I=7,c=1,r=1", NULL, 0);
    CHECK(sscanf(reply, "\x1b_Gi=%u,I=%u;OK", &id, &num) == 2 && id == id2);
    struct graphics_placement *p =
        graphics_placement_get_nonvirtual(&t.graphics, grid_row(t.grid, 0)->cells[0].ul);
    CHECK(p != NULL && p->image_id == id2);

    send_apc(&t, "a=d,d=N,I=7", NULL, 0);
    CHECK(graphics_get(&t.graphics, id2) == NULL);
    CHECK(graphics_get(&t.graphics, id) == NULL);

    term_destroy(&t);
}

static void
test_source_rectangle(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[40 * 30 * 4] = {0};
    /* Crop 20x10 px at (5, 6); no c=/r=, so the size follows the crop: 2x1 cells. */
    send_apc(&t, "f=32,s=40,v=30,i=1,a=T,x=5,y=6,w=20,h=10", px, sizeof(px));
    struct graphics_placement *p =
        graphics_placement_get_nonvirtual(&t.graphics, grid_row(t.grid, 0)->cells[0].ul);
    CHECK(p != NULL);
    CHECK(p != NULL && p->src_x == 5 && p->src_y == 6 && p->src_w == 20 && p->src_h == 10);
    CHECK(p != NULL && p->cols == 2 && p->rows == 1);

    /* An oversized rectangle is clamped to the image; a=p on a virtual one too. */
    send_apc(&t, "a=p,i=1,U=1,c=2,r=2,p=3,x=30,y=20,w=99,h=99", NULL, 0);
    struct graphics_placement *v = graphics_placement_get(&t.graphics, 1, 3);
    CHECK(v != NULL && v->src_x == 30 && v->src_y == 20 && v->src_w == 10 && v->src_h == 10);

    term_destroy(&t);
}

static void
test_cursor_stays_with_c1(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    t.cursor.row = 1;
    t.cursor.col = 3;
    send_apc(&t, "f=32,s=2,v=2,i=1,a=T,c=2,r=2,C=1", px, sizeof(px));
    CHECK(grid_row(t.grid, 2)->cells[4].attrs & ATTR_IMAGE);
    CHECK(t.cursor.row == 1 && t.cursor.col == 3);

    /* Near the bottom the placement scrolls the screen; the cursor still ends
     * on the placement's first row. */
    t.cursor.row = 4;
    t.cursor.col = 0;
    send_apc(&t, "a=p,i=1,c=2,r=2,C=1", NULL, 0);
    CHECK(t.cursor.row == 3 && t.cursor.col == 0);
    CHECK(grid_row(t.grid, 3)->cells[0].attrs & ATTR_IMAGE);
    CHECK(grid_row(t.grid, 4)->cells[0].attrs & ATTR_IMAGE);

    term_destroy(&t);
}

static void
test_delete_by_position(void) {
    struct term t;
    term_init(&t, 10, 6, &host, NULL);
    term_set_cell_size(&t, 10, 10);

    uint8_t px[2 * 2 * 4] = {0};
    send_apc(&t, "f=32,s=2,v=2,i=1,a=T,c=2,r=2", px, sizeof(px));   /* rows 0-1, cols 0-1 */
    t.cursor.row = 3;
    t.cursor.col = 5;
    send_apc(&t, "f=32,s=2,v=2,i=2,a=T,c=2,r=2", px, sizeof(px));   /* rows 3-4, cols 5-6 */

    /* d=p: the placement covering column 2, row 1 (1-based) -- image 1's. */
    send_apc(&t, "a=d,d=p,x=2,y=1", NULL, 0);
    CHECK(!(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE));
    CHECK(!(grid_row(t.grid, 1)->cells[1].attrs & ATTR_IMAGE));
    CHECK(grid_row(t.grid, 3)->cells[5].attrs & ATTR_IMAGE);
    CHECK(graphics_get(&t.graphics, 1) != NULL); /* lowercase keeps the data */

    /* d=X: column 6 (1-based) crosses image 2; uppercase frees its data. */
    send_apc(&t, "a=d,d=X,x=6", NULL, 0);
    CHECK(!(grid_row(t.grid, 3)->cells[5].attrs & ATTR_IMAGE));
    CHECK(graphics_get(&t.graphics, 2) == NULL);

    /* d=c: the cell under the cursor. */
    t.cursor.row = 0;
    t.cursor.col = 0;
    send_apc(&t, "a=T,f=32,s=2,v=2,i=3,c=2,r=2", px, sizeof(px));
    t.cursor.row = 1;
    t.cursor.col = 1;
    send_apc(&t, "a=d,d=C", NULL, 0);
    CHECK(!(grid_row(t.grid, 0)->cells[0].attrs & ATTR_IMAGE));
    CHECK(graphics_get(&t.graphics, 3) == NULL);

    term_destroy(&t);
}

static void
test_temp_file_medium(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);

    const char *path = "/tmp/astralia-test-tty-graphics-protocol.rgba";
    uint8_t px[2 * 1 * 4] = {9, 8, 7, 6, 5, 4, 3, 2};
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL)
        return;
    fwrite(px, 1, sizeof(px), f);
    fclose(f);

    send_apc(&t, "f=32,s=2,v=1,i=1,t=t", (const uint8_t *)path, strlen(path));
    CHECK(strcmp(reply, "\x1b_Gi=1;OK\x1b\\") == 0);
    struct graphics_image *img = graphics_get(&t.graphics, 1);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);
    CHECK(access(path, F_OK) != 0); /* the terminal deleted it */

    /* A plain t=f file is never deleted, and S=/O= select a slice of it. */
    path = "/tmp/astralia-test-plain.rgba";
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL)
        return;
    fwrite("xxxx", 1, 4, f);
    fwrite(px, 1, sizeof(px), f);
    fwrite("yyyy", 1, 4, f);
    fclose(f);
    send_apc(&t, "f=32,s=2,v=1,i=2,t=f,O=4,S=8", (const uint8_t *)path, strlen(path));
    CHECK(strcmp(reply, "\x1b_Gi=2;OK\x1b\\") == 0);
    img = graphics_get(&t.graphics, 2);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);
    CHECK(access(path, F_OK) == 0);
    unlink(path);

    /* Device and pseudo-filesystem paths are refused outright. */
    send_apc(&t, "f=32,s=1,v=1,i=3,t=f", (const uint8_t *)"/dev/zero", 9);
    CHECK(strncmp(reply, "\x1b_Gi=3;EBADF", 12) == 0);
    CHECK(graphics_get(&t.graphics, 3) == NULL);

    term_destroy(&t);
}

static void
test_shared_memory_medium(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);

    const char *name = "/astralia-test-shm";
    uint8_t px[1 * 2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(ftruncate(fd, sizeof(px)) == 0);
    CHECK(write(fd, px, sizeof(px)) == (ssize_t)sizeof(px));
    close(fd);

    send_apc(&t, "f=32,s=1,v=2,i=1,t=s", (const uint8_t *)name, strlen(name));
    CHECK(strcmp(reply, "\x1b_Gi=1;OK\x1b\\") == 0);
    struct graphics_image *img = graphics_get(&t.graphics, 1);
    CHECK(img != NULL && memcmp(img->rgba, px, sizeof(px)) == 0);
    CHECK(shm_open(name, O_RDONLY, 0) < 0); /* ownership passed to the terminal */

    term_destroy(&t);
}

static void
test_compressed_png(void) {
    struct term t;
    term_init(&t, 10, 5, &host, NULL);

    /* Build a 2x2 PNG with spng, then send it deflated (o=z). */
    uint8_t px[2 * 2 * 4];
    for (size_t i = 0; i < sizeof(px); i++)
        px[i] = (uint8_t)(i * 9);

    spng_ctx *enc = spng_ctx_new(SPNG_CTX_ENCODER);
    struct spng_ihdr ihdr = {.width = 2, .height = 2, .bit_depth = 8,
                             .color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA};
    spng_set_option(enc, SPNG_ENCODE_TO_BUFFER, 1);
    spng_set_ihdr(enc, &ihdr);
    CHECK(spng_encode_image(enc, px, sizeof(px), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) == 0);
    int err = 0;
    size_t png_len = 0;
    void *png = spng_get_png_buffer(enc, &png_len, &err);
    CHECK(png != NULL);
    if (png == NULL)
        return;

    uLongf zlen = compressBound((uLong)png_len);
    uint8_t *z = malloc(zlen);
    CHECK(compress(z, &zlen, png, (uLong)png_len) == Z_OK);

    send_apc(&t, "f=100,i=1,o=z", z, zlen);
    CHECK(strcmp(reply, "\x1b_Gi=1;OK\x1b\\") == 0);
    struct graphics_image *img = graphics_get(&t.graphics, 1);
    CHECK(img != NULL && img->width == 2 && memcmp(img->rgba, px, sizeof(px)) == 0);

    free(z);
    free(png);
    spng_ctx_free(enc);
    term_destroy(&t);
}

static void
test_malformed_apc_is_ignored(void) {
    struct term t;
    term_init(&t, 10, 2, &host, NULL);

    reply[0] = '\0';
    term_feed(&t, (const uint8_t *)"\x1b_Xnot-a-graphics-command\x1b\\", 27);
    CHECK(reply[0] == '\0');

    /* The parser must still work normally afterwards. */
    term_feed(&t, (const uint8_t *)"A", 1);
    CHECK(grid_row(t.grid, 0)->cells[0].cp == 'A');

    term_destroy(&t);
}

int main(void) {
    test_transmit_rgba();
    test_transmit_rgb24_expands_alpha();
    test_quiet_modes();
    test_query_does_not_store();
    test_delete();
    test_chunked_transmission();
    test_zlib_compressed();
    test_png_roundtrip();
    test_quota();
    test_virtual_placement();
    test_virtual_placement_defaults();
    test_nonvirtual_placement();
    test_nonvirtual_placement_default_size();
    test_nonvirtual_placement_via_transmit();
    test_delete_erases_nonvirtual_cells();
    test_placement_cleared_with_image();
    test_anonymous_transmit_no_reply();
    test_anonymous_placement_no_reply();
    test_placement_ids_are_per_image();
    test_image_numbers();
    test_source_rectangle();
    test_cursor_stays_with_c1();
    test_delete_by_position();
    test_temp_file_medium();
    test_shared_memory_medium();
    test_compressed_png();
    test_malformed_apc_is_ignored();

    if (failures == 0)
        printf("test_graphics: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
