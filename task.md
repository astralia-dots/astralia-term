# Task: images as anchored overlays

- [x] `graphics.h`/`graphics.c`: anchor-only placement, `graphics_each_anchor()`, delete paths (no `grid_mark_all_dirty()` in `blank_cells_for_handle()`: the render hash covers it)
- [x] `grid.h`/`grid.c`: drop image clip rule, comments
- [x] `render.h`/`render.c`: `draw_images()`, placement hash full redraw
- [x] `sixel.c`: comment
- [x] `test_graphics.c`, `test_grid.c`: updated and new tests
- [x] `index.md`, `critical-knowledge.md`, `local/system_architecture.md`
- [x] `./build.sh` passes
