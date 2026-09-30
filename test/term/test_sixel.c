/* Sixel body decoder correctness: raster-attribute sizing, color register
 * definition/reuse (RGB and HLS), repeat-count runs, band transitions
 * ($ and -), and a known-good small reference image compared
 * pixel-for-pixel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term/sixel.h"

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static bool
decode(const char *body, uint8_t **rgba, int *w, int *h) {
    return sixel_decode((const uint8_t *)body, strlen(body), rgba, w, h);
}

static void
check_pixel(const uint8_t *rgba, int width, int x, int y, uint8_t r, uint8_t g, uint8_t b,
           uint8_t a) {
    const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr,
                "%s:%d: pixel (%d,%d) = %d,%d,%d,%d, expected %d,%d,%d,%d\n", __FILE__,
                __LINE__, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        failures++;
    }
}

/* Raster-attribute sizing + RGB color register: "1;1;3;6 sets a 3x6 hint,
 * #0;2;100;0;0 defines register 0 as pure red and selects it, ~~~ draws
 * three fully-set (all 6 rows) columns. */
static void
test_raster_and_rgb_color(void) {
    uint8_t *rgba;
    int w, h;
    CHECK(decode("\"1;1;3;6#0;2;100;0;0~~~", &rgba, &w, &h));
    CHECK(w == 3);
    CHECK(h == 6);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 3; x++)
            check_pixel(rgba, w, x, y, 255, 0, 0, 255);
    free(rgba);
}

/* HLS color register: hue=0 is sixel-blue (rotated +240 from standard HSL),
 * lum=50/sat=100 -> pure blue. */
static void
test_hls_color(void) {
    uint8_t *rgba;
    int w, h;
    CHECK(decode("#0;1;0;50;100~", &rgba, &w, &h));
    CHECK(w == 1);
    CHECK(h == 6);
    for (int y = 0; y < 6; y++)
        check_pixel(rgba, w, 0, y, 0, 0, 255, 255);
    free(rgba);
}

/* Repeat count: !5~ draws 5 columns of a fully-set sixel char, same as
 * writing ~ five times. */
static void
test_repeat_count(void) {
    uint8_t *rgba;
    int w, h;
    CHECK(decode("#0;2;0;100;0!5~", &rgba, &w, &h));
    CHECK(w == 5);
    CHECK(h == 6);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 5; x++)
            check_pixel(rgba, w, x, y, 0, 255, 0, 255);
    free(rgba);
}

/* Color register reuse: define once, select later by index alone. */
static void
test_color_reuse(void) {
    uint8_t *rgba;
    int w, h;
    /* col 0: register 0 (red, just defined); col 1: register 0 reselected
     * by bare index, still red. */
    CHECK(decode("#0;2;100;0;0~#0~", &rgba, &w, &h));
    CHECK(w == 2);
    CHECK(h == 6);
    for (int y = 0; y < 6; y++) {
        check_pixel(rgba, w, 0, y, 255, 0, 0, 255);
        check_pixel(rgba, w, 1, y, 255, 0, 0, 255);
    }
    free(rgba);
}

/* Band transitions ($ carriage return, - graphical newline) plus a
 * known-good small reference image, verified pixel-for-pixel: band 0 lights
 * only the top-left pixel; band 1 (after -) lights only the bottom-right
 * 6-tall strip. $ is exercised by returning to column 0 mid-band before
 * moving on. */
static void
test_band_transitions_reference_image(void) {
    uint8_t *rgba;
    int w, h;
    /* "1;1;2;12: hint 2x12 (two bands). #0;2;100;0;0 red.
     * @ (value 1: bit0 only) at col 0, ? (value 0: nothing) at col 1,
     * then $ back to col 0 and ? again (no-op re-draw of nothing) to
     * exercise the carriage-return path, then - to band 1, ? then ~
     * (value 63: all 6 rows) at col 1. */
    CHECK(decode("\"1;1;2;12#0;2;100;0;0@?$?-?~", &rgba, &w, &h));
    CHECK(w == 2);
    CHECK(h == 12);

    check_pixel(rgba, w, 0, 0, 255, 0, 0, 255); /* band 0, col 0, row 0: lit */
    for (int y = 1; y < 6; y++)
        check_pixel(rgba, w, 0, y, 0, 0, 0, 0); /* band 0, col 0, rows 1-5: unlit */
    for (int y = 0; y < 6; y++)
        check_pixel(rgba, w, 1, y, 0, 0, 0, 0); /* band 0, col 1: untouched */

    check_pixel(rgba, w, 0, 6, 0, 0, 0, 0); /* band 1, col 0: untouched */
    for (int y = 6; y < 12; y++)
        check_pixel(rgba, w, 1, y, 255, 0, 0, 255); /* band 1, col 1: all lit */

    free(rgba);
}

/* Empty/malformed input decodes to nothing, not a crash. */
static void
test_empty_input(void) {
    uint8_t *rgba;
    int w, h;
    CHECK(!decode("", &rgba, &w, &h));
    CHECK(!decode("\"1;1;0;0", &rgba, &w, &h)); /* raster hint alone, no pixels */
}

int main(void) {
    test_raster_and_rgb_color();
    test_hls_color();
    test_repeat_count();
    test_color_reuse();
    test_band_transitions_reference_image();
    test_empty_input();

    if (failures == 0)
        printf("test_sixel: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
