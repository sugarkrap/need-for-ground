project('nfsu2-native', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ])

cc = meson.get_compiler('c')
fs = import('fs')

# --- Wine headers -------------------------------------------------------
# Declaration source only: no libwine, no wine loader, no winegcc. See
# include/nfsu2/win32_compat.h for why this works and what the calling
# conventions end up being.
wine_inc_dir = get_option('wine_headers')
if not fs.is_dir(wine_inc_dir / 'windows')
  error('Wine headers not found at @0@/windows. Install them (Arch: wine, Debian: '
        .format(wine_inc_dir) + 'wine-dev / libwine-dev) or pass -Dwine_headers=<dir>.')
endif
wine_inc = include_directories(wine_inc_dir / 'windows', is_system : true)

nfsu2_inc = include_directories('include')

target_bits = cc.sizeof('void*') * 8
message('target pointer width: @0@ bits'.format(target_bits))
if target_bits != 32
  warning('This is a @0@-bit build. Game code is 32-bit; only the shim, the '
          .format(target_bits) +
          'ABI probe and the DXVK smoke test are meaningful here. Use '
          + '--cross-file cross/linux32.txt for the real target.')
endif

# --- Win32 shim ---------------------------------------------------------
# One static library for every DLL we implement that needs no SDL. Sources are
# grouped into a directory per emulated DLL - tools/win32_coverage.py reports
# coverage per directory, so the layout is also the bookkeeping.
win32_sources = files(
  'src/advapi32/registry.c',
  'src/gdi32/gdi.c',
  'src/shell32/shell.c',
  'src/tapi32/tapi.c',
  'src/win32/comm.c',
  'src/win32/core.c',
  'src/win32/exception.c',
  'src/win32/file.c',
  'src/win32/heap.c',
  'src/win32/interlocked.c',
  'src/win32/locale.c',
  'src/win32/mapping.c',
  'src/win32/memory.c',
  'src/win32/module.c',
  'src/win32/path.c',
  'src/win32/process.c',
  'src/win32/resource.c',
  'src/win32/sync.c',
  'src/win32/thread.c',
  'src/win32/time.c',
  'src/win32/toolhelp.c',
  'src/win32/volume.c',
  'src/winmm/mmtimer.c',
  'src/ws2_32/posix_net.c',
  'src/ws2_32/winsock.c',
)

thread_dep = dependency('threads')
dl_dep = cc.find_library('dl', required : false)

libnfsu2_win32 = static_library('nfsu2_win32',
  win32_sources,
  include_directories : [nfsu2_inc, wine_inc],
  dependencies : [thread_dep, dl_dep],
  c_args : ['-D_GNU_SOURCE'],
  install : false)

# link_whole, not link_with: every entry point has to reach the final binary even
# when nothing references it, because GetProcAddress can ask for any of them.
# win32/interlocked.c is the clearest case - the header's inline versions satisfy
# every call site, so the archive member would otherwise never be pulled in.
nfsu2_win32_dep = declare_dependency(
  link_whole : libnfsu2_win32,
  include_directories : [nfsu2_inc, wine_inc],
  dependencies : [thread_dep, dl_dep])

# --- user32 shim (SDL2-backed) ------------------------------------------
# Kept separate from the core shim so that libnfsu2_win32 stays SDL-free and
# usable on a machine without 32-bit SDL2 installed.
sdl2 = dependency('sdl2', required : false)

if sdl2.found()
  libnfsu2_user32 = static_library('nfsu2_user32',
    files(
      'src/user32/input.c',
      'src/user32/keymap.c',
      'src/user32/message.c',
      'src/user32/window.c',
    ),
    include_directories : [nfsu2_inc, wine_inc],
    dependencies : [sdl2, thread_dep],
    c_args : ['-D_GNU_SOURCE'],
    install : false)

  nfsu2_user32_dep = declare_dependency(
    link_whole : libnfsu2_user32,
    dependencies : [nfsu2_win32_dep, sdl2])

  # --- dinput8 + ddraw (SDL2-backed) ------------------------------------
  # Separate from user32 because user32 must not depend on it: the message pump
  # reaches dinput8 through a weak symbol, so a build without this library still
  # links and simply has no buffered DirectInput.
  libnfsu2_dinput8 = static_library('nfsu2_dinput8',
    files(
      'src/ddraw/ddraw.c',
      'src/dinput8/data_formats.c',
      'src/dinput8/device.c',
      'src/dinput8/dik_map.c',
      'src/dinput8/dinput8.c',
    ),
    include_directories : [nfsu2_inc, wine_inc],
    dependencies : [sdl2],
    c_args : ['-D_GNU_SOURCE'],
    install : false)

  nfsu2_dinput8_dep = declare_dependency(
    link_whole : libnfsu2_dinput8,
    dependencies : [nfsu2_user32_dep])
endif

# --- shim selftest ------------------------------------------------------
shim_selftest = executable('nfsu2-shim-selftest',
  'tests/shim_selftest.c',
  dependencies : [nfsu2_win32_dep],
  c_args : ['-D_GNU_SOURCE'],
  # GetProcAddress resolves shim entry points through dlsym(RTLD_DEFAULT, ...)
  export_dynamic : true,
  install : false)
