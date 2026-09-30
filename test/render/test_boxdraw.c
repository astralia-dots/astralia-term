/* Procedural box-drawing and block geometry. */
#include <stdio.h>
#include <string.h>

#include "render/boxdraw.h"

#define CW 11
#define CH 23
#define LIGHT 2

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static uint8_t px[CH][CW];

static struct boxdraw_spec
render(uint32_t cp) {
    struct boxdraw_spec s;
    memset(px, 0, sizeof(px));
    if (!boxdraw_lookup(cp, &s)) {
        s.kind = BOXDRAW_LINES;
        return s;
    }
    if (s.kind == BOXDRAW_ARC || s.kind == BOXDRAW_DIAG) {
        static uint8_t mask[CH * 12];
        boxdraw_mask(&s, CW, CH, LIGHT, mask, 12);
        for (int y = 0; y < CH; y++)
            memcpy(px[y], mask + y * 12, CW);
        return s;
    }
    struct boxdraw_rect rects[BOXDRAW_MAX_RECTS];
    int n = boxdraw_rects(&s, CW, CH, LIGHT, rects);
    for (int i = 0; i < n; i++)
        for (int y = rects[i].y; y < rects[i].y + rects[i].h; y++)
            for (int x = rects[i].x; x < rects[i].x + rects[i].w; x++) {
                CHECK(x >= 0 && x < CW && y >= 0 && y < CH);
                px[y][x] = rects[i].alpha;
            }
    return s;
}

static int
count(void) {
    int n = 0;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++)
            n += px[y][x] != 0;
    return n;
}

static bool
column_full(int x) {
    for (int y = 0; y < CH; y++)
        if (!px[y][x])
            return false;
    return true;
}

static bool
row_full(int y) {
    for (int x = 0; x < CW; x++)
        if (!px[y][x])
            return false;
    return true;
}

static void
test_range(void) {
    struct boxdraw_spec s;
    CHECK(!boxdraw_lookup('a', &s));
    CHECK(!boxdraw_lookup(0x24ff, &s));
    CHECK(!boxdraw_lookup(0x25a0, &s));
    for (uint32_t cp = BOXDRAW_FIRST; cp <= BOXDRAW_LAST; cp++) {
        CHECK(boxdraw_lookup(cp, &s));
        if (s.kind == BOXDRAW_LINES)
            CHECK(s.left | s.right | s.up | s.down);
    }
}

static void
test_lines(void) {
    render(0x2502);
    CHECK(count() == LIGHT * CH);
    CHECK(column_full((CW - LIGHT) / 2));

    render(0x2503);
    CHECK(count() == LIGHT * 2 * CH);

    render(0x2500);
    CHECK(count() == LIGHT * CW);
    CHECK(row_full((CH - LIGHT) / 2));

    render(0x253c);
    CHECK(column_full((CW - LIGHT) / 2));
    CHECK(row_full((CH - LIGHT) / 2));
    CHECK(count() == LIGHT * CH + LIGHT * CW - LIGHT * LIGHT);

    render(0x250c);
    CHECK(!px[0][CW / 2] && px[CH - 1][CW / 2] && !px[CH / 2][0] && px[CH / 2][CW - 1]);
    CHECK(px[(CH - LIGHT) / 2][(CW - LIGHT) / 2]);

    render(0x2574);
    CHECK(px[CH / 2][0] && !px[CH / 2][CW - 1]);
}

