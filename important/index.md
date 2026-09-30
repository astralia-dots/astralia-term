# `astralia-term` index

## Rule

- One-line, no break.
- Grouped by `directory`, one `##` heading per directory.
- Entry format: `file`: Purpose (≤ 20 words).
- Reflect current structure and function of each file in the code base.
- No mentions of past fixes.

## src

- `main.c`: Arguments, backend selection, pty/term/renderer wiring, `term_host` callbacks, scrollback/font-size keys, mouse reporting/wheel-scroll/1007 alt-scroll, click-counted selection, clipboard, link hover (hand cursor) and Ctrl+click `xdg-open` in any mouse mode, scale-driven font reload, visual bell, blink and 2026 timers. Stays at `src/` root: wires every subsystem below, owns none of them.

## src/core

- `util.h`+`.c`: `xmalloc` family, `MIN`/`MAX`/`CLAMP`/`ARRAY_LEN`, `likely`/`unlikely`, `LOG_MODULE` logging, table-driven `cp_width()`.
- `loop.h`+`.c`: Single-threaded `epoll` fd/timer/signal/hook registry; `signalfd` signals, `timerfd` timers, `HIGH`/`NORMAL`/`LOW` hooks. Port of foot `fdm.c`.
- `pty.h`+`.c`: `posix_openpt` pty spawn of `argv`/`$SHELL`/`/bin/sh`, `TIOCSWINSZ` resize, buffered non-blocking `pty_write` waiting on `EPOLLOUT`.
- `base64.h`+`.c`: Base64 decode and encode for OSC 52 and, later, kitty graphics payloads.
- `width_table.h`: Generated two-level codepoint width table (0/1/2); do not edit.

## src/vt

- `vt_parser.h`+`.c`: Paul Williams DEC state machine with UTF-8 ground fast path, `:` sub-parameters, `vt_callbacks` dispatch; OSC reports its terminator; DCS captures its `Ps;...` header (private marker, params, final byte) then buffers the body like OSC/APC.

## src/term

