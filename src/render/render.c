#include "render/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "render"
#include "core/util.h"
#include "render/boxdraw.h"
#include "term/kitty_placeholder.h"
#include "term/selection.h"
#include "term/url.h"

#define LINK_HOVER_FG 0x9b57f4 /* astralia-shell-i3 palette::accent */

#define BELL_FLASH_ALPHA 0x4000 /* opacity of the visual-bell overlay, 0..0xffff */

/* Solid-color source reused between glyphs of the same color */
static pixman_image_t *solid_img;
static uint32_t solid_rgb;

/* Premultiplied, as pixman expects for a8r8g8b8 destinations */
static pixman_color_t
to_pixman(uint32_t rgb, uint16_t alpha) {
    return (pixman_color_t){
        .red = (uint16_t)(((rgb >> 16) & 0xff) * 257u * alpha / 0xffff),
        .green = (uint16_t)(((rgb >> 8) & 0xff) * 257u * alpha / 0xffff),
        .blue = (uint16_t)((rgb & 0xff) * 257u * alpha / 0xffff),
        .alpha = alpha,
    };
}

static pixman_image_t *
solid(uint32_t rgb) {
    if (solid_img == NULL || solid_rgb != rgb) {
        if (solid_img != NULL)
            pixman_image_unref(solid_img);
        pixman_color_t c = to_pixman(rgb, 0xffff);
        solid_img = pixman_image_create_solid_fill(&c);
        solid_rgb = rgb;
    }
    return solid_img;
}

static void
fill_alpha(pixman_image_t *dst, uint32_t rgb, uint16_t alpha, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    pixman_color_t c = to_pixman(rgb, alpha);
    pixman_box32_t box = {x, y, x + w, y + h};
    pixman_image_fill_boxes(PIXMAN_OP_SRC, dst, &c, 1, &box);
}

static void
fill(pixman_image_t *dst, uint32_t rgb, int x, int y, int w, int h) {
    fill_alpha(dst, rgb, 0xffff, x, y, w, h);
}

void render_init(struct renderer *r, struct fonts *fonts) {
    *r = (struct renderer){
        .fonts = fonts,
        .focused = true,
        .bg_alpha = 0xffff,
        .last_cursor_row = -1,
        .force_full = true,
    };
}

void render_invalidate(struct renderer *r) {
    r->force_full = true;
}

bool render_needed(const struct renderer *r, const struct term *t) {
    if (t->modes.sync_updates)
        return false;
    if (r->force_full || r->bell_on != r->last_bell_on || t->cursor_dirty || t->view_changed ||
        t->colors_changed || t->selection_changed)
        return true;
    for (int i = 0; i < t->rows; i++) {
        /* grid_row() only allocates, so casting away const is harmless */
        if (grid_row(((struct term *)t)->grid, i)->dirty)
            return true;
    }
    return false;
}

static inline uint32_t
default_fg(const struct term *t) {
    return t->modes.reverse_video ? t->default_bg : t->default_fg;
}

static inline uint32_t
default_bg(const struct term *t) {
    return t->modes.reverse_video ? t->default_fg : t->default_bg;
}

static uint32_t
resolve(const struct term *t, uint32_t color, bool is_fg, bool bold) {
    switch (COLOR_TAG(color)) {
    case COLOR_PALETTE: {
        uint32_t idx = COLOR_VALUE(color) & 0xff;
        if (is_fg && bold && idx < 8)
            idx += 8; /* bold as bright */
        return t->palette[idx];
    }
    case COLOR_RGB:
        return COLOR_VALUE(color);
    default:
        return is_fg ? default_fg(t) : default_bg(t);
    }
}

