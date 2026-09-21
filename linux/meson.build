project('picoview', ['c', 'cpp'],
  version : '1.3.0',
  license : 'MIT',
  default_options : [
    'cpp_std=c++17',
    'c_std=c11',
    'optimization=2',
    'warning_level=2',
    'b_ndebug=if-release',
  ])

cc = meson.get_compiler('cpp')

wl_client = dependency('wayland-client')
wl_protos = dependency('wayland-protocols', version : '>=1.25')
xkb       = dependency('xkbcommon')
# Text: fontconfig finds a font file, FreeType turns characters into pixels.
freetype  = dependency('freetype2')
fontcfg   = dependency('fontconfig')
# Video and sound. Writing a player on ffmpeg directly means writing demuxing,
# timing and audio sync; libmpv is that program, as a library.
mpv       = dependency('mpv')
threads   = dependency('threads')
m_dep     = cc.find_library('m', required : false)

# The protocol headers and glue are generated, not vendored: the versions that
# matter are whatever this machine's wayland-protocols ships, and pinning a
# copy would only go stale.
wl_scanner = find_program('wayland-scanner')
protodir   = wl_protos.get_variable(pkgconfig : 'pkgdatadir')

protocols = [
  protodir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
  protodir / 'unstable' / 'xdg-decoration' / 'xdg-decoration-unstable-v1.xml',
]

gen_src = generator(wl_scanner,
  output    : '@BASENAME@-protocol.c',
  arguments : ['private-code', '@INPUT@', '@OUTPUT@'])
gen_hdr = generator(wl_scanner,
  output    : '@BASENAME@-client-protocol.h',
  arguments : ['client-header', '@INPUT@', '@OUTPUT@'])

proto_src = []
proto_hdr = []
foreach p : protocols
  proto_src += gen_src.process(p)
  proto_hdr += gen_hdr.process(p)
endforeach

sources = [
  'picoview.cpp',
  'paint.cpp',
  'text.cpp',
  'video.cpp',
  '../src/core/coverplan.cpp',
  '../src/core/loudness_math.cpp',
]

executable('picoview',
  sources + proto_src + proto_hdr,
  dependencies : [wl_client, xkb, freetype, fontcfg, mpv, threads, m_dep],
  install : true)

install_data('picoview.desktop', install_dir : get_option('datadir') / 'applications')
install_data('picoview.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
