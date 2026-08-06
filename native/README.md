# native/ - the Linux ELF port scaffolding

Target shape: a **native 32-bit Linux ELF** with no Wine loader involved at
runtime, using **Wine's headers** as the Win32/D3D9 declaration source and
**DXVK Native** as the D3D9 implementation (for now - the endgame in
`../DIRECTX_SCOPE.md` is our own Vulkan renderer behind the same interfaces).

Nothing here contains or requires game code to build. The pieces that exist
today are the host-side layers the ported game code will sit on, plus tests
that prove those layers behave.

## Why this shape and not winelib

Three options were on the table; the first two are ruled out on this machine
for concrete reasons, not preference:

| approach | what it produces | verdict |
| --- | --- | --- |
| winelib (`winegcc`, libwine) | ELF that needs the wine loader | **not possible here**: wine 11 is new-WoW64, there is no `i386-unix` tree, so `winegcc -m32` cannot link (`cannot find -lkernel32`) |
| PE build + wine at runtime | `speed2.exe` under wine | works today, but it is what we already do; not a port |
| **wine headers + native gcc + DXVK Native** | plain i386 ELF, `libdxvk_d3d9.so` linked in | **chosen** |

Wine's headers turn out to compile cleanly as-is with `gcc -m32` against glibc,
in the same translation unit as `<stdio.h>` and `<SDL2/SDL.h>`. That gives us
the full Win32 type universe (and the DX9 SDK struct layouts) for free, with no
vendored header fork to maintain.

## The one real hazard: calling conventions

Wine's headers on i386 define `WINAPI` as
`__attribute__((stdcall)) __attribute__((force_align_arg_pointer))`.
DXVK Native defines `STDMETHODCALLTYPE`, `__stdcall` and `WINAPI` as **empty**,
so on i386 every D3D9 entry point and vtable slot in `libdxvk_d3d9.so` is
**cdecl**. Compiling Wine's `d3d9.h` unmodified against DXVK would link fine
and then corrupt the stack by 4-8 bytes per call.

`include/nfsu2/d3d9_native.h` neutralises those two macros for the duration of
the d3d9 includes and restores them afterwards. Verified with `gcc -m32 -O1 -S`
on `dev->lpVtbl->SetViewport(dev, vp)`:

```
wine headers as-is     call *188(%edx) ; addl $20, %esp   <- callee popped 8: stdcall
via d3d9_native.h      call *188(%edx) ; addl $28, %esp   <- caller popped 8: cdecl
```

Everything *else* stays stdcall on purpose: the Win32 shim is defined `WINAPI`,
so a call site in ported game code has the same ABI the original MSVC code did.
That also keeps hybrid execution (not-yet-ported original machine code running
in-process) possible later.

## Layout

```
include/nfsu2/
  win32_compat.h     single include point for Win32 types; documents conventions
  d3d9_native.h      wine's D3D9 types at DXVK Native's ABI  <- the keystone
  ghidra_types.h     undefined4/byte/uint/... so decompiled output compiles
  win32_shim.h       shim control surface (init, path translation, tracing)
src/win32/           kernel32: files, paths, heap, vmem, mapping, threads, sync,
                     time, locale/codepages, volumes, toolhelp, SEH, resources
src/user32/          windows, message queue, SDL->WM_* translation, input
src/ws2_32/          Winsock over POSIX sockets (winsock.c + posix_net.c)
src/dinput8/         DirectInput 8 over SDL: keyboard, mouse, joysticks
src/advapi32/        registry, backed by registry.ini in the game root
src/gdi32/           DC/bitmap/font object model; nothing rasterised
src/shell32/         SHGetFolderPathA onto the XDG directories
src/tapi32/ src/ddraw/ src/winmm/  telephony, DirectDrawCreate, mm timers
src/host/            smoke_d3d9.c (SDL-driven) and smoke_game_loop.c (game-shaped)
tests/               shim, services, user32, dinput8, D3D9 convention; ABI probes
tools/               DXVK build script, coverage report, ABI diff
cross/linux32.txt    the real target: i386 ELF
third_party/         DXVK checkout and built .so (not committed)
```

## Build

```sh
# 1. DXVK Native's d3d9, as a plain .so (pinned to v3.0.2)
native/tools/build_dxvk_native.sh 32        # or 64

# 2. configure + build
meson setup native/build32 --cross-file native/cross/linux32.txt
ninja -C native/build32
meson test -C native/build32
```

Requirements beyond a normal toolchain (Arch package names):