static uint32_t
blend_half(uint32_t a, uint32_t b) {
    return ((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1);
}

static void
cell_colors(const struct renderer *r, const struct term *t, const struct cell *c,
            bool block_cursor, bool selected, uint32_t *fg, uint32_t *bg, uint16_t *bg_alpha) {
    uint32_t f = resolve(t, c->fg, true, c->attrs & ATTR_BOLD);
    uint32_t b = resolve(t, c->bg, false, false);

    /* Only the default background is translucent */
    if (bg_alpha != NULL) {
        bool default_bg_shows = COLOR_TAG(c->bg) == COLOR_DEFAULT &&
                                !(c->attrs & ATTR_REVERSE) && !block_cursor && !selected;
        *bg_alpha = default_bg_shows ? r->frame_alpha : 0xffff;
    }

    if (c->attrs & ATTR_REVERSE) {
        uint32_t tmp = f;
        f = b;
        b = tmp;
    }
    if (selected && !block_cursor) {
        uint32_t tmp = f;
        f = b;
        b = tmp;
        if (f == b) {
            f = default_bg(t);
            b = default_fg(t);
        }
    }
    if (c->attrs & ATTR_DIM)
        f = blend_half(f, b);
    if (block_cursor && t->cursor_color_set) {
        f = b;
        b = t->cursor_color;
    } else if (block_cursor) {
        uint32_t tmp = f;
        f = b;
        b = tmp;
        if (f == b) {
            f = default_bg(t);
            b = default_fg(t);
        }
    }
    if (c->attrs & ATTR_INVISIBLE)
        f = b;

    *fg = f;
    *bg = b;
}

static enum font_style
cell_style(const struct cell *c) {
    bool bold = c->attrs & ATTR_BOLD;
    bool italic = c->attrs & ATTR_ITALIC;
    return bold && italic ? FONT_BOLD_ITALIC : bold ? FONT_BOLD
                                           : italic ? FONT_ITALIC
                                                    : FONT_REGULAR;
}

static void
draw_decorations(const struct renderer *r, pixman_image_t *dst, const struct cell *c,
                 uint32_t fg, int x, int y, int w, bool link) {
    const struct fonts *f = r->fonts;
    int ul = (c->attrs & ATTR_UNDERLINE_MASK) >> ATTR_UNDERLINE_SHIFT;
    if (link && ul == 0)
        ul = 1;

    if (ul != 0) {
        int thick = f->underline_thickness;
        int uy = y + f->baseline + f->underline_pos - thick / 2;
        uy = MIN(uy, y + f->cell_height - thick);
        fill(dst, fg, x, uy, w, thick);
        if (ul == 2) /* double; curly/dotted/dashed draw as single for now */
            fill(dst, fg, x, MIN(uy + thick * 2, y + f->cell_height - thick), w, thick);
    }
    if (c->attrs & ATTR_STRIKE) {
        int thick = f->strikeout_thickness;
        fill(dst, fg, x, y + f->baseline + f->strikeout_pos - thick / 2, w, thick);
    }
    if (c->attrs & ATTR_OVERLINE)
        fill(dst, fg, x, y, w, f->underline_thickness);
}

static uint8_t glyph_lut[256];
static bool glyph_lut_ready;

/* Light text on dark backgrounds looks thin under plain OVER, so its glyph
 * alpha is remapped through a gamma table (ASTRALIA_GLYPH_GAMMA, default 1.4). */
static bool
glyph_boost(uint32_t fg) {
    if (!glyph_lut_ready) {
        const char *env = getenv("ASTRALIA_GLYPH_GAMMA");
        double gamma = env != NULL ? atof(env) : 1.4;
        if (gamma <= 0)
            gamma = 1.0;
        for (int i = 0; i < 256; i++)
            glyph_lut[i] = (uint8_t)(255.0 * pow(i / 255.0, 1.0 / gamma) + 0.5);
        glyph_lut_ready = true;
    }
    uint32_t luma = (((fg >> 16) & 0xff) * 54 + ((fg >> 8) & 0xff) * 183 + (fg & 0xff) * 19) >> 8;
    return luma >= 128;
}

static pixman_image_t *
boosted_mask(pixman_image_t *src) {
    int w = pixman_image_get_width(src), h = pixman_image_get_height(src);
    int sstride = pixman_image_get_stride(src);
    const uint8_t *s = (const uint8_t *)pixman_image_get_data(src);
    pixman_image_t *m = pixman_image_create_bits(PIXMAN_a8, w, h, NULL, 0);
    int dstride = pixman_image_get_stride(m);
    uint8_t *d = (uint8_t *)pixman_image_get_data(m);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            d[yy * dstride + xx] = glyph_lut[s[yy * sstride + xx]];
    return m;
}

static void
blit_glyph(const struct renderer *r, pixman_image_t *dst, const struct fcft_glyph *g,
           uint32_t fg, int x, int y) {
    if (g == NULL || g->pix == NULL)
        return;

    int gx = x + g->x;
    int gy = y + r->fonts->baseline - g->y;

    if (g->is_color_glyph ||
        pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8) {
        pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, dst,
                                 0, 0, 0, 0, gx, gy, g->width, g->height);
    } else if (glyph_boost(fg) && pixman_image_get_format(g->pix) == PIXMAN_a8) {
        pixman_image_t *m = boosted_mask(g->pix);
        pixman_image_composite32(PIXMAN_OP_OVER, solid(fg), m, dst,
                                 0, 0, 0, 0, gx, gy, g->width, g->height);
        pixman_image_unref(m);
    } else {
        pixman_image_composite32(PIXMAN_OP_OVER, solid(fg), g->pix, dst,
                                 0, 0, 0, 0, gx, gy, g->width, g->height);
    }
}

