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
src/loader/          minimal in-process PE mapper, for differential testing
src/host/            smoke_d3d9.c (SDL-driven) and smoke_game_loop.c (game-shaped)
game/manifest.txt    which functions to import from decompiled/ (code stays local)
tests/               shim, services, user32, dinput8, D3D9 convention; ABI probes
tools/               DXVK build script (sh) + TypeScript tooling, run by Deno:
                     import_decompiled, survey_decompiled, win32_coverage, abi_diff
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
- **services** - 92 checks over the registry (including persistence across a
  close/reopen), Winsock driven by a real TCP and a real UDP conversation over
  loopback plus a non-blocking connect, `select()`, FIONREAD/FIONBIO,
  gethostbyname and the interface list; CP1252/UTF-8 conversion; the gdi32
  object model; mmap-backed file mapping (including that a mapping outlives its
  file handle); and a multimedia timer's callback rate.
- **dinput8** - 37 checks over device enumeration, the acquire/state contract,
  and both input paths: immediate `GetDeviceState` and the buffered
  `GetDeviceData` stream, fed by synthetic SDL events and checked against the
  DIK scancodes the game will look for. The buffered assertions filter for the
  keys the test pushed rather than counting events, because the buffer is fed
  from the real SDL stream - a keypress from whoever is at the machine lands in
  it too, and a test that fails when someone touches the keyboard is worse than
  no test.
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
deno task coverage                # summary + grouped missing list
deno task coverage -- --done
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

## Solved: the window that showed black

Kept because the investigation is more useful than the answer, and because the
answer was our own test rig.

**Cause: a frame whose entire content is a `Clear` never reaches the screen.** DXVK
defers a Clear on a render target and materialises it when the image is next
actually used. The swapchain blit does not trigger that, so it samples an uncleared
- black - image, while `GetRenderTargetData` *does* trigger it and returns the
cleared colour. Hence the contradiction that drove the whole investigation: "the GPU
renders correctly, and the window is black".

**Fix: draw something.** Both hosts now draw a quad (fixed-function,
pre-transformed vertices, `SetFVF`/`SetRenderState`/`DrawPrimitiveUP` - the same
calls the game's own UI uses) and the whole frame appears, clear included. No real
game presents a Clear-only frame, so this never affected the port; it only ever
affected hosts that did nothing but clear. `--clear-only` on the SDL host
reproduces the black window on demand.

It is arguably still a DXVK bug - on Windows a Clear-only frame presents the
cleared colour - but it is a corner case nothing real hits, and the probe patch in
`patches/` is what a report would need.

### What the investigation ruled out on the way

Worth reading before trusting a similar diagnosis. In order: present mode
(`IMMEDIATE` and `FIFO`), word size (32- and 64-bit identical), SDL's video
backend (`wayland` and `x11`), a window/backbuffer size mismatch, `BackBufferCount`,
Vulkan API misuse (core, synchronisation *and* best-practices validation all clean),
app-side ordering (`--sync-each-frame` forces a flush and wait before every
Present), swapchain image index handling (acquire, draw and present all name the
same image), the present rects (full window), and DXVK's D3D9 backbuffer rotation
(`extraFrontbuffer` defaults to false, so there is one stable backbuffer).

`DXVK_CONFIG="dxvk.numBackBuffers = 3"` (or `--present-workaround`) makes content
appear in roughly one frame in ten, which looked like a fix from a single screen
capture and was actually a flicker - ten captures a quarter-second apart showed it.

### Three lessons, all of which cost time here

1. **A frame counter is not evidence that anything is on screen.** "presented 240
   frames" was reported as success for several commits while the window was black.
   `--readback` and `--readback-png` exist so that claim can be checked.
2. **One capture is not evidence of a fix.** The `numBackBuffers` change looked
   like a fix from one screenshot and was a one-in-ten flicker.
3. **A measurement tool needs its own control.** The in-DXVK probe reported "the
   presented image is empty" while missing a host-read barrier, and that got
   reported as a DXVK bug. Capturing the *source* image - which could not possibly
   be empty - is what exposed the probe as faulty. See
   `patches/dxvk-blit-probe.md`.

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

## Running game code

Started, and verified two ways. `native/tools/import_decompiled.py` turns your
local `decompiled/` output into compilable translation units, driven by
`game/manifest.txt`; the generated code is gitignored, because decompiled
pseudocode is a derivative of the copyrighted binary exactly as `decompiled/`
itself is. The manifest, the tooling and the tests are what live in the repo.

```sh
deno task import          # native/game/manifest.yaml -> game/generated/
meson setup native/build32 --cross-file native/cross/linux32.txt \
      -Dnfsu2_exe=/path/to/unwrapped/speed2.exe
meson test -C native/build32 game-functions
```

The manifest is YAML, and the importer generates the differential test's plumbing
from it: `game_originals.h` carries each function's address and a
function-pointer typedef built from the signature Ghidra recovered - including
the calling convention, which is the part that cannot be guessed. Adding a
function is therefore a manifest entry and nothing else.

Verification is deliberately two-fold:

1. **Against a reference** - the statically-linked CRT functions against libc,
   the game's own maths against a recomputation. Catches a port that compiles
   but computes the wrong thing.
2. **Against the original machine code** - `src/loader/pe_loader.c` maps the
   unwrapped exe's sections at its own ImageBase (0x400000) inside this process,
   and the same inputs go through both the ported C and the original compiled
   function. 282 comparisons currently, all identical, including
   `FUN_0043ce40` being *bit*-identical (both compute in x87 80-bit, so anything
   less than equality would mean the port changed the arithmetic) and
   `FUN_0048b710` receiving its `__thiscall` argument in ECX correctly.

That second one is the important one: it does not depend on anyone having
guessed the intended behaviour correctly, which is what makes porting the
remaining functions tractable rather than speculative.

### What the corpus actually looks like

`native/tools/survey_decompiled.ts` (`deno task survey`) compiles decompiled functions in isolation
and buckets the failures, so the port's bottleneck is measured rather than
assumed. On a 500-function sample, with calls to undeclared callees and `DAT_` globals
neutralised because those are artefacts of compiling one function alone:

```
compiles as-is : 356/500 (71.2%)
  14.2%  undeclared identifier (another function or a global)
   3.0%  invalid unary '*' on an int (a pointer Ghidra typed as an integer)
   2.6%  field-slice syntax on a scalar (_X._6_2_)
   1.8%  invalid use of a void expression
   1.2%  type mismatch
```

Three functions were tried and excluded from the manifest, and they are a fair
sample of the remaining 30%: `shortsort` reads an `in_EAX` Ghidra invented
because it missed a parameter; `__ftol` takes its argument on the x87 register
stack, which has no C spelling at all; `__isnan` uses Ghidra's field-slice
syntax on a `double`. Each needs a different fix, and none of them is generic.

### What comes next

1. Widen the manifest. The renderer scope in `../DIRECTX_SCOPE.md` is the useful
   direction, since that is where the D3D9 boundary and both widescreen fixes
   already are.
2. Teach the loader imports, so a ported function can call one that is not
   ported yet - the shim already resolves names (`win32/module.c`), so this is
   thunking the IAT, not new infrastructure.
3. The TEB and `%fs`. Any function with a `__try` block or a `__declspec(thread)`
   access reads glibc's TLS through `%fs` and misbehaves. Wine solves it with a
   custom LDT entry via `modify_ldt()`; that is the next real piece of work.