- `wine` (headers only; no wine process is ever started)
- 32-bit runtime: `lib32-glibc`, `lib32-vulkan-icd-loader`, plus an ICD
  (`lib32-mesa` or `lib32-nvidia-utils`)
- `lib32-sdl2-compat` - **needed for the D3D9 smoke test and for DXVK's WSI**.
  Without it, meson still configures and the shim/ABI tests still build and
  run; only the D3D9 target is skipped, with a message saying so. (Verified
  both ways: the degraded configure path works, and with it installed the
  32-bit smoke test runs.)
- `meson`, `ninja`, `glslang` (DXVK build)

A 64-bit build (`meson setup native/build64`, no cross file) is supported for
toolchain validation only - game code is ILP32 and `ghidra_types.h` refuses to
compile at 64-bit rather than miscompile.

## What is verified today

Run `meson test -C native/build32` (and `build64`):

- **shim** - 35 checks over path translation, file I/O, enumeration, heap,
  virtual memory, timing, critical sections, TLS, events, threads.
- **user32** - 34 checks over class registration, window creation and geometry,
  window longs, the message queue (including filters and PM_REMOVE semantics),
  WNDPROC dispatch, PostQuitMessage, the ShowCursor *counter*, and SDL-event ->
  WM_* translation driven by synthetic SDL events (W, escape, arrows, F-keys,
  the key-up transition bit). Skips with meson's SKIP status on a headless
  machine rather than failing.
- **d3d9-abi** - D3D9 vtable dispatch through Wine's macros in the cdecl world:
  arguments arrive intact, seven-argument calls marshal, and the caller's stack
  is still balanced after 100k calls.
- **services** - 60 checks over the registry (including persistence across a
  close/reopen), Winsock driven by a real TCP and a real UDP conversation over
  loopback plus a non-blocking connect, `select()`, FIONREAD/FIONBIO,
  gethostbyname and the interface list; CP1252/UTF-8 conversion; and the gdi32
  object model.
- **dinput8** - 39 checks over device enumeration, the acquire/state contract,
  and both input paths: immediate `GetDeviceState` and the buffered
  `GetDeviceData` stream, fed by synthetic SDL events and checked against the
  DIK scancodes the game will look for.
- **abi-layout-match** - 47 struct sizes, field offsets and enum constants
  compared between Wine's headers and DXVK Native's own headers. All agree, at
  both 32- and 64-bit.

End-to-end at the real target width, on this machine:

```
$ ./build32/nfsu2-smoke-d3d9 --frames 300 --width 2560 --height 1080
info:  DXVK: 3.0.2
info:  Build: x86 gcc 16.1.1
info:  Found device: NVIDIA GeForce RTX 2060 SUPER (NVIDIA 610.43.3)
adapter    : NVIDIA GeForce RTX 2060 SUPER
driver     : nvd3dum.dll
vendor/dev : 10de:1f06
presented  : 300 frames at 2560x1080 (32-bit build)
```

That is an i386 native ELF, no wine loader, driving D3D9 -> DXVK -> Vulkan,
with the adapter identifier read back through the D3D9 vtable - i.e. the
convention decision in `d3d9_native.h` confirmed against the real library and
not just against our own test doubles. The 64-bit build does the same at
1280x720.

`nfsu2-smoke-game-loop` goes further and touches **no SDL at all** - it is the
shape the ported game will have:

```
$ ./build32/nfsu2-smoke-game-loop --frames 200 --width 2560 --height 1080
screen     : 2560x1080 (GetSystemMetrics)
wndproc    : WM_CREATE
client     : 2560x1080 (requested 2560x1080)
adapter    : NVIDIA GeForce RTX 2060 SUPER
wndproc    : WM_ACTIVATEAPP active=1
wndproc    : WM_KEYDOWN vk=0x20
presented  : 200 frames (32-bit build)
```

RegisterClassExA -> CreateWindowExA -> that HWND into
`D3DPRESENT_PARAMETERS.hDeviceWindow` -> PeekMessageA/DispatchMessageA loop.
It is the only test that can prove HWND-is-an-SDL_Window holds all the way
through DXVK's WSI when the window came from *our* CreateWindowExA, and that
real input reaches a stdcall WNDPROC.

## Win32 coverage

```sh
python3 native/tools/win32_coverage.py          # summary + grouped missing list
python3 native/tools/win32_coverage.py --done
```

It cross-references `../analysis/win32_imports.txt` (what the unwrapped exe
actually imports) against the entry points defined under `src/`, per module, so
"how much Win32 is left" is measured rather than guessed. Currently **250/250
(100%)**: 249 shimmed plus Direct3DCreate9 from DXVK.