static void
draw_text(const struct renderer *r, const struct term *t, pixman_image_t *dst,
          const struct cell *c, uint32_t fg, int x, int y) {
    enum font_style style = cell_style(c);
    const struct composed_chain *chain = composed_get(&t->composed, c->cp);
    if (chain == NULL) {
        blit_glyph(r, dst, fonts_glyph(r->fonts, c->cp, style), fg, x, y);
        return;
    }

    const struct fcft_grapheme *gr = fonts_grapheme(r->fonts, chain->cps, chain->count, style);
    if (gr != NULL) {
        for (size_t i = 0; i < gr->count; i++) {
            blit_glyph(r, dst, gr->glyphs[i], fg, x, y);
            x += gr->glyphs[i]->advance.x;
        }
        return;
    }

    /* No shaping: overlay the marks on the base glyph */
    for (int i = 0; i < chain->count; i++)
        blit_glyph(r, dst, fonts_glyph(r->fonts, chain->cps[i], style), fg, x, y);
}

/* Reused across box-drawing cells, like placeholder_buf below */
static uint8_t *boxdraw_buf;
static size_t boxdraw_buf_size;

static void
fill_over(pixman_image_t *dst, uint32_t rgb, uint8_t alpha, int x, int y, int w, int h) {
    pixman_color_t c = to_pixman(rgb, alpha * 257u);
    pixman_box32_t box = {x, y, x + w, y + h};
    pixman_image_fill_boxes(PIXMAN_OP_OVER, dst, &c, 1, &box);
}

static void
draw_boxdraw(const struct renderer *r, pixman_image_t *dst, const struct boxdraw_spec *spec,
             uint32_t fg, int x, int y) {
    int cw = r->fonts->cell_width, ch = r->fonts->cell_height;
    /* A light line is 1pt, ceil(96/72) = 2px at 96 dpi */
    int light = MAX(2, r->fonts->underline_thickness);

    if (spec->kind == BOXDRAW_ARC || spec->kind == BOXDRAW_DIAG) {
        int stride = (cw + 3) & ~3;
        size_t size = (size_t)stride * ch;
        if (size > boxdraw_buf_size) {
            free(boxdraw_buf);
            boxdraw_buf = xmalloc(size);
            boxdraw_buf_size = size;
        }
        boxdraw_mask(spec, cw, ch, light, boxdraw_buf, stride);
        pixman_image_t *mask =
            pixman_image_create_bits(PIXMAN_a8, cw, ch, (uint32_t *)(void *)boxdraw_buf, stride);
        pixman_image_composite32(PIXMAN_OP_OVER, solid(fg), mask, dst, 0, 0, 0, 0, x, y, cw, ch);
        pixman_image_unref(mask);
        return;
    }

    struct boxdraw_rect rects[BOXDRAW_MAX_RECTS];
    int n = boxdraw_rects(spec, cw, ch, light, rects);
    for (int i = 0; i < n; i++)
        fill_over(dst, fg, rects[i].alpha, x + rects[i].x, y + rects[i].y, rects[i].w, rects[i].h);
}

/* ---- kitty graphics: Unicode placeholders ---- */
/* Diacritic decoding and left-neighbor inheritance live in
 * term/kitty_placeholder.c (no pixman/fcft dependency, so they're testable
 * standalone); this file only does the pixel-slice blit. */

/* Reused across placeholder cells within a frame, like solid_img above. */
static uint32_t *placeholder_buf;
static int placeholder_buf_w, placeholder_buf_h;

/* Blits the (row,col)-th cell-sized slice of img's plc->src_* rectangle (per
 * plc's cols x rows grid) at (x,y), nearest-neighbor sampled and premultiplied against dst.
 * img->rgba is straight alpha; pixman's a8r8g8b8 expects premultiplied. */
