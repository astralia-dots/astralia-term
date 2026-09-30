project('astralia-term', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ])

cc = meson.get_compiler('c')
fs = import('fs')

add_project_arguments(
  '-D_GNU_SOURCE',
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-fstrict-aliasing',
  ]),
  language: 'c')

if get_option('buildtype') == 'release' and get_option('westmere')
  add_project_arguments('-march=westmere', '-mtune=westmere', language: 'c')
endif

pixman = dependency('pixman-1')
zlib = dependency('zlib')
libspng = dependency('spng')
# fcft.pc lists the header-only tllist in Requires.private, so the lookup
# fails when tllist isn't installed even though fcft.h doesn't need it.
fcft = dependency('fcft', version: '>=3.0', required: false)
if not fcft.found()
  fcft = declare_dependency(
    dependencies: [cc.find_library('fcft'), pixman])
  if not cc.has_header('fcft/fcft.h', dependencies: fcft)
    error('fcft not found')
  endif
endif

xkbcommon = dependency('xkbcommon')
math = cc.find_library('m', required: false)

wayland_client = dependency('wayland-client', required: get_option('wayland'))
wayland_protocols = dependency('wayland-protocols', required: get_option('wayland'))
wayland_scanner = dependency('wayland-scanner', native: true, required: get_option('wayland'))
have_wayland = wayland_client.found() and wayland_protocols.found() and wayland_scanner.found()

xcb = dependency('xcb', required: get_option('x11'))
xcb_shm = dependency('xcb-shm', required: get_option('x11'))
xcb_xkb = dependency('xcb-xkb', required: get_option('x11'))
xcb_cursor = dependency('xcb-cursor', required: get_option('x11'))
xkbcommon_x11 = dependency('xkbcommon-x11', required: get_option('x11'))
have_x11 = (xcb.found() and xcb_shm.found() and xcb_xkb.found() and xcb_cursor.found()
            and xkbcommon_x11.found())

if not have_wayland and not have_x11
  error('at least one of the Wayland or X11 backends must be enabled')
endif

conf = configuration_data()
conf.set10('HAVE_WAYLAND', have_wayland)
conf.set10('HAVE_X11', have_x11)
conf.set_quoted('ASTRALIA_VERSION', meson.project_version())
configure_file(output: 'config-build.h', configuration: conf)

core_sources = files(
  'src/core/util.c',
  'src/vt/vt_parser.c',
  'src/term/vt_csi.c',
  'src/term/term.c',
  'src/term/grid.c',
  'src/term/composed.c',
  'src/term/hyperlink.c',
  'src/term/vt_osc.c',
  'src/core/base64.c',
  'src/term/selection.c',
  'src/term/url.c',
  'src/term/graphics.c',
  'src/term/kitty_placeholder.c',
  'src/term/sixel.c',
  'src/render/boxdraw.c',
)

app_sources = files(
  'src/main.c',
  'src/core/loop.c',
  'src/core/pty.c',
  'src/render/font.c',
  'src/render/render.c',
  'src/input/input.c',
)

app_deps = [pixman, fcft, xkbcommon, math, zlib, libspng]
src_inc = include_directories('src')

if have_wayland
  wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')
  wayland_scanner_prog = find_program(
    wayland_scanner.get_variable('wayland_scanner'), native: true)

  wl_proto_xmls = [
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
    wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    # cursor-shape depends on tablet-v2 types
    wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
    wl_protocol_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
    wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
    wl_protocol_dir / 'stable/viewporter/viewporter.xml',
  ]

  wl_proto_sources = []
  foreach xml : wl_proto_xmls
    base = fs.stem(xml)
    wl_proto_sources += custom_target(base + '-client-header',
      input: xml, output: base + '.h',
      command: [wayland_scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'])
    wl_proto_sources += custom_target(base + '-code',
      input: xml, output: base + '.c',
      command: [wayland_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach

  app_sources += files('src/backend/wayland.c')
  app_sources += wl_proto_sources
  app_deps += [wayland_client]
endif

if have_x11
  app_sources += files('src/backend/x11.c')
  app_deps += [xcb, xcb_shm, xcb_xkb, xcb_cursor, xkbcommon_x11]
endif

core = static_library('astralia-core', core_sources, include_directories: src_inc)

executable('astralia-term', app_sources,
  link_with: core,
  dependencies: app_deps,
  include_directories: src_inc,
  install: true)

install_data('misc/astralia-term.desktop',
  install_dir: get_option('datadir') / 'applications')

# sixel.c (core_sources) uses libm; every executable linking core needs it
# too, since a static library doesn't resolve its own symbols at build time.
test('parser', executable('test_parser', 'test/vt/test_parser.c',
  link_with: core, dependencies: [math], include_directories: src_inc))
test('grid', executable('test_grid', 'test/term/test_grid.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc,
  c_args: ['-DFASTFETCH_REGRESSION_RAW="' +
           meson.current_source_dir() / 'test/term/testdata/fastfetch-sixel-regression.raw' + '"',
           '-DFASTFETCH_KITTY_REGRESSION_RAW="' +
           meson.current_source_dir() / 'test/term/testdata/fastfetch-kitty-regression.raw' + '"']))
test('osc', executable('test_osc', 'test/term/test_osc.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc))
test('url', executable('test_url', 'test/term/test_url.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc))
test('graphics', executable('test_graphics', 'test/term/test_graphics.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc))
test('kitty_placeholder', executable('test_kitty_placeholder', 'test/term/test_kitty_placeholder.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc))
test('sixel', executable('test_sixel', 'test/term/test_sixel.c',
  link_with: core, dependencies: [zlib, libspng, math], include_directories: src_inc))
test('boxdraw', executable('test_boxdraw', 'test/render/test_boxdraw.c',
  link_with: core, dependencies: [math], include_directories: src_inc))
# input.c pulls in backend.h (pixman + xkbcommon) for struct key_event, so it
# stays out of core_sources; this test links it in directly instead.
test('input', executable('test_input', ['test/input/test_input.c', 'src/input/input.c'],
  link_with: core, dependencies: [pixman, xkbcommon, zlib, libspng, math],
  include_directories: src_inc))
