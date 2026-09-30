#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <fcft/fcft.h>

enum font_style {
    FONT_REGULAR,
    FONT_BOLD,
    FONT_ITALIC,
    FONT_BOLD_ITALIC,
    FONT_STYLE_COUNT,
};

struct fonts {
    char *name; /* fontconfig pattern */
    int dpi;
    struct fcft_font *face[FONT_STYLE_COUNT]; /* bold/italic loaded lazily */
    bool face_failed[FONT_STYLE_COUNT];

    int cell_width, cell_height;
    int baseline;                           /* ascent: y offset of the baseline in a cell */
    int underline_pos, underline_thickness; /* from the baseline, downwards */
    int strikeout_pos, strikeout_thickness;
};

bool fonts_init(void);
void fonts_fini(void);

/* name is a fontconfig pattern such as "monospace:size=10". */
bool fonts_load(struct fonts *f, const char *name, int dpi);
void fonts_destroy(struct fonts *f);

/* Loads a new set and swaps it in; on failure `f` is left untouched. */
bool fonts_reload(struct fonts *f, const char *name, int dpi);

/* Point size of a pattern's `size=` token, or `fallback` when it has none. */
double fonts_pattern_size(const char *pattern, double fallback);

/* Copy of `pattern` with its `size=`/`pixelsize=` tokens replaced by `size`; caller frees. */
char *fonts_pattern_with_size(const char *pattern, double size);

const struct fcft_glyph *fonts_glyph(struct fonts *f, uint32_t cp, enum font_style style);

/* Shaped glyphs for a base character plus combining marks; NULL when fcft
 * was built without grapheme shaping or shaping failed. */
const struct fcft_grapheme *fonts_grapheme(struct fonts *f, const uint32_t *cps, size_t count,
                                           enum font_style style);