static void
blit_placeholder_image(const struct renderer *r, pixman_image_t *dst,
                       const struct graphics_image *img, const struct graphics_placement *plc,
                       int row, int col, int x, int y) {
    int cw = r->fonts->cell_width, ch = r->fonts->cell_height;
    int sx0 = plc->src_x + (int)((int64_t)plc->src_w * col / plc->cols);
    int sx1 = plc->src_x + (int)((int64_t)plc->src_w * (col + 1) / plc->cols);
    int sy0 = plc->src_y + (int)((int64_t)plc->src_h * row / plc->rows);
    int sy1 = plc->src_y + (int)((int64_t)plc->src_h * (row + 1) / plc->rows);
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0)
        return;

    if (placeholder_buf == NULL || placeholder_buf_w != cw || placeholder_buf_h != ch) {
        free(placeholder_buf);
        placeholder_buf = xmalloc((size_t)cw * ch * 4);
        placeholder_buf_w = cw;
        placeholder_buf_h = ch;
    }

    for (int dy = 0; dy < ch; dy++) {
        int sy = sy0 + dy * sh / ch;
        for (int dx = 0; dx < cw; dx++) {
            int sx = sx0 + dx * sw / cw;
            const uint8_t *s = img->rgba + ((size_t)sy * img->width + sx) * 4;
            uint8_t a = s[3];
            placeholder_buf[dy * cw + dx] = ((uint32_t)a << 24) |
                                            ((uint32_t)(s[0] * a / 255) << 16) |
                                            ((uint32_t)(s[1] * a / 255) << 8) |
                                            (uint32_t)(s[2] * a / 255);
        }
    }

    pixman_image_t *src =
        pixman_image_create_bits(PIXMAN_a8r8g8b8, cw, ch, placeholder_buf, cw * 4);
    pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, dst, 0, 0, 0, 0, x, y, cw, ch);
    pixman_image_unref(src);
}

/* Draws an ATTR_IMAGE cell (a non-virtual placement, written directly into
 * the grid by graphics_place_nonvirtual()): unlike a placeholder cell, its
 * placement handle (ul) is already the lookup key, and tile_row/tile_col are
 * already resolved -- no diacritic decode, since the terminal synthesized
 * these cells itself rather than an app printing them. */
static void
draw_image_cell(const struct renderer *r, struct term *t, pixman_image_t *dst,
                const struct cell *c, int x, int y) {
    struct graphics_placement *plc = graphics_placement_get_nonvirtual(&t->graphics, c->ul);
    if (plc == NULL || plc->cols <= 0 || plc->rows <= 0)
        return;
    if (c->tile_row >= plc->rows || c->tile_col >= plc->cols)
        return;
    struct graphics_image *img = graphics_get(&t->graphics, plc->image_id);
    if (img == NULL)
        return;
    blit_placeholder_image(r, dst, img, plc, c->tile_row, c->tile_col, x, y);
}

/* Draws cell c (already known to be a placeholder) at (x,y): resolves its
 * row/col/image id/placement id and blits the matching image slice. Does
 * nothing if the diacritics, image or placement can't be resolved. */
static void
draw_placeholder(const struct renderer *r, struct term *t, pixman_image_t *dst,
                 const struct kitty_placeholder_cell *pc, struct kitty_placeholder_run *run,
                 const struct cell *c, int x, int y) {
    int row, col, msb;
    if (!kitty_placeholder_resolve(run, pc, c->fg, c->ul, &row, &col, &msb))
        return;

    uint32_t image_id = COLOR_VALUE(c->fg) | ((uint32_t)(msb & 0xff) << 24);
    uint32_t placement_id = COLOR_VALUE(c->ul);

    struct graphics_image *img = graphics_get(&t->graphics, image_id);
    if (img == NULL)
        return;
    struct graphics_placement *plc = graphics_placement_get(&t->graphics, image_id, placement_id);
    if (plc == NULL)
        plc = graphics_placement_get_any(&t->graphics, image_id);
    if (plc == NULL || plc->cols <= 0 || plc->rows <= 0 || row >= plc->rows || col >= plc->cols)
        return;

    blit_placeholder_image(r, dst, img, plc, row, col, x, y);
}