- `term.h`+`.c`: Terminal state, `term_host` callbacks, C0/ESC handlers, combining attach, scrollback view offset, reflowing `term_resize`, `term_reset`, mouse tracking mode/SGR/alt-scroll flags, `struct selection` ownership.
- `vt_csi.c`: CSI handlers: cursor, erase, insert/delete, `SGR`, `DECSTBM`, `DECSET` (incl. `1000`-`1003`/`1004`/`1006`/`1007`/`2004`/`2026`), `DECRQM`, `XTWINOPS`, DA/DSR, kitty keyboard protocol push/pop/set/query. Shares `term.h`'s internals section.
- `vt_osc.c`: OSC handlers: title, palette and fg/bg/cursor color set/query/reset, OSC 8 hyperlinks, OSC 52 clipboard writes (reads denied).
- `hyperlink.h`+`.c`: Hash-deduplicated `(id, uri)` table for OSC 8; cells store a 16-bit handle in `tile_row`/`tile_col` behind `ATTR_LINK`.
- `url.h`+`.c`: Link under a cell (OSC 8 run or wrapped plain-text URL, none on the cursor's typing line), per-row link mask for the always-on underline, hover range.
- `grid.h`+`.c`: 20-byte `struct cell` (`ATTR_IMAGE` cells carry a placement ID in `ul` and `tile_row`/`tile_col`), tagged colors, lazy rows, scrollback ring with `dirty` flags, reflowing resize tracking `grid_point`s, per-screen kitty keyboard protocol flags stack.
- `selection.h`+`.c`: Character/word/line selection: click/word/line boundary classification, drag extension, row-span query, UTF-8 extraction, scroll and reflow-resize adjustment, clearing when selected rows are written or erased.
- `composed.h`+`.c`: Hash-deduplicated table of base plus combining codepoint chains; cells store `COMPOSED_BASE + index`.
- `kitty_placeholder.h`+`.c`: Unicode-placeholder diacritic decode and left-neighbor inheritance for virtual placements, factored out of `render.c` for standalone testing.
- `graphics.h`+`.c`: Kitty graphics protocol APC transport: control-data parsing, `m=`-chunk reassembly, `direct`/`file` transmission, formats 24/32/100 (`libspng`), `o=z` (`zlib`), a 64 MB LRU image store, `a=t`/`T`/`q`/`p`/`d`; virtual and non-virtual placements (the latter written into the grid as `ATTR_IMAGE` cells, cursor-advanced like printed text); `graphics_store_insert()` for internally-decoded (sixel) images; `id=0` (anonymous, no `i=` key) is processed normally but never replied to.
- `sixel.h`+`.c`: Sixel DCS body decoder (raster attributes, RGB/HLS color registers, repeat counts, band control) into a straight-alpha RGBA buffer; adapted from foot's algorithm, simplified to buffer-in/pixmap-out.

## src/input

- `input.h`+`.c`: Legacy xterm and kitty keyboard protocol (`CSI ... u`) encoding of `struct key_event`; SGR/X10 mouse and wheel encoding, into pty bytes.
- `kitty_keys.h`: Kitty keyboard protocol key code and legacy-final table (`kitty_keymap[]`), adapted from foot; must stay sorted on `sym`.

## src/render

- `render.h`+`.c`: Draws dirty or scrolled-back rows, composed glyphs, selection highlight, blinking cursor, always-on link underline with accent (`#9B57F4`) hover highlight, visual-bell flash into premultiplied `pixman` buffers; translucent default background; damage.
- `boxdraw.h`+`.c`: Procedural `U+2500..U+259F`: line, dash and block rectangles, plus supersampled coverage masks for arcs and diagonals; no `pixman` dependency.
- `font.h`+`.c`: `fcft` wrapper loading a `fontconfig` pattern per style at a given dpi; glyph and grapheme lookup, cell metrics, atomic reload, pattern size substitution.

## src/backend

- `backend.h`: Backend vtable (`backend_ops`), `backend_listener` events (configure, scale, key, pointer motion/button, scroll, paste, selection_lost), `struct buffer`, `struct key_event`, `enum key_mods`/`key_action`/`selection_target`/`cursor_shape`; includes `config-build.h`.
- `wayland.c`: `xdg_toplevel` window, up to three `ARGB8888` `wl_shm` buffers, `fractional-scale` plus `viewporter` HiDPI, frame callbacks, `xkbcommon` keyboard with client-side repeat and press/repeat/release reporting, `wl_pointer` motion/button/frame-reconciled axis (v8 `axis_value120`), `cursor-shape` (text/pointer), `xdg-decoration`, `wl_data_device`/`zwp_primary_selection_device_v1` clipboard.
- `x11.c`: `xcb` window on a 32-bit ARGB visual when available, `Xft.dpi` scale, MIT-SHM `put_image`, `xcb-xkb` keyboard with press/release and autorepeat collapsing, button/motion/wheel pointer events, text/pointer cursors, `ConfigureNotify` resize, 60 Hz frame throttle, `CLIPBOARD`/`PRIMARY` ownership and paste with `INCR` for large transfers.

## test/vt

- `test_parser.c`: Byte streams fed through `vt_parser`: states, UTF-8 decoding, parameters, sub-parameters, string payloads.

## test/term

- `test_grid.c`: `cp_width()`, then `term` byte streams: wrapping, scrolling, erase, alt screen, replies, reflow, combining, `DECRQM`/`XTWINOPS`, mouse tracking modes, scrollback view, selection click/word/line/extraction/scroll/resize/cancellation; regressions feeding `testdata/fastfetch-sixel-regression.raw` and `testdata/fastfetch-kitty-regression.raw`.
- `test_osc.c`: OSC title, palette and dynamic color set/query/reset with both terminators, OSC 8 links (dedup, ids, scroll, erase, limits, reset), OSC 52 decode, base64.
- `test_url.c`: OSC 8 runs across SGR reset and erase; plain-text URL detection across wraps, paren/punctuation trimming, typing-line exclusion, per-row link mask, hover.
- `test_graphics.c`: Kitty graphics APC control-data parsing, chunk reassembly, raw/`zlib`/PNG payloads, quiet modes, query, delete, quota eviction, virtual and non-virtual placement creation/cursor-advance/default-sizing/`a=d` erase, anonymous (`id=0`) transmit/placement with no reply.
- `test_kitty_placeholder.c`: Diacritic table round-trip, left-neighbor inheritance rule, id extraction.
- `test_sixel.c`: Sixel decoder correctness: raster sizing, RGB/HLS color registers, repeat counts, band transitions, a pixel-for-pixel reference image.

## test/render

- `test_boxdraw.c`: Line, double-corner, dash seam, block fraction, arc and diagonal geometry, and lookup range for `boxdraw`.

## test/input

- `test_input.c`: `kitty_keymap[]` sortedness, legacy key encoding, kitty keyboard protocol push/pop/set/query and `CSI ... u` encoding, SGR/X10 mouse and wheel encoding, arrow fallback.

## root

- `meson.build`: Build config, `astralia-core` static library for tests, backend feature detection, `fcft` fallback, `wayland-scanner` codegen for `xdg-shell`, `xdg-decoration`, `cursor-shape` (and its `tablet-v2` dependency), `primary-selection`, `fractional-scale`, `viewporter`, test registration, `install_data()` for `misc/astralia-term.desktop` into `{datadir}/applications`.
- `meson_options.txt`: `wayland`/`x11` feature options and the `westmere` release-build flag.
- `build.sh`: `setup`/`build`/`install`/`run`/`test`/`uninstall` commands; no arguments builds and runs the automated suite; `test` does that and then launches `build/astralia-term` for manual checks; `run` just launches it, uninstalled; `install` (`sudo ninja -C build install`) deploys the binary to `{prefix}/bin` and the desktop entry to `{prefix}/share/applications`, then refreshes the desktop database (skipped if `update-desktop-database` isn't installed); `uninstall` does the same refresh after removing the files; jobs capped via `ASTRALIA_TERM_BUILD_JOBS`.
- `.clang-format`: Formatting style, 4-space indent, no column limit.
- `.clangd`: `clangd` configuration.
- `CLAUDE.md`: Project instructions for Claude Code.
- `LICENSE`: Project license.

## misc

- `astralia-term.desktop`: Freedesktop desktop entry (`Exec=astralia-term`, `Icon=utilities-terminal`, generic icon name since the project ships no icon asset of its own), installed by `meson.build`'s `install_data()`.

## important

- `index.md`: Index of every source, test, and build file.
- `convention.md`: Commenting, formatting, file-structure, include, logging, and dependency rules.
- `critical-knowledge.md`: Hard-won development rules, one statement plus one explanation each.

## local

- `local/`: Planning/design docs, not part of the shipped repo.
- `system_architecture.md`: Data flow, event loop, frame pacing, rendering, and terminal-model design decisions.
- `request.md`: Request template for the next task.
- `plan/plan.md`: The single plan: goals, stack, milestones 1–5 with status, per-file changes for upcoming steps, risks.
