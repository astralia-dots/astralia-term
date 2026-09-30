# Images as anchored overlays

## Problem

A non-virtual placement (kitty `a=T`/`a=p`, sixel) is written into the grid as `cols x rows` `ATTR_IMAGE` tile cells. Text printed into a narrow window wraps through those rows, and on widening the text cannot rejoin without moving tiles, so either the image tears (old code) or the rows freeze (current image-clip rule).

## Approach

- A placement writes only one `ATTR_IMAGE` cell: its top-left anchor (`ul` = handle). The rest of its area is left as ordinary cells.
- The anchor is a normal width-1 cell, so it scrolls, reflows, enters scrollback and is erased exactly like text; no new position tracking is needed.
- The renderer finds anchors on and up to 254 rows above the visible rows and composites each image over the rows it covers, after glyphs and before the cursor.
- Erasing the anchor (`ED`, `EL`, `ECH`, overwrite, scroll-out, `RIS`) removes the whole image; erasing other cells in its area does not, matching kitty's model.
- With tiles gone, `resize_reflow()` drops the `row_has_image()` clip rule: wrapped text rejoins freely and the image stays whole, anchored to its top-left cell.

## Files

### [MODIFY] `src/term/grid.h`

- `ATTR_IMAGE` comment: the cell is a placement's top-left anchor; `tile_row`/`tile_col` unused (0).
- `grid_resize()` comment: drop the image-row clause.

### [MODIFY] `src/term/grid.c`

- `resize_reflow()`: remove `row_has_image()` and the `clip` override it feeds; clip stays for unwrapped non-cursor rows.

### [MODIFY] `src/term/graphics.h`

- `struct graphics_placement` comment: non-virtual placements are anchored by one cell.
- `graphics_place_nonvirtual()` comment: writes the anchor only, still advances the cursor as today.
- [NEW API] `graphics_each_anchor(struct term *t, int row_lo, int row_hi, fn, user)`: calls `fn(user, placement, row, col)` for every live non-virtual placement whose area intersects live rows `[row_lo, row_hi]`, scanning anchors from `row_lo - 254` (clamped to scrollback) to `row_hi`. One scan shared by render and delete.

### [MODIFY] `src/term/graphics.c`

- `graphics_place_nonvirtual()`: write the anchor cell at the start position; still mark every covered row dirty and call `term_index()` per row, so cursor behavior and scrolling are unchanged.
- `blank_cells_for_handle()`: unchanged logic (it already finds the anchor); then `grid_mark_all_dirty()` both screens, since the image covered rows the anchor row doesn't.
- `delete_placements_at()`: use `graphics_each_anchor()` and test the placement rectangle against the query rectangle instead of scanning tile cells.
- Replace `row_has_image`-style overflow scan: `blank_cells_for_handle()` keeps scanning `overflow` (anchors can be clipped there).

### [MODIFY] `src/render/render.c`

- `draw_image_cell()`: removed; `ATTR_IMAGE` cells draw as blank.
- [NEW] `draw_images(r, t, row_idx, dst)`: called in `draw_row()` after the glyph loop, before the cursor; for each placement from `graphics_each_anchor()` covering this view row, blit the tiles in this row with the existing `blit_placeholder_image()`, clipped to the grid width by the row's clip region.
- `render_frame()`: compute a hash of the visible placements' `(handle, row, col)` once per frame; if it differs from `r->last_images_hash`, force a full redraw. This covers an anchor being erased, scrolled or reflowed while its lower rows are not dirty.
- `ponytail:` note: the anchor scan reads up to 254 rows above the view per frame; add an anchor index if it shows in profiles.

### [MODIFY] `src/render/render.h`

- `struct renderer`: add `uint64_t last_images_hash`.

### [MODIFY] `src/term/sixel.c`

- Header comment: erase follows the anchor scheme, not per-tile cells.

### [MODIFY] `test/term/test_graphics.c`

- `test_nonvirtual_placement`, `_via_transmit`, `test_anonymous_*`, `test_cursor_stays_with_c1`, `test_placement_ids_are_per_image`: assert the anchor cell at top-left and no `ATTR_IMAGE` on the other covered cells; cursor assertions unchanged.
- `test_delete_erases_nonvirtual_cells`, `test_delete_by_position`: assert the anchor is cleared; delete-by-position hits a non-anchor cell inside the image area and still deletes it.
- [NEW] `test_each_anchor`: a placement anchored above the queried rows is reported; one fully above is not.

### [MODIFY] `test/term/test_grid.c`

- `test_resize_clip()` image case: replace with a wrapped text run whose first row holds an anchor; shrink then widen rejoins the text and the anchor stays at its original column on the first row.
- `test_fastfetch_kitty_regression`: unchanged (it checks the anchor at `0,0`).
- `test_fastfetch_sixel_regression` round-trip: unchanged.

### [MODIFY] `important/index.md`, `important/critical-knowledge.md`, `local/system_architecture.md`

- Index entries for `grid`, `graphics`, `render`, tests.
- Critical knowledge: image area redraw depends on the per-frame placement hash; anchor erase removes the whole image.
- Architecture: replace the image-clip note with the anchor/overlay model.

## Behavior changes

- Text printed inside an image's area is drawn under the image instead of erasing that tile.
- Partial erase inside an image's area no longer punches holes in it; only erasing the anchor or `a=d` removes it.
- A narrow-printed fastfetch widened afterwards rejoins its box lines; the box then sits beside a taller image, as in kitty.

## Verification

- `./build.sh` (build plus automated suite).
- Manual: `fastfetch` narrow → widen; wide → narrow → wide; `clear` removes the image; scrollback view shows the image partially scrolled.