static void
draw_row(struct renderer *r, struct term *t, int row_idx, pixman_image_t *dst) {
    const int cw = r->fonts->cell_width;
    const int ch = r->fonts->cell_height;
    const int y = r->pad_y + row_idx * ch;
    const struct cell *cells = term_view_row(t, row_idx)->cells;

    bool cursor_here = t->modes.cursor_visible && !r->blink_off &&
                       t->cursor.row + t->view_offset == row_idx;
    int cursor_col = cursor_here ? t->cursor.col : -1;
    bool block = cursor_here && t->cursor_style == CURSOR_BLOCK && r->focused;

    int sel_from = 0, sel_to = 0;
    selection_row_span(t, row_idx - t->view_offset, &sel_from, &sel_to);

    int live_row = row_idx - t->view_offset;
    bool *links = xmalloc(t->cols);
    url_row_links(t, live_row, links);

    int hover_from = 0, hover_to = 0;
    const struct grid_range *hover = &t->link_hover.range;
    if (t->link_hover.active && live_row >= hover->start.row && live_row <= hover->end.row) {
        hover_from = live_row == hover->start.row ? hover->start.col : 0;
        hover_to = live_row == hover->end.row ? hover->end.col + 1 : t->cols;
    }

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, r->pad_x, y, t->cols * cw, ch);
    pixman_image_set_clip_region32(dst, &clip);

    /* Backgrounds, merged into runs of equal color */
    int run_start = 0;
    uint32_t run_bg = 0;
    uint16_t run_alpha = 0xffff;
    for (int col = 0; col <= t->cols; col++) {
        uint32_t fg, bg = 0;
        uint16_t alpha = 0xffff;
        if (col < t->cols) {
            bool on_cursor = block && (col == cursor_col ||
                                       (cells[col].cp == CELL_SPACER && col - 1 == cursor_col));
            bool selected = col >= sel_from && col < sel_to;
            cell_colors(r, t, &cells[col], on_cursor, selected, &fg, &bg, &alpha);
        }
        if (col == 0) {
            run_bg = bg;
            run_alpha = alpha;
            continue;
        }
        if (col == t->cols || bg != run_bg || alpha != run_alpha) {
            fill_alpha(dst, run_bg, run_alpha, r->pad_x + run_start * cw, y,
                       (col - run_start) * cw, ch);
            run_start = col;
            run_bg = bg;
            run_alpha = alpha;
        }
    }

    /* Glyphs and decorations */
    struct kitty_placeholder_run placeholder_run = {0};
    for (int col = 0; col < t->cols; col++) {
        const struct cell *c = &cells[col];
        if (c->cp == CELL_SPACER)
            continue;

        uint32_t fg, bg;
        bool selected = col >= sel_from && col < sel_to;
        cell_colors(r, t, c, block && col == cursor_col, selected, &fg, &bg, NULL);
        if (col >= hover_from && col < hover_to && !selected && !(block && col == cursor_col) &&
            !(c->attrs & ATTR_INVISIBLE))
            fg = LINK_HOVER_FG;
        int x = r->pad_x + col * cw;
        int w = (col + 1 < t->cols && cells[col + 1].cp == CELL_SPACER) ? 2 * cw : cw;

        if (c->attrs & ATTR_IMAGE) {
            placeholder_run.valid = false;
            draw_image_cell(r, t, dst, c, x, y);
        } else {
            struct kitty_placeholder_cell pc = kitty_placeholder_decode(t, c->cp);
            if (pc.is_placeholder) {
                draw_placeholder(r, t, dst, &pc, &placeholder_run, c, x, y);
            } else {
                placeholder_run.valid = false;
                struct boxdraw_spec box;
                if (!(c->attrs & ATTR_INVISIBLE)) {
                    if (boxdraw_lookup(c->cp, &box))
                        draw_boxdraw(r, dst, &box, fg, x, y);
                    else if (c->cp > ' ')
                        draw_text(r, t, dst, c, fg, x, y);
                }
            }
        }
        if (links[col] || (c->attrs & (ATTR_UNDERLINE_MASK | ATTR_STRIKE | ATTR_OVERLINE)))
            draw_decorations(r, dst, c, fg, x, y, w, links[col]);
    }
    free(links);

    /* Non-block cursors, and the hollow block when unfocused */
    if (cursor_here && !block) {
        const struct cell *c = &cells[cursor_col];
        uint32_t fg, bg;
        bool selected = cursor_col >= sel_from && cursor_col < sel_to;
        cell_colors(r, t, c, false, selected, &fg, &bg, NULL);
        if (t->cursor_color_set)
            fg = t->cursor_color;
        int x = r->pad_x + cursor_col * cw;
        int w = (cursor_col + 1 < t->cols && cells[cursor_col + 1].cp == CELL_SPACER)
                    ? 2 * cw
                    : cw;

        switch (t->cursor_style) {
        case CURSOR_BLOCK: /* unfocused */
            fill(dst, fg, x, y, w, 1);
            fill(dst, fg, x, y + ch - 1, w, 1);
            fill(dst, fg, x, y, 1, ch);
            fill(dst, fg, x + w - 1, y, 1, ch);
            break;
        case CURSOR_UNDERLINE: {
            int thick = MAX(r->fonts->underline_thickness, 2);
            fill(dst, fg, x, y + ch - thick, w, thick);
            break;
        }
        case CURSOR_BAR:
            fill(dst, fg, x, y, MAX(cw / 8, 2), ch);
            break;
        }
    }

    pixman_image_set_clip_region32(dst, NULL);
    pixman_region32_fini(&clip);
}

