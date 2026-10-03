# `astralia-term` development knowledge

## Description

Hard-won rules from astralia-term's development, plus the ones carried over from astralia-shell that apply here.
Can be updated if found new knowledge that supersedes old ones, or genuinely new ones.

## Rule

One statement + One explanation, ≤ 20 words each.
Drop an entry once newer knowledge fully supersedes it.

## 1. Event loop

- **The `epoll` loop must never block, even briefly.** One loop drives the pty, the display, and timers; a blocking call freezes the whole terminal.
- **A `timerfd` callback must call `loop_timer_ack()`.** Otherwise the fd stays readable and the loop spins on it forever.
- **Hooks run `HIGH` → `NORMAL` → `LOW` before every `epoll_wait`; a new backend must dispatch in `HIGH` and flush in `LOW`.** Otherwise already-queued display events stall until an unrelated fd wakes the loop.
- **`loop_del()` is safe inside a callback; removed handlers are freed after the dispatch pass.** Don't add extra deferral around it.
- **The main loop must `continue` on `EINTR`, never break.** Breaking silently exits the whole process, masquerading as an unexplained crash.

## 2. Processes and the pty

- **Never allocate memory in a forked child before `exec()`.** `fork()` can copy a lock held by another thread, deadlocking the child forever.
- **Ignoring `SIGCHLD` and calling `waitpid()` cannot coexist.** Ignoring it lets the kernel auto-reap, breaking exit-code propagation; `SIGCHLD` goes through `signalfd`.
- **`pty_write` must buffer and wait for `EPOLLOUT` when the pty is full.** A blocking write deadlocks when the child is itself blocked writing output back.
- **Kill test instances with `pkill -x astralia-term`, not `pkill -f`.** `pkill -f` also matches the shell that launched it.
- **`child_exec()` must `scrub_host_env()` prefix-matched terminal/editor self-identification vars (`TERM_PROGRAM`, `NVIM`, `KITTY_`, ...).** Otherwise they leak from whatever launched `astralia-term` into every spawned shell, breaking shell logic that checks them.

## 3. Frame pacing and rendering

- **Draw only when `frame_ready && render_needed()`.** That caps output at one frame per display refresh regardless of pty throughput.
- **Wayland `frame_ready` must also be set on `xdg_surface.configure` when no frame callback is pending.** Otherwise the resized frame after an ack never draws.
- **X11 has no frame callback, so `commit` arms a 60 Hz `timerfd`.** MIT-SHM completion events are what mark buffers free again.
- **`get_buffer()` returning `NULL` just skips the frame.** All buffers busy is normal; the next wakeup retries.
- **Backends may free buffers only on a size change.** `render_frame()` copies the previous frame into a switched buffer and relies on it still existing.
- **A pending `wl_callback` must be released with `wl_callback_destroy`, never just nulled.** `wl_surface_destroy` doesn't free it; a late `done` then fires against reset state.
- **Optional protocol events need sane fallback defaults, not zero.** Some compositors never send `repeat_info`; `0/0` silently disables key repeat.
- **On Wayland, never attach a buffer before the first `xdg_surface.configure` is acked.** Compositors kill the client with "xdg_surface has never been configured".
- **While mode `2026` is set, `render_needed()` returns false; a 150 ms timer ends it.** A client that never resets it would otherwise freeze the display.
- **Only the default background is translucent, and fills are premultiplied.** Explicit backgrounds stay opaque; unpremultiplied colors glow on `a8r8g8b8` buffers.
- **Draw `U+2500..U+259F` procedurally via `boxdraw`, never from font glyphs.** Font glyphs stop short of the cell edge, so stacked rows show gaps.

## 4. Terminal model

- **An all-zero `struct cell` is blank with default colors.** Rows can be `calloc`ed lazily, so never give "blank" a nonzero encoding.
- **`struct cell` must stay 20 bytes.** Grid memory and scroll cost scale with it; reuse fields behind an attr bit (e.g. `ATTR_LINK` handle in `tile_row`/`tile_col`).
- **`cp_width()` reads the generated `width_table.h`, never libc `wcwidth()`.** Widths must not depend on the user's locale or glibc's Unicode version.
- **Bold palette colors 0–7 draw bright, and colors resolve at draw time.** Resolving at write time would break `DECSCNM` and palette changes.
- **A cell `cp` at or above `COMPOSED_BASE` is a `composed_table` index, not a codepoint.** Anything reading text must resolve it through `composed_get()` first.
- **Only the normal screen reflows on resize; the alternate screen truncates.** Full-screen apps redraw on `SIGWINCH`, and reflowing their layout would corrupt it.
- **Scrolls that push lines into history must bump `view_offset` when scrolled back.** Otherwise the viewed text slides away while output keeps arriving.
- **OSC replies must end with the request's terminator, BEL or ST.** Some applications only parse the terminator they sent.
- **A kitty graphics command with no image id (`id=0`) must never get a reply, success or error.** The client isn't reading one; an unsolicited reply leaks onto the pty and gets typed into the next shell prompt.

