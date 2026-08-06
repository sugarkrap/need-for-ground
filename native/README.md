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
src/win32/           the shim: files, paths, heap, vmem, threads, sync, time, ...
src/host/            smoke_d3d9.c - window + device + clear/present
tests/               shim selftest, D3D9 convention selftest, header ABI probes
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
- **d3d9-abi** - D3D9 vtable dispatch through Wine's macros in the cdecl world:
  arguments arrive intact, seven-argument calls marshal, and the caller's stack
  is still balanced after 100k calls.
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

## Win32 coverage

```sh
python3 native/tools/win32_coverage.py          # summary + grouped missing list
python3 native/tools/win32_coverage.py --done
```

It cross-references `../analysis/win32_imports.txt` (what the unwrapped exe
actually imports) against the entry points defined in `src/win32/`, so
"how much Win32 is left" is measured rather than guessed.

## Known gaps, in rough priority order

1. **Window and input** (`user32`): no window creation, message loop, or
   `dinput8`. The smoke test uses SDL2 directly; the game's own
   `CreateWindowExA` / `PeekMessageA` / `DispatchMessageA` path needs an
   SDL-backed message pump.
2. **CRT and locale**: the import list is full of MSVCRT support entry points
   (`LCMapStringA`, `GetStringTypeA`, `MultiByteToWideChar`, ...). Ported code
   compiled with gcc will use glibc for most of it, but any code path kept as
   original machine code needs these.
3. **Registry**: `RegOpenKeyExA`/`RegQueryValueExA` for settings. An INI file
   under the game root is the obvious backing store.
4. **SEH**: `SetUnhandledExceptionFilter` is a deliberate no-op; real
   SIGSEGV -> `EXCEPTION_RECORD` translation is its own piece of work.
5. **Audio** (`mss32.dll`, Miles Sound System) is not addressed at all.
6. **`VirtualFree(MEM_RELEASE)`** leaks the mapping because reservation sizes
   are not tracked. Harmless for a process that exits; wrong for anything that
   cycles large allocations.

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