void render_frame(struct renderer *r, struct term *t, struct buffer *buf,
                  pixman_region32_t *damage) {
    /* The flash overlay is baked into the frame, and the next frame copies
     * this one, so both the flash and the frame after it redraw everything. */
    bool full = r->force_full || buf->width != r->last_width ||
                buf->height != r->last_height || t->colors_changed || t->view_changed ||
                t->selection_changed || r->bell_on || r->last_bell_on;

    /* A different buffer than last time holds an older frame. Backends only
     * free buffers on a size change, so last_buf is valid if sizes match. */
    if (!full && buf != r->last_buf) {
        if (r->last_buf != NULL && r->last_buf->stride == buf->stride)
            memcpy(buf->data, r->last_buf->data, (size_t)buf->stride * buf->height);
        else
            full = true;
    }

    r->frame_alpha = pixman_image_get_format(buf->pix) == PIXMAN_a8r8g8b8
                         ? r->bg_alpha
                         : 0xffff;

    if (full) {
        fill_alpha(buf->pix, default_bg(t), r->frame_alpha, 0, 0, buf->width, buf->height);
        grid_mark_all_dirty(t->grid);
        pixman_region32_union_rect(damage, damage, 0, 0, buf->width, buf->height);
    }

    /* Dirty flags track live rows; a scrolled-back view redraws everything */
    bool all_rows = full || t->view_offset > 0;
    if (!all_rows) {
        if (r->last_cursor_row >= 0 && r->last_cursor_row < t->rows) {
            if (r->last_cursor_row != t->cursor.row) {
                /* Links on the lines the cursor left and entered switch on/off */
                url_mark_line_dirty(t, r->last_cursor_row);
                url_mark_line_dirty(t, t->cursor.row);
            } else
                grid_row(t->grid, r->last_cursor_row)->dirty = true;
        }
        grid_row(t->grid, t->cursor.row)->dirty = true;
    }

    const int ch = r->fonts->cell_height;
    const int width = t->cols * r->fonts->cell_width;
    int max_rows = (buf->height - r->pad_y) / ch;
    int rows = MIN(t->rows, max_rows);

    for (int i = 0; i < t->rows; i++) {
        struct row *row = grid_row(t->grid, i);
        bool draw = all_rows || row->dirty;
        row->dirty = false;
        if (!draw || i >= rows)
            continue;
        draw_row(r, t, i, buf->pix);
        pixman_region32_union_rect(damage, damage, r->pad_x, r->pad_y + i * ch, width, ch);
    }

    if (r->bell_on) {
        pixman_color_t c = to_pixman(default_fg(t), BELL_FLASH_ALPHA);
        pixman_box32_t box = {0, 0, buf->width, buf->height};
        pixman_image_fill_boxes(PIXMAN_OP_OVER, buf->pix, &c, 1, &box);
    }
    r->last_bell_on = r->bell_on;

    t->cursor_dirty = false;
    t->view_changed = false;
    t->colors_changed = false;
    t->selection_changed = false;
    r->last_cursor_row = t->cursor.row + t->view_offset;
    r->last_cursor_col = t->cursor.col;
    r->last_buf = buf;
    r->last_width = buf->width;
    r->last_height = buf->height;
    r->force_full = false;
}