test('shim', shim_selftest, timeout : 60)

# --- services selftest --------------------------------------------------
# Registry, Winsock (over real loopback traffic), locale/codepages, gdi32.
services_selftest = executable('nfsu2-services-selftest',
  'tests/services_selftest.c',
  dependencies : [nfsu2_win32_dep],
  c_args : ['-D_GNU_SOURCE'],
  export_dynamic : true,
  install : false)
test('services', services_selftest, timeout : 120)

# --- D3D9 calling-convention selftest -----------------------------------
# Needs no DXVK and no GPU: guards d3d9_native.h against regressions.
d3d9_abi_selftest = executable('nfsu2-d3d9-abi-selftest',
  'tests/d3d9_abi_selftest.c',
  include_directories : [nfsu2_inc, wine_inc],
  c_args : ['-D_GNU_SOURCE'],
  install : false)
test('d3d9-abi', d3d9_abi_selftest, timeout : 60)

# --- user32 selftest ----------------------------------------------------
# Needs a display; the test itself exits 77 (SKIP) when there is none.
if sdl2.found()
  user32_selftest = executable('nfsu2-user32-selftest',
    'tests/user32_selftest.c',
    dependencies : [nfsu2_user32_dep],
    c_args : ['-D_GNU_SOURCE'],
    export_dynamic : true,
    install : false)
  test('user32', user32_selftest, timeout : 120)

  dinput8_selftest = executable('nfsu2-dinput8-selftest',
    'tests/dinput8_selftest.c',
    dependencies : [nfsu2_dinput8_dep],
    c_args : ['-D_GNU_SOURCE'],
    export_dynamic : true,
    install : false)
  test('dinput8', dinput8_selftest, timeout : 120)
endif

# --- DXVK Native --------------------------------------------------------
# Built out of tree by tools/build_dxvk_native.sh, which drops the .so into
# third_party/dxvk-native/lib<bits>/.
dxvk_lib_dir = get_option('dxvk_lib_dir')
if dxvk_lib_dir == ''
  dxvk_lib_dir = meson.current_source_dir() / 'third_party/dxvk-native/lib@0@'.format(target_bits)
endif

dxvk_d3d9 = cc.find_library('dxvk_d3d9',
  dirs : [dxvk_lib_dir],
  required : false)

if dxvk_d3d9.found() and sdl2.found()
  # Drives SDL directly: proves the ELF + DXVK + Vulkan path in isolation.
  executable('nfsu2-smoke-d3d9',
    'src/host/smoke_d3d9.c',
    dependencies : [nfsu2_win32_dep, dxvk_d3d9, sdl2],
    c_args : ['-D_GNU_SOURCE'],
    build_rpath : dxvk_lib_dir,
    export_dynamic : true,
    install : false)

  # Touches no SDL at all: window via our CreateWindowExA, HWND straight into
  # DXVK, PeekMessageA/DispatchMessageA loop - the shape the ported game has.
  executable('nfsu2-smoke-game-loop',
    'src/host/smoke_game_loop.c',
    dependencies : [nfsu2_user32_dep, dxvk_d3d9],
    c_args : ['-D_GNU_SOURCE'],
    build_rpath : dxvk_lib_dir,
    export_dynamic : true,
    install : false)
else
  if not dxvk_d3d9.found()
    message('libdxvk_d3d9 not found in @0@ - skipping the D3D9 smoke test.'.format(dxvk_lib_dir))
    message('  run: native/tools/build_dxvk_native.sh @0@'.format(target_bits))
  endif
  if not sdl2.found()
    message('sdl2 not found for this architecture - skipping the D3D9 smoke test.')
    if target_bits == 32
      message('  Arch Linux: pacman -S lib32-sdl2-compat')
    endif
  endif
endif

# --- ABI probe ----------------------------------------------------------
# Compares the D3D9 struct layout Wine's headers give us against the one DXVK
# Native was compiled with. Only buildable when DXVK sources are checked out.
dxvk_src = 'third_party/dxvk'
if fs.is_dir(meson.current_source_dir() / dxvk_src / 'include/native/windows') \
   and add_languages('cpp', required : false)
  abi_wine = executable('nfsu2-abi-wine',
    'tests/abi_probe_wine.c',
    include_directories : [nfsu2_inc, wine_inc],
    c_args : ['-D_GNU_SOURCE'],
    install : false)
  abi_dxvk = executable('nfsu2-abi-dxvk',
    'tests/abi_probe_dxvk.cpp',
    include_directories : include_directories(
      dxvk_src / 'include/native/windows',
      dxvk_src / 'include/native/directx',
      is_system : true),
    install : false)
  test('abi-layout-match',
    find_program('tools/abi_diff.py'),
    args : [abi_wine.full_path(), abi_dxvk.full_path()],
    depends : [abi_wine, abi_dxvk],
    timeout : 60)
else
  message('DXVK sources not checked out - skipping the header ABI probe.')
endif