- **The `XTVERSION` reply name must contain `kitty`.** `snacks.nvim` enables image support only by matching that name, else opens images as binary.

- **The open OSC 8 link lives in `term.link`, never in `struct pen`.** `SGR 0` resets the whole pen and would silently close the link.
- **A link handle in `tile_row`/`tile_col` is valid only with `ATTR_LINK` and without `ATTR_IMAGE`.** Image cells reuse those bytes; read them through `cell_link()`.
- **Every visual-bell frame and the one after it must be a full redraw.** The overlay is baked into the buffer, and the next frame copies it.
- **Backend `configure` sizes and pointer coordinates are buffer pixels, not logical.** Wayland multiplies logical values by the fractional scale.
- **Rescale `app->width/height` before reloading fonts on a scale change.** Otherwise the grid is reflowed to the new cell size and back, and the alternate screen loses content.

## 5. Keyboard input

- **`kitty_keymap[]` must stay sorted on `sym`, with no exceptions.** A single out-of-order entry (e.g. `Delete` = `0xffff`) silently breaks `bsearch()`/binary search for unrelated keys elsewhere in the table.
- **`ev->utf8` has Ctrl's transformation applied; the keysym doesn't.** Deriving a kitty CSI-u code point from `utf8` instead of `xkb_keysym_to_utf32(ev->sym)` corrupts it for every Ctrl combo.
- **X11 has no detectable-autorepeat extension enabled, so a held key sends release+press pairs.** Collapse them into one `KEY_REPEAT` by peeking the next queued event's timestamp, not by adding the XKB extension.

## 6. Mouse input

- **xterm's wire button numbering is 0-based (left=0); `input_mouse_encode()`'s API is 1-based.** Encoding `button` directly instead of `button - 1` reports the wrong button to every mouse-aware app.
- **`wl_pointer.axis`/`axis_discrete`/`axis_value120` must be reconciled once per `frame()`, not dispatched individually.** A compositor sends only one of the three per scroll; emitting on each event double- or under-counts notches.
- **`axis_value120` needs the `wl_seat` bind capped at v8, not v7.** Sub-objects inherit the parent's bound version; a lower cap silently falls back to deprecated `axis_discrete`.

## 7. Selection and clipboard

- **`grid_resize()`'s `clamp_points()` clamps every tracked point into `[0, rows-1]`.** A selection endpoint threaded through resize like the cursor loses its scrollback-range row if reflow pushes it above the visible window.
- **Wayland's `wl_data_device_manager` is bound at v1, not the newest version.** v3+ sends drag-and-drop action-negotiation events (`source_actions`/`action`) on offers/sources this backend never implements.
- **A hex escape in a C string literal consumes every following hex-digit character.** `"\x1bc"` is one out-of-range escape (`0x1bc`), not ESC+`c`; split it: `"\x1b" "c"`.

## 8. Build

- **The "Pkg-config error with 'fcft'" configure warning is expected.** `fcft.pc` requires header-only `tllist`; `meson.build` falls back to linking `libfcft` directly.
- **`HAVE_WAYLAND`/`HAVE_X11` come from generated `config-build.h`, which `backend.h` includes.** Test with `#if`, not `#ifdef`, since both are always defined as `0` or `1`.
- **`cursor-shape-v1.xml` needs `tablet-v2.xml` generated alongside it.** It references `tablet-v2` types, so dropping it breaks the link.
- **When a dependency is missing, stop and tell the user.** `vttest` is not installed on the dev machine; `cage` is.
- **Test Wayland with `cage -- build/astralia-term`, and capture with `import -window root`.** Cage's X11 window is hard to find by name; cage shows black behind translucency.
- **Glyph alpha of light-on-dark text is remapped in `blit_glyph`, not blended straight.** A gamma table (`ASTRALIA_GLYPH_GAMMA`, default `1.4`) thickens the strokes; color glyphs and `boxdraw` masks skip it.
- **Never `pkill astralia-term` (by name or `-f`) to clean up test instances.** It kills every instance, including the one the user is working in. Launch tests with `&`, keep `$!`, and kill only that PID.
- **`yazi` sends `a=T,U=1,t=s` with no `c=`/`r=`.** A virtual placement without a size must default from the image and cell size, or its preview is blank.
- **A `Delete`/`BackSpace` keysym maps to a control code point in `xkb_keysym_to_utf32`.** Kitty alternate-key reporting must skip functional keys, or `Delete` is sent as `CSI 3:127~` and `yazi` drops it.
- **A `static_library()`'s consumers need every dependency its linked-in objects reference, not just the ones the consumer calls directly.** `term.c` calling into `graphics.c` means every test that calls `term_init` (`test_grid`, `test_osc`, `test_input`), not only `test_graphics`, must also list `zlib`/`libspng` in its own `dependencies:`.
- **A non-virtual image lives in one `ATTR_IMAGE` anchor cell; erasing that cell removes the whole image.** Tile cells tore images on reflow; the anchor moves with text instead.
- **`render_frame()` redraws in full whenever the visible placement hash changes.** Image pixels span rows the anchor's row doesn't dirty, so they'd otherwise go stale.