100% means every import resolves, not that every import is *implemented* -
several are deliberate honest failures, and which is which is the useful
distinction:

| module | imports | what it is |
| --- | --- | --- |
| win32 (kernel32) | 159 | real |
| user32 | 35 | real, SDL-backed |
| ws2_32 | 22 | real, POSIX sockets |
| gdi32 | 12 | real object model, nothing rasterised |
| tapi32 | 9 | honest failure: no modem, and none wanted |
| advapi32 | 6 | real, file-backed |
| shell32 | 2 | real (XDG directories) |
| winmm | 2 | real (thread-per-timer) |
| dinput8 | 1 | real, SDL-backed |
| ddraw | 1 | honest failure: D3D9 is the renderer |

Run anything with `NFSU2_SHIM_TRACE=1` to see every stubbed call it makes.

## user32 design notes

- **HWND is an SDL_Window\***, not a handle of ours. The game passes the HWND
  from CreateWindowExA into `D3DPRESENT_PARAMETERS.hDeviceWindow`, and DXVK
  casts that straight to its backend window pointer, so anything else would
  crash inside DXVK. Per-window state hangs off `SDL_SetWindowData`, so there is
  no side table to keep in sync.
- **CreateWindowExA's size is the client size, and AdjustWindowRect is the
  identity.** SDL_CreateWindow also takes a client size, so the standard Win32
  idiom ("adjust a client rect, create with the result") lands on exactly the
  client size the caller wanted. `GetSystemMetrics` reports zero for every
  decoration metric to stay consistent with that.
- **Keys are mapped by scancode, not keycode** - i.e. by physical position. A
  2004 game hardcodes VK_W/VK_A/VK_S/VK_D meaning "the cluster on the left";
  mapping through keycodes would move those to Z/Q/S/D on the AZERTY layout
  this build shipped for. Text entry does not use that path at all - WM_CHAR
  comes from SDL_TEXTINPUT, which is layout- and dead-key-aware.
- **ShowCursor is a counter**, as on Windows, not a boolean. Game code hides on
  entering the 3D view and shows on leaving, and it relies on the pairing.
- **TranslateMessage is a no-op** returning TRUE for key messages: character
  composition already happened in the SDL pump, which is where the platform
  does it.
- **No GDI.** BeginPaint returns a fake HDC and a correct rcPaint so a WM_PAINT
  branch does not bail out or divide by zero; anything drawn through it is
  dropped. The renderer presents every frame, so nothing depends on it.

## Known gaps, in rough priority order

1. **Audio.** `mss32.dll` (Miles Sound System) is not in the import list at all -
   the game loads it dynamically - and nothing implements it. This is the largest
   remaining hole in the platform layer.
2. **SEH.** `RaiseException`/`RtlUnwind` abort with a diagnostic rather than
   unwinding. Real support means translating SIGSEGV into an `EXCEPTION_RECORD`,
   walking the handler chain off the TEB, and unwinding frames the compiler never
   emitted unwind data for. Worth doing only once the port runs.
3. **Force feedback.** `IDirectInputDevice8::CreateEffect` fails; SDL's haptic
   API could back it later.
4. **PE resources.** `FindResourceA` and friends fail: an ELF has no `.rsrc`.
   The path if one is ever needed is an extraction tool (pefile is already a
   dependency) plus a loader keyed on (type, name) - not built on speculation.
5. **`VirtualFree(MEM_RELEASE)`** leaks the mapping because reservation sizes
   are not tracked. Harmless for a process that exits; wrong for anything that
   cycles large allocations.
6. **Buffered DirectInput needs a message pump.** `GetDeviceData` is fed from
   user32's SDL pump, so a frame that reads input without pumping sees no
   buffered events. Immediate `GetDeviceState` works either way.

## How the game code gets in

Not yet started, and deliberately so - the boundary layers had to be provable
first. The intended path, in order:

1. Keep using the existing patch pipeline (`tools/unwrap.py` and friends) for a
   working exe under wine; that stays the reference to diff behaviour against.
2. Port bottom-up from `DIRECTX_SCOPE.md`'s 99-function renderer scope, since
   that is where the D3D9 boundary is and where the two independent widescreen
   fixes already live. `ghidra_types.h` exists so a freshly exported function
   compiles with minimal edits, then gets its types narrowed.
3. Everything not yet ported stays in the original binary. That means the
   in-process PE loader + import thunking has to come before the port is
   runnable end-to-end - which is why the shim is defined `WINAPI` (stdcall)
   throughout instead of the more convenient cdecl.