static void
test_double(void) {
    int hy = (CH - 3 * LIGHT) / 2, vx = (CW - 3 * LIGHT) / 2;

    render(0x2551);
    CHECK(column_full(vx) && column_full(vx + 2 * LIGHT) && !column_full(vx + LIGHT));

    render(0x2550);
    CHECK(row_full(hy) && row_full(hy + 2 * LIGHT) && !row_full(hy + LIGHT));

    render(0x2554);
    CHECK(px[hy][vx] && px[hy][CW - 1] && px[CH - 1][vx]);
    CHECK(!px[hy + LIGHT][vx + LIGHT] && !px[hy + LIGHT][CW - 1]);
    CHECK(px[hy + 2 * LIGHT][vx + 2 * LIGHT] && px[CH - 1][vx + 2 * LIGHT]);
    CHECK(!px[hy + 2 * LIGHT][vx + LIGHT] && !px[hy + LIGHT][vx + 2 * LIGHT]);

    render(0x256c);
    CHECK(!px[hy + LIGHT][0] && !px[0][vx + LIGHT]);
    CHECK(!px[hy + LIGHT][vx + LIGHT]);
    CHECK(px[hy][0] && px[0][vx] && px[CH - 1][vx + 2 * LIGHT] && px[hy + 2 * LIGHT][CW - 1]);

    render(0x256b);
    CHECK(row_full((CH - LIGHT) / 2) && column_full(vx) && column_full(vx + 2 * LIGHT));
}

static void
test_dashes(void) {
    render(0x2504);
    int y = (CH - LIGHT) / 2;
    int runs = 0;
    for (int x = 0; x < CW; x++)
        runs += px[y][x] && (x == 0 || !px[y][x - 1]);
    CHECK(runs == 3);
    int first_end = 0, second_start = 0;
    while (!px[y][first_end])
        first_end++;
    int lead = first_end, trail = 0;
    while (px[y][first_end])
        first_end++;
    for (second_start = first_end; !px[y][second_start]; second_start++)
        ;
    while (!px[y][CW - 1 - trail])
        trail++;
    CHECK(lead + trail == second_start - first_end);

    render(0x254e);
    int x = (CW - LIGHT) / 2;
    runs = 0;
    for (int yy = 0; yy < CH; yy++)
        runs += px[yy][x] && (yy == 0 || !px[yy - 1][x]);
    CHECK(runs == 2);
    CHECK(!px[0][x] && !px[CH - 1][x]);
}

static void
test_blocks(void) {
    render(0x2588);
    CHECK(count() == CW * CH);

    render(0x2580);
    CHECK(count() == CW * (CH / 2) && px[0][0] && !px[CH / 2][0]);

    render(0x2584);
    CHECK(count() == CW * (CH - CH * 4 / 8) && !px[0][0] && px[CH - 1][0]);

    render(0x258f);
    CHECK(px[0][0] && !px[0][CW * 1 / 8 + 1]);

    render(0x2591);
    CHECK(count() == CW * CH && px[0][0] < 128 && px[0][0] > 0);
    uint8_t light = px[0][0];
    render(0x2593);
    CHECK(px[0][0] > light);

    render(0x2599);
    CHECK(px[0][0] && !px[0][CW - 1] && px[CH - 1][0] && px[CH - 1][CW - 1]);

    render(0x259a);
    CHECK(px[0][0] && !px[0][CW - 1] && !px[CH - 1][0] && px[CH - 1][CW - 1]);
}

static void
test_arcs_and_diagonals(void) {
    int cx = (CW - LIGHT) / 2, cy = (CH - LIGHT) / 2;

    render(0x256d);
    CHECK(px[CH - 1][cx] > 128 && px[cy][CW - 1] > 128);
    CHECK(px[0][0] == 0 && px[0][CW - 1] == 0 && px[CH - 1][0] == 0);

    render(0x256f);
    CHECK(px[0][cx] > 128 && px[cy][0] > 128 && px[CH - 1][CW - 1] == 0);

    render(0x2571);
    CHECK(px[0][CW - 1] > 0 && px[CH - 1][0] > 0 && px[0][0] == 0 && px[CH - 1][CW - 1] == 0);
    int slash = count();

    render(0x2572);
    CHECK(px[0][0] > 0 && px[CH - 1][CW - 1] > 0 && px[0][CW - 1] == 0);
    CHECK(count() == slash);

    render(0x2573);
    CHECK(px[0][0] > 0 && px[0][CW - 1] > 0 && px[CH - 1][0] > 0 && px[CH - 1][CW - 1] > 0);
}

int main(void) {
    test_range();
    test_lines();
    test_double();
    test_dashes();
    test_blocks();
    test_arcs_and_diagonals();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
