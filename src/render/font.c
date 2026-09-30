#include "render/font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "font"
#include "core/util.h"

static const char *const style_attrs[FONT_STYLE_COUNT] = {
    [FONT_REGULAR] = "",
    [FONT_BOLD] = "weight=bold",
    [FONT_ITALIC] = "slant=italic",
    [FONT_BOLD_ITALIC] = "weight=bold:slant=italic",
};

bool fonts_init(void) {
    return fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_WARNING);
}

void fonts_fini(void) {
    fcft_fini();
}

static struct fcft_font *
load_face(const struct fonts *f, enum font_style style) {
    char attrs[64];
    snprintf(attrs, sizeof(attrs), "dpi=%d%s%s", f->dpi,
             style_attrs[style][0] != '\0' ? ":" : "", style_attrs[style]);
    const char *names[] = {f->name};
    return fcft_from_name(1, names, attrs);
}

bool fonts_load(struct fonts *f, const char *name, int dpi) {
    *f = (struct fonts){.name = xstrdup(name), .dpi = dpi > 0 ? dpi : 96};

    struct fcft_font *regular = load_face(f, FONT_REGULAR);
    if (regular == NULL) {
        LOG_ERR("failed to load font '%s'", name);
        free(f->name);
        f->name = NULL;
        return false;
    }
    f->face[FONT_REGULAR] = regular;

    /* Cell width from the advance of a typical glyph, not the widest one:
     * max_advance is often inflated by a few odd glyphs. */
    const struct fcft_glyph *m = fcft_rasterize_char_utf32(regular, 'M', FCFT_SUBPIXEL_NONE);
    f->cell_width = m != NULL && m->advance.x > 0 ? m->advance.x : regular->max_advance.x;
    f->cell_height = MAX(regular->height, regular->ascent + regular->descent);
    f->baseline = regular->ascent;

    f->underline_thickness = MAX(regular->underline.thickness, 1);
    f->underline_pos = -regular->underline.position;
    f->strikeout_thickness = MAX(regular->strikeout.thickness, 1);
    f->strikeout_pos = -regular->strikeout.position;

    if (f->cell_width <= 0 || f->cell_height <= 0) {
        LOG_ERR("font '%s' has invalid metrics", name);
        fonts_destroy(f);
        return false;
    }

    LOG_INFO("%s: cell %dx%d", regular->name ? regular->name : name,
             f->cell_width, f->cell_height);
    return true;
}

void fonts_destroy(struct fonts *f) {
    for (int i = 0; i < FONT_STYLE_COUNT; i++)
        fcft_destroy(f->face[i]);
    free(f->name);
    *f = (struct fonts){0};
}

bool fonts_reload(struct fonts *f, const char *name, int dpi) {
    struct fonts next;
    if (!fonts_load(&next, name, dpi))
        return false;
    fonts_destroy(f);
    *f = next;
    return true;
}

double fonts_pattern_size(const char *pattern, double fallback) {
    const char *p = strstr(pattern, "size=");
    while (p != NULL && p != pattern && p[-1] != ':')
        p = strstr(p + 1, "size=");
    if (p == NULL)
        return fallback;
    double v = strtod(p + 5, NULL);
    return v > 0 ? v : fallback;
}

char *fonts_pattern_with_size(const char *pattern, double size) {
    size_t cap = strlen(pattern) + 32;
    char *out = xmalloc(cap);
    size_t n = 0;

    /* Copy every ':'-separated token except size/pixelsize, then append ours. */
    for (const char *s = pattern; *s != '\0';) {
        const char *end = strchr(s, ':');
        size_t len = end != NULL ? (size_t)(end - s) : strlen(s);
        bool drop = strncmp(s, "size=", 5) == 0 || strncmp(s, "pixelsize=", 10) == 0;
        if (!drop && len > 0) {
            if (n > 0)
                out[n++] = ':';
            memcpy(out + n, s, len);
            n += len;
        }
        s += len + (end != NULL ? 1 : 0);
    }
    snprintf(out + n, cap - n, "%ssize=%g", n > 0 ? ":" : "", size);
    return out;
}

static struct fcft_font *
face_for(struct fonts *f, enum font_style style) {
    if (f->face[style] != NULL)
        return f->face[style];

    if (!f->face_failed[style]) {
        f->face[style] = load_face(f, style);
        if (f->face[style] == NULL) {
            LOG_WARN("no %s face for '%s'; using regular", style_attrs[style], f->name);
            f->face_failed[style] = true;
        }
    }
    return f->face[style] != NULL ? f->face[style] : f->face[FONT_REGULAR];
}

const struct fcft_glyph *
fonts_glyph(struct fonts *f, uint32_t cp, enum font_style style) {
    return fcft_rasterize_char_utf32(face_for(f, style), cp, FCFT_SUBPIXEL_NONE);
}

const struct fcft_grapheme *
fonts_grapheme(struct fonts *f, const uint32_t *cps, size_t count, enum font_style style) {
    if (!(fcft_capabilities() & FCFT_CAPABILITY_GRAPHEME_SHAPING))
        return NULL;
    return fcft_rasterize_grapheme_utf32(face_for(f, style), count, cps, FCFT_SUBPIXEL_NONE);
}
