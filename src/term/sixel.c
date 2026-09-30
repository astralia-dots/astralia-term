/* Sixel body decoder, adapted from foot's sixel.c decode algorithm
 * (Copyright (c) 2019 Daniel Eklöf, MIT; see LICENSE) and its hsl.c color
 * conversion. Simplified to a pure buffer-in/pixmap-out function: no
 * incremental streaming and no grid coupling (astralia-term gets scroll/
 * erase for free from the ATTR_IMAGE anchor cell scheme in graphics.c instead of
 * out-of-band sixel tracking), and no Pan/Pad pixel-doubling -- the
 * aspect-ratio scaling parameters are accepted but ignored, since real-world
 * sixel producers overwhelmingly leave them at their 1:1 default. */
#include "term/sixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "sixel"
#include "core/util.h"

struct sixel_decoder {
    uint8_t *rgba;  /* width * rows_alloc * 4, straight alpha, row-major */
    int width;      /* current pixel width; grows to fit as needed */
    int height;     /* highest row touched by a sixel char, + 1 */
    int rows_alloc; /* allocated rows (>= height), grows in bands of 6 */

    int col, row; /* cursor, in pixels; row is the top of the current band */

    uint32_t palette[SIXEL_MAX_COLORS]; /* packed 0x00RRGGBB, opaque */
    unsigned color_idx;
    uint32_t color;

    unsigned repeat_count;
    uint32_t params[5];
    int nparams;
    uint32_t param;

    enum { ST_MAIN, ST_RASTER, ST_REPEAT, ST_COLOR } state;

    bool error;
};

static uint32_t
accum_digit(uint32_t v, uint8_t c) {
    v = v * 10 + (uint32_t)(c - '0');
    return v > 0xffffffu ? 0xffffffu : v;
}

/* Sixel's HLS hues run blue=0/red=120/green=240, rotated from "standard"
 * HSL (red=0/green=120/blue=240); adapted from foot's hsl_to_rgb(). */
static uint32_t
hls_to_rgb(int hue, int lum, int sat) {
    hue = (hue + 240) % 360;
    double L = lum / 100.0, S = sat / 100.0;
    double C = (1. - fabs(2. * L - 1.)) * S;
    double X = C * (1. - fabs(fmod((double)hue / 60., 2.) - 1.));
    double m = L - C / 2.;
    double r, g, b;
    if (hue <= 60) {
        r = C;
        g = X;
        b = 0.;
    } else if (hue <= 120) {
        r = X;
        g = C;
        b = 0.;
    } else if (hue <= 180) {
        r = 0.;
        g = C;
        b = X;
    } else if (hue <= 240) {
        r = 0.;
        g = X;
        b = C;
    } else if (hue <= 300) {
        r = X;
        g = 0.;
        b = C;
    } else {
        r = C;
        g = 0.;
        b = X;
    }
    r += m;
    g += m;
    b += m;
    uint32_t R = (uint32_t)lround(r * 255.), G = (uint32_t)lround(g * 255.),
             B = (uint32_t)lround(b * 255.);
    return (R << 16) | (G << 8) | B;
}

static bool
ensure_width(struct sixel_decoder *d, int min_width) {
    if (min_width <= d->width)
        return true;
    if (min_width > (int)SIXEL_MAX_WIDTH)
        return false;
    if (d->rows_alloc > 0) {
        size_t new_stride = (size_t)min_width * 4;
        uint8_t *new_rgba = xcalloc(new_stride * d->rows_alloc, 1);
        size_t old_stride = (size_t)d->width * 4;
        for (int y = 0; y < d->rows_alloc; y++)
            memcpy(new_rgba + (size_t)y * new_stride, d->rgba + (size_t)y * old_stride, old_stride);
        free(d->rgba);
        d->rgba = new_rgba;
    }
    d->width = min_width;
    return true;
}

static bool
ensure_height(struct sixel_decoder *d, int min_height) {
    if (min_height <= d->height)
        return true;
    if (min_height > (int)SIXEL_MAX_HEIGHT)
        return false;
    int new_rows_alloc = ((min_height + 5) / 6) * 6;
    if (new_rows_alloc > d->rows_alloc) {
        size_t stride = (size_t)d->width * 4;
        uint8_t *new_rgba = xrealloc(d->rgba, stride * (size_t)new_rows_alloc);
        memset(new_rgba + stride * (size_t)d->rows_alloc, 0,
              stride * (size_t)(new_rows_alloc - d->rows_alloc));
        d->rgba = new_rgba;
        d->rows_alloc = new_rows_alloc;
    }
    d->height = min_height;
    return true;
}

static void
put_pixel(struct sixel_decoder *d, int x, int y, uint32_t rgb) {
    uint8_t *p = d->rgba + ((size_t)y * d->width + x) * 4;
    p[0] = (uint8_t)(rgb >> 16);
    p[1] = (uint8_t)(rgb >> 8);
    p[2] = (uint8_t)rgb;
    p[3] = 0xff;
}

static void
draw_sixel_run(struct sixel_decoder *d, uint8_t bits, unsigned count) {
    if (d->error)
        return;
    if (count == 0)
        count = 1;
    if (!ensure_width(d, d->col + (int)count) || !ensure_height(d, d->row + 6)) {
        d->error = true;
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        int x = d->col + (int)i;
        for (int b = 0; b < 6; b++)
            if (bits & (1u << b))
                put_pixel(d, x, d->row + b, d->color);
    }
    d->col += (int)count;
}

