# `astralia-term` developing conventions

## Language

- C11 only (`c_std=c11`), compiled with `-D_GNU_SOURCE`.
- No C++, no GPU/OpenGL path; all rendering is CPU-side through `pixman`.
- No AVX or anything newer than SSE4.2; release builds use `-march=westmere`.

## Commenting

- Comments are sparse and use `/* */`, never `//`.
- Allowed:
  - A one-line contract above a public declaration in a header, when the name alone doesn't say it (e.g. `/* Return false to stop the loop */`).
  - Comments that group constants or tables (e.g. `/* DEC special graphics, 0x5f..0x7e */`).
  - Section dividers in headers (e.g. `/* ---- internals shared with vt_csi.c ---- */`).
  - A short "why" where the code would otherwise look wrong.
  - License/attribution notices for code taken from foot.
- No comments narrating what the next line does, and no mentions of past fixes.

## Formatting

- Command: `clang-format -i <filename>.c`.
- Style taken from `.clang-format` (LLVM base, 4-space indent, no column limit); its output is authoritative, including where it joins or keeps return-type line breaks.
- Run it on every `.c`/`.h` file you touch, except the generated `src/core/width_table.h`.

## File structure

- `src/` is grouped by subsystem into `core/`, `vt/`, `term/`, `input/`, `render/`, `backend/`; `test/` mirrors the groups that have standalone tests (`test/vt/`, `test/term/`, `test/input/`). `main.c` alone stays at `src/` root.
- One `name.h`+`name.c` pair per subsystem, within its group directory; a pair may be split into extra `.c` files sharing one header (e.g. `term/vt_csi.c` uses `term/term.h`'s internals section).
- Anything the tests need lives in `core_sources` (`astralia-core`), which must stay free of display, font, and pty dependencies.
- Backends talk to the rest of the program only through `backend/backend.h`; nothing outside `backend/*.c` includes Wayland or `xcb` headers.
- `main.c` wires subsystems together and owns no terminal, rendering, or protocol logic.
- New Wayland protocols are added to the `have_wayland` block in `meson.build`, never checked in as generated code.

## Includes

- Local headers are prefixed with their group (`#include "term/term.h"`, `#include "core/util.h"`), resolved through a single `include_directories('src')`; tests get the same include dir.
- A `.c` file includes its own header first, then one blank line.
- Then system headers (`<header>`) in one sorted block, then one blank line.
- Then `"config-build.h"` when needed, then `#define LOG_MODULE "<name>"` immediately before the local headers, ending with `"core/util.h"`.

## Logging and allocation

- Every `.c` file that logs defines its own `LOG_MODULE` before including `util.h`.
- Use `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup`, which abort on failure, instead of raw allocation plus a `NULL` check.
- Use `MIN`/`MAX`/`CLAMP`/`ARRAY_LEN` from `util.h` rather than redefining them.

## Dependencies

- Allowed: `pixman`, `fcft`, `xkbcommon`, `libwayland-client`, `wayland-protocols`, `xcb` family, `zlib`, `libspng`.
- Ruled out: `GLFW`, `SDL`, `GTK`, `Xlib`, `OpenGL`, and anything requiring AVX.
- Code copied or adapted from foot keeps its MIT copyright notice.

## Testing

- Plain `./build.sh` builds, then runs the automated suite (`test_parser`, `test_grid`, `test_osc`, `test_input`) via `meson test`.
- `./build.sh test` does the same, then launches `build/astralia-term` for a manual check; `./build.sh run` just launches it, without testing.
- Sanitizer build: `meson setup build -Db_sanitize=address,undefined`.