/* DECGRA "Pan;Pad;Ph;Pv: sizing hint only (Pan/Pad, the aspect-ratio
 * numerator/denominator, are accepted and ignored per this file's header
 * comment). Falls through to re-dispatch the terminating byte via ST_MAIN,
 * as a normal byte would. */
static void
finish_raster(struct sixel_decoder *d, uint8_t c) {
    if (d->nparams < (int)ARRAY_LEN(d->params))
        d->params[d->nparams++] = d->param;
    if (d->nparams > 2 && d->params[2] > 0)
        ensure_width(d, MIN((int)d->params[2], (int)SIXEL_MAX_WIDTH));
    if (d->nparams > 3 && d->params[3] > 0)
        ensure_height(d, MIN((int)d->params[3], (int)SIXEL_MAX_HEIGHT));
    d->state = ST_MAIN;
}

static void
finish_color(struct sixel_decoder *d) {
    if (d->nparams < (int)ARRAY_LEN(d->params))
        d->params[d->nparams++] = d->param;
    if (d->nparams > 0)
        d->color_idx = MIN(d->params[0], SIXEL_MAX_COLORS - 1);
    if (d->nparams > 4) {
        uint32_t format = d->params[1];
        int c1 = (int)d->params[2], c2 = (int)d->params[3], c3 = (int)d->params[4];
        if (format == 1) /* HLS: Pc;1;hue(0-360);lum(0-100);sat(0-100) */
            d->palette[d->color_idx] = hls_to_rgb(MIN(c1, 360), MIN(c2, 100), MIN(c3, 100));
        else if (format == 2) { /* RGB: Pc;2;r(0-100);g(0-100);b(0-100) */
            uint32_t r = 255 * (unsigned)MIN(c1, 100) / 100;
            uint32_t g = 255 * (unsigned)MIN(c2, 100) / 100;
            uint32_t b = 255 * (unsigned)MIN(c3, 100) / 100;
            d->palette[d->color_idx] = (r << 16) | (g << 8) | b;
        }
    }
    /* Always refresh the active color here (even after a full
     * Pc;Pu;Px;Py;Pz redefinition): most real producers draw with a color
     * immediately after defining it, in the same command. */
    d->color = d->palette[d->color_idx];
    d->state = ST_MAIN;
}

static void
dispatch_main(struct sixel_decoder *d, uint8_t c) {
    switch (c) {
    case '"':
        d->state = ST_RASTER;
        d->nparams = 0;
        d->param = 0;
        break;
    case '!':
        d->state = ST_REPEAT;
        d->repeat_count = 0;
        break;
    case '#':
        d->state = ST_COLOR;
        d->nparams = 0;
        d->param = 0;
        break;
    case '$': /* carriage return: back to the start of the current band */
        d->col = 0;
        break;
    case '-': /* graphical new line: next 6-row band */
        d->row += 6;
        d->col = 0;
        break;
    case '?' ... '~':
        draw_sixel_run(d, (uint8_t)(c - 63), 1);
        break;
    case ' ':
    case '\n':
    case '\r':
        break;
    default:
        break; /* lenient: ignore anything else rather than aborting */
    }
}

static void
feed_byte(struct sixel_decoder *d, uint8_t c) {
    if (d->error)
        return;
    switch (d->state) {
    case ST_MAIN:
        dispatch_main(d, c);
        break;
    case ST_RASTER:
        if (c >= '0' && c <= '9')
            d->param = accum_digit(d->param, c);
        else if (c == ';') {
            if (d->nparams < (int)ARRAY_LEN(d->params))
                d->params[d->nparams++] = d->param;
            d->param = 0;
        } else {
            finish_raster(d, c);
            dispatch_main(d, c);
        }
        break;
    case ST_REPEAT:
        if (c >= '0' && c <= '9') {
            uint32_t v = accum_digit(d->repeat_count, c);
            d->repeat_count = MIN(v, SIXEL_MAX_WIDTH);
        } else if (c >= '?' && c <= '~') {
            draw_sixel_run(d, (uint8_t)(c - 63), d->repeat_count != 0 ? d->repeat_count : 1);
            d->state = ST_MAIN;
        } else {
            d->state = ST_MAIN;
            dispatch_main(d, c);
        }
        break;
    case ST_COLOR:
        if (c >= '0' && c <= '9')
            d->param = accum_digit(d->param, c);
        else if (c == ';') {
            if (d->nparams < (int)ARRAY_LEN(d->params))
                d->params[d->nparams++] = d->param;
            d->param = 0;
        } else {
            finish_color(d);
            dispatch_main(d, c);
        }
        break;
    }
}

bool sixel_decode(const uint8_t *data, size_t len, uint8_t **out_rgba, int *out_width,
                  int *out_height) {
    struct sixel_decoder d = {0};

    for (size_t i = 0; i < len && !d.error; i++)
        feed_byte(&d, data[i]);

    /* Flush a pending raster/color header with no terminating byte (a
     * stream that ends mid-header rather than with more sixel data). */
    if (!d.error && d.state == ST_RASTER)
        finish_raster(&d, 0);
    else if (!d.error && d.state == ST_COLOR)
        finish_color(&d);

    if (d.error || d.width <= 0 || d.height <= 0) {
        LOG_WARN("sixel: empty or malformed image, discarding");
        free(d.rgba);
        return false;
    }

    *out_rgba = d.rgba;
    *out_width = d.width;
    *out_height = d.height;
    return true;
}
