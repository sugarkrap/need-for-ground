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
- **game-functions** - the ported functions, twice each: against a reference, and
  against the original machine code with identical inputs (283 comparisons). Plus
  import resolution, hybrid execution both directions, the TEB, and a renderer
  function checked against a fake D3D9 device. Needs an unwrapped exe
  (`-Dnfsu2_exe=`); without one it runs the reference half.
- **seh** - 24 checks over the exception dispatcher: the handler chain walked in
  order until one accepts, the catch path (unwind the frames in between, then
  transfer control - `setjmp`/`longjmp` standing in for the `jmp` a `__except`
  block does), what `RaiseException` puts in the record, `RtlUnwind`'s bounds and
  its `EXCEPTION_UNWINDING`/`EXCEPTION_EXIT_UNWIND` flags, and a real SIGSEGV
  repaired by the handler and resumed. Two more run in forked children, because
  refusing a corrupt chain has no non-fatal outcome: the parent checks *how* the
  child died. 32-bit only.
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

## Launching the game

`nfsu2-game-launch` maps the exe, resolves its imports onto the shim, gives the
thread a TEB and the process a fault handler, and jumps to `AddressOfEntryPoint`.
There is nothing left to build before trying that, and trying it is the only way to
find out what the game needs next *in the order it needs it*.

```
$ NFSU2_SHIM_TRACE=1 ./build32/nfsu2-game-launch --exe /path/to/speed2.exe
mapped     : base 0x400000, 5312 KiB, entry 0x75b8d1
imports    : 249 of 251 resolved (21 by ordinal)

--- entering the game at 0x0075b8d1 ---

[nfsu2/shim] registry: no store at ./registry.ini (starting empty)
[nfsu2/shim] RegOpenKey: HKEY_LOCAL_MACHINE\Software\Microsoft\Direct3D not found
[nfsu2/shim] HANDLE alloc 0x57821040 kind=8 size=137232       <- toolhelp snapshot
[nfsu2/shim] HANDLE free 0x57821040 kind=8
[nfsu2/shim] DuplicateHandle(current thread): returning the pseudo-handle itself
[nfsu2/shim] HANDLE alloc 0x577e5f40 kind=5 size=108          <- a thread
[nfsu2/shim] RegOpenKey: ...\Need for Speed Underground 2\ergc not found
[nfsu2/shim] HANDLE alloc 0x577f0750 kind=3 size=92           <- events
[MessageBox] NFS Underground 2: Please insert Disc 2
```

**The game's own startup runs.** CRT initialisation, its anti-debug process scan,
its registry lookups, its threads and events - and it stops at its own disc check,
which is the game telling us what it wants rather than anything failing underneath
it.

Five bugs stood between the entry point and that dialog, and every one of them was
invisible to the test suite:

1. **The PE headers were not mapped.** An `HMODULE` *is* the image base on Windows,
   and MSVC's CRT startup calls `GetModuleHandleA(NULL)` and then
   `cmp WORD PTR [eax], 0x5a4d` looking for `MZ`. The loader now maps
   `SizeOfHeaders` bytes at the base, and `GetModuleHandleA` returns the base
   rather than an opaque handle (also for the exe's own name, which is as common).
2. **`CloseHandle` trusted any pointer.** The game closes something that is not one
   of our handles, whose first two words happen to read as
   `{kind = FIND, refs = 1}` - arbitrary data is full of small integers. There is
   now a table of the handles we issued, and only pointers in it get dereferenced;
   anything else gets `ERROR_INVALID_HANDLE`, which is what Windows says.
3. **Two object types shared a kind.** `toolhelp.c` tagged its process snapshot
   `NFSU2_OBJ_FIND` because it had "the same lifetime shape and no separate
   destructor needed". The kind selects the *destructor*, so closing a snapshot ran
   the find destructor over it, read the process count as a `char *` and the first
   pid as a `DIR *`, and called `closedir(1)`. It presented as a fault inside libc,
   five frames from anything that explained it.
4. **Pseudo-handles were not handled.** `DuplicateHandle(GetCurrentProcess(),
   GetCurrentThread(), ...)` is the standard way to get a thread handle that
   outlives the call, and `-2` was being dereferenced as an object.
5. **`CREATE_SUSPENDED` was refused**, on the reasoning that "nothing in this game
   creates suspended threads". It does, twice, before it has drawn anything. There
   is now a real start gate and a suspend count, with `ResumeThread` returning the
   previous count as callers expect.

Two of those five were comments asserting the game would never do something. Both
were wrong, and neither could have been contradicted by anything except running it.

Threads the game creates now get their own TEB in the trampoline, since `%fs` and
the TEB are both per-thread and the first `__try` on a worker would otherwise write
through a stray segment.

### The disc check, solved

It is two questions, and it took running the game to find either:

1. `GetDriveTypeA("J:\")` must answer `DRIVE_CDROM`. `J:` is not a guess - the game
   reads it out of its own registry, where the installer wrote
   `"CD Drive"="J:\"`.
2. `CreateFileA("J:\bin.dat")` must succeed. `BIN.DAT` is a 130-byte file in the
   root of disc 2.

Neither had an answer here, for reasons that were written down as facts:
`volume.c` said "the disc check is gone (that is what tools/unwrap.py is for)" -
but `unwrap.py` removes *SafeDisc*, and EA's own media check is a different thing
that is still in the executable. And `path.c` said "the game only ever uses its own
install drive", so every drive letter resolved to the game root.

So there are now two small mechanisms, and neither invents anything:

- **`tools/import_wine_registry.ts`** copies the game's own keys out of a Wine
  prefix into `registry.ini` - its registration code, its video settings, and the
  `CD Drive` value the check is built on. They are the user's own values from their
  own install, and `registry.ini` stays in the game directory: a registration code
  does not belong in a repository. `Wow6432Node` is stripped, because a 64-bit
  prefix redirects a 32-bit installer's writes and this port has no such
  redirection - the paths the game asks for, visible in the trace, have none.
- **`NFSU2_DRIVE_<letter>=[cdrom:]<host dir>`** maps a drive letter to a directory
  and says what kind of drive it is. Unmapped letters still mean the install drive,
  and an unmapped `J:` still reports `DRIVE_NO_ROOT_DIR` - the honest answer when no
  disc has been provided.

With those, the dialog is gone and the game runs on into graphics initialisation.

### The D3D9 bridge: the game's code on DXVK

Two things stop the game from calling DXVK Native directly, and the bridge exists
for both:

1. **Calling convention.** MSVC compiled the game against Windows' COM ABI, where
   methods are `__stdcall` and the callee pops. DXVK Native defines
   `STDMETHODCALLTYPE` as empty, so its methods are `__cdecl` and the caller pops.
2. **Stack alignment.** MSVC-compiled i386 code does not keep the stack 16-byte
   aligned; GCC-compiled code assumes it does and is full of `movaps` on stack
   slots.

So every method the game reaches goes through a thunk that is `__stdcall` *and*
carries `force_align_arg_pointer`, forwarding to DXVK's `__cdecl` method. There are
274 of them across 17 interfaces, generated by `tools/generate_d3d9_bridge.ts` -
which takes its tables from `analysis/derive_vtable_args.py --json`, so the one
parser that `--check` verifies against `directx_vtables.py` feeds both Ghidra's types
and this.

Arguments forward as raw 4-byte slots rather than typed parameters. That is
bit-preserving on i386 - a float arrives as four bytes and leaves as the same four -
and it keeps the generator out of the business of parsing C declarations, which is
where its bugs would otherwise live. Eight-byte arguments occupy two slots and the
parser counts them. `GetNPatchMode` is generated with a `float` return type because
it comes back on the x87 stack rather than in EAX.

COM pointers are translated in both directions - 46 out-parameters wrapped, 22
in-parameters unwrapped - and identity is preserved through a table, because
`GetBackBuffer` twice must return the same pointer and games compare interface
pointers. `Release` frees the bridge exactly when the object it wraps reaches zero.

The game reaches it through its import table: the loader resolves
`d3d9.dll!Direct3DCreate9` to the bridge *first*, before `dlsym`, because DXVK does
export that name and it is the wrong ABI for this caller. Nothing here shadows it for
the rest of `native/` - our own hosts keep calling DXVK's cdecl entry point.

With that in place:

```
[nfsu2/shim] d3d9 bridge: wrapped IDirect3D9 0x56978660 as 0x5697a120
[nfsu2/shim] d3d9 bridge: wrapped IDirect3DDevice9 0x5724e540 as 0x57212530
[nfsu2/shim] d3d9 bridge: wrapped IDirect3DVertexDeclaration9 ...
```

**The game creates a real D3D9 device on Vulkan and starts building its renderer
state.** It then faults at `0x5d180a`, in its own code, with a second thread
reporting no TEB - which is the next thread of investigation rather than a mystery.

Worth recording, because it is the third time the same trap has appeared: the
loader's reference to the bridge is *weak*, and a weak undefined reference does not
pull a member out of a static archive, so the bridge linked in name only and the game
went straight to DXVK's export again. `link_whole` rather than `link_with`. The
`--as-needed` version of this cost an afternoon two commits ago.

### The alignment fault that made the bridge necessary

```
Thread 1 received signal SIGSEGV
0xf7973448 in dxvk::DxvkInstance::DxvkInstance(...)
=> movaps %xmm0,-0x178(%ebp)
```

The game calls `Direct3DCreate9` through its import table, DXVK gets four frames
into constructing its instance, and dies on an *aligned* 16-byte SSE store to a
stack slot. **MSVC-compiled i386 code does not keep the stack 16-byte aligned;
GCC-compiled code assumes it is.** Our own shim never had this problem because
`WINAPI` carries `force_align_arg_pointer` (see `win32_compat.h`) - every one of the
250 resolved imports realigns on entry. DXVK's exports are not ours and do not.

That makes the next piece of work concrete, and it is the same piece the fake-device
test predicted from the other direction: **a D3D9 bridge**. Every entry point the
game reaches in DXVK needs a thunk that is `__stdcall` (Windows' COM ABI, callee
pops) *and* `force_align_arg_pointer` (realign for GCC's SSE), forwarding to DXVK's
`__cdecl` method. That is one thunk per method, which is exactly the sort of thing
to generate rather than write - and `analysis/derive_vtable_args.py` already reads
the argument count of every method out of the SDK headers, which is all a generator
needs.

### What the disc check needed (superseded by the section above)

The dialog comes from the game's own code and loops because `MessageBoxA` answers
`IDOK`, which it reads as "retry". `GetVolumeInformationA` is not in its import
table, so the check is not a volume-label comparison; the string lives at
`0x7a3c00`, and the next step is an xref query in Ghidra to find the function that
decides. The registry keys it looked for and did not find - notably
`...\Need for Speed Underground 2\ergc`, the registration entry a real install
writes - are the other half of the answer, and `registry.ini` can carry them.

### PE resources, because the shaders are in them

The lead from the bridge was a null-pointer fault at `0x5d180a`, in a `__thiscall`
that reads a COM pointer from `this+0x20` and immediately dereferences its vtable.
`eax` was 0. Tracing every *failing* D3D9 call showed none - so nothing in the
graphics path had failed. What fills that field is visible in the caller:

```c
FUN_00640852(PTR_00870974, 0, (&PTR_s_IDI_WORLD_FX_007ff854)[param_2 * 0x23], ...,
             param_1 + 0x20, ...);   /* device, a resource name, an out pointer */
FUN_005d1800();                      /* which then dereferences it */
```

**The game loads its shader effects from PE resources.** `resource.c` failed every
call, on the stated reasoning that a native ELF has no `.rsrc` and the game only
wanted its icon and its version block. The first half is still true; the second was
wrong, and the symptom was a fault three call levels away from anything mentioning
resources.

No extraction tool was needed after all. The loader maps *every* section of the exe
at its own base, `.rsrc` included, so the resource directory is already in memory in
exactly the layout PE/COFF describes - a three-level tree of type, then name, then
language - and walking it is the whole implementation. `FindResourceA` returns a
pointer to the leaf `IMAGE_RESOURCE_DATA_ENTRY`; `LoadResource` and `LockResource`
both resolve to `image base + OffsetToData`, because in a mapped image the bytes are
already there and there is nothing to commit or unlock.

With that, the game gets its shaders:

```
[nfsu2/shim] FindResourceA(#10, IDI_WORLD_FX) = 0x8bc4e8
[nfsu2/shim] FindResourceA(#10, IDI_CAR_FX) = 0x8bc388
[nfsu2/shim] d3d9 bridge: wrapped IDirect3DPixelShader9 ...
[nfsu2/shim] d3d9 bridge: wrapped IDirect3DVertexShader9 ...
[nfsu2/shim] d3d9 bridge: wrapped IDirect3DTexture9 ...
[game] EZ Wheel Wrapper v4.26
```

**23 resources found, 519 D3D9 objects wrapped** - shaders, textures, vertex and
index buffers, state blocks, vertex declarations - and the game reaches the point of
printing its own log line. It stops next in *input* initialisation, on
`IDirectInput8::EnumDevicesBySemantics` (DirectInput's action-mapping API, which is
what "EZ Wheel Wrapper" wants), then faults at `0x80000001` - an address that is
plainly a returned status being called as a function pointer.

### Every import resolved, and the audio gap made honest

The fault at `0x80000001` was not an address at all. `DSOUND.dll!#1` was the last
unresolved import, and an unresolved slot still holds what the file put there - for
an ordinal import, `IMAGE_ORDINAL_FLAG | 1`. The game's own `jmp [0x78302c]` thunk
jumped to it. Three fixes came out of that:

- **`src/dsound/dsound.c`** answers `DirectSoundCreate` with `DSERR_NODRIVER`. Audio
  is still not implemented; this is the truth about that, and it is an outcome every
  Windows game had to handle - a machine with no sound card. **251 of 251 imports now
  resolve.**
- **Unresolved imports get a named stub.** The loader used to leave the slot as the
  file had it, on the reasoning that a stub returning a plausible value turns "this
  API is missing" into a crash somewhere else. That reasoning holds for *faking a
  result* - and this does not fake one. Ten bytes of i386 put the stub's index in ECX
  and jump to a reporter that names the import and stops. The failure stays exactly
  as loud, and lands at the call.
- **A thread's object outlives its handle.** `CloseHandle` on a running thread's
  handle dropped the last reference and `nfsu2_thread_destroy` ran
  `pthread_mutex_destroy` on the lock the trampoline was using, which glibc caught as
  `assertion failed: mutex->__data.__owner == 0` from inside `pthread_cond_wait` -
  with nothing pointing at a closed handle. The running thread now holds its own
  reference and releases it as its last act. The suspend gate added earlier is what
  made this reachable; the bug was always there.

### Where it stops now

The game gets through graphics, input and audio initialisation and faults in
`FUN_005793c0`, which is an asset-relocation routine: it links an object into a list
and then walks an array at `+0x20` in strides of 0x14, turning relative offsets into
absolute pointers by adding the object's base. The loop count comes from the data
(`param_1[3]`), so a wrong count walks off the end - which means the data is wrong,
not the loop.

That reading was wrong, and the thing that showed it was running the game again.
**The failure is not deterministic.** Three consecutive runs, unchanged binary:

```
run 1: malloc(): invalid next size (unsorted)      -> abort
run 2: exception 0xc0000005 at 0x5793e7            -> the relocation loop
run 3: exception 0xc0000005 in ld.so               -> _dl_allocate_tls_init
```

`malloc(): invalid next size` is glibc reporting **heap corruption**: something has
written past the end of an allocation. That single line reframes all three: the
relocation loop walking off the end is a *symptom* of corrupted data, not a cause,
and a crash inside the dynamic loader's TLS allocation is what a corrupted heap looks
like when the next thing to allocate happens to be a thread.

So chasing the relocation loop, or the fact that no data files had been opened, would
have been chasing downstream effects. What is worth recording about that run instead:

- Only **3** of the ~130 threads in the process are ours; the rest are DXVK's
  shader-compile workers churning. A 32-bit address space with that many stacks plus
  DXVK plus the game is also close enough to full that allocation failures are
  plausible on their own.
- `IDirect3DDevice9::BeginStateBlock` fails repeatedly with `D3DERR_INVALIDCALL`,
  which is what DXVK returns when a state block is *already* recording. That is a
  real bug to fix on its own merits, and it may or may not share a cause with the
  corruption.

#### What the hunt for it has established

Three things were tried. None found the writer, and each ruled something out or
turned up something worth knowing.

**AddressSanitizer is not usable here as-is.** A `-m32` ASan build works
(`meson setup build32-asan --cross-file cross/linux32.txt -Db_sanitize=address`, with
`ASAN_OPTIONS=handle_segv=0` so our own SEH keeps the fault handler), but the game
then fails *earlier* - before it creates a single D3D9 object - with no ASan report at
all. Replacing the allocator changes the program's behaviour rather than observing
it, so the corruption never happens in the same place.

**Padding every HeapAlloc by 32 bytes made it fail earlier and consistently.** That
is not a null result: it can only happen if the game *uses* the inflated size, and it
gets one because `HeapSize` answers `malloc_usable_size`. Windows returns what was
asked for. So this game is squarely in the group the comment in `heap.c` warned
about - "anything that round-trips it as an element count".

**Tracking the requested size in a block header made it worse still**, and for a
second, separate reason: `GlobalAlloc` handed out plain `malloc` pointers, so a
`GlobalFree` of a `HeapAlloc` block called `free()` on an interior pointer.
On Windows both come from the process heap and code from 2004 crosses them freely.
Any header scheme has to cover every family at once, and moving the pointer also
moves whatever alignment the game was getting by accident.

That points at the right design, which is the next thing to build: **keep the pointer
exactly where glibc put it.** Record `(pointer -> requested size)` in a *side* table
at `HeapAlloc`, fill the difference between the requested size and
`malloc_usable_size` with a pattern, and check that pattern at `HeapFree` and
`HeapReAlloc`. Nothing moves, so there is no alignment or interior-pointer hazard; a
small overflow is caught at the block that suffered it, with its size and the API
that allocated it; and `HeapSize` can answer the requested size, which is what
Windows does and is a correctness fix regardless of the corruption.

#### What the canary found, and what that rules out

`heap.c` now keeps a side table of `(pointer -> requested size)` and fills the gap
between the request and `malloc_usable_size` with `0xb7`, checking it at `HeapFree`
and `HeapReAlloc`. Nothing moves, so there is no alignment or interior-pointer
hazard, and `HeapSize` finally answers what Win32 answers - the size that was asked
for - rather than glibc's larger usable size. `GlobalAlloc`/`GlobalFree`/`GlobalSize`
route through the same table, because on Windows both families come from the process
heap and this game crosses them.

**No overrun is detected, in any run.** So the corruption is *not* a small write past
a `HeapAlloc` block: that is exactly what the canary would catch, and it never fires.
What remains:

- a write far past a block, skipping the slack entirely and landing in another
  chunk's header
- a write into memory that has already been freed
- a write past a buffer that **DXVK** owns, not us - a locked vertex or index buffer
  being the obvious candidate, since `Lock` hands the game a raw pointer and the
  canary cannot see allocations DXVK made with its own `new`

The third is the most promising, and it is where to look next. It also fits the
symptom: with tracing on, three consecutive runs now fault in *libc*, on a worker
thread (`esp` in a thread stack, not the main one), writing to a high mmap address -
which is what a corrupted allocator arena looks like when the next thread to allocate
trips over it.

#### One trap, and one correction

**The game persists state that changes its startup path.** It writes `NotFirstTime`,
`SwapSize` and `Language` into `registry.ini` once it gets far enough, and then takes
a different path - a fullscreen 2560x1080 swapchain rather than the first-run one. Any
before/after comparison should re-import a clean store:

```sh
deno run -A tools/import_wine_registry.ts --prefix /mnt/games/NFSU2/pfx \
    --out "<game dir>/registry.ini"
```

**And a correction to what this file said a commit ago.** It claimed the failure point
moves between runs and that how far the game gets varies wildly. That was measurement
error: the bridge and resource traces only print with `NFSU2_SHIM_TRACE=1`, and a
series of comparison runs had been made without it - so "0 D3D9 objects wrapped" meant
"tracing was off", not "the game got nowhere". With tracing on and the same registry,
three consecutive runs are identical: **519 D3D9 objects wrapped, 23 resources found**,
then the same crash. The progress is reproducible; only the exact faulting address
moves, which is what a corrupted heap does.

## Known gaps, in rough priority order

1. **Audio.** `mss32.dll` (Miles Sound System) is not in the import list at all -
   the game loads it dynamically - and nothing implements it. This is the largest
   remaining hole in the platform layer.
2. **The game's CRT startup.** SEH itself works (see below), but the first thing
   the game's own C++ frame handler does is take a CRT lock, and the CRT lock
   table is only filled by the game's startup code. Until that runs, any original
   code path that locks recurses to death. This is now the blocker in front of
   hybrid execution, and it is not a Win32 gap at all.
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

### Imports resolve, so original code can call the shim

`nfsu2_pe_resolve_imports()` walks the import directory and points each IAT slot at
our own implementation. On the unwrapped exe:

```
14 libraries, 251 imports: 249 resolved, 2 unresolved (21 by ordinal)
  unresolved: d3d9.dll!Direct3DCreate9     (DXVK, not linked into this test)
  unresolved: DSOUND.dll!#1 (ordinal)      (audio, genuinely not implemented)
```

The test then checks the thing that matters: an IAT slot really does hold the
address of *our* `GetTickCount`. Unresolved slots are left alone and counted rather
than filled with a stub, because a stub turns "this API is missing" into a crash
somewhere unrelated.

Three things had to be fixed to get from 188 to 249, and all three were the same
hidden-visibility trap in different clothes:

- **shell32** and **NTSYSAPI** (`RtlUnwind`) needed `_SHELL32_` and `_NTSYSTEM_`
  adding to `win32_dllmacros.h`, exactly as kernel32 and user32 did.
- **ws2_32 cannot use dlsym at all.** Its entry points are hidden *on purpose* -
  exporting names like `socket` and `select` hijacks libc for the whole process -
  so the loader consults an explicit table in the sockets shim via a weak symbol.
- **ws2_32 is imported by ordinal**, not by name: 21 of the game's Winsock imports
  have no name in the file at all. The same table carries ws2_32.dll's documented
  ordinals, without which those 21 slots would stay empty.

### Hybrid execution works

Two manifest entries call Win32 imports, which makes them the interesting case:
the ported copy calls our shim directly, and the original - once the IAT is
resolved - reaches the same shim through its import table.

```
ok - the ORIGINAL FUN_006f5ac8 returned 7720315, bracketed by our own
     7719965..7720405 - it called our QueryPerformanceCounter
ok - the ORIGINAL __fastcall FUN_006fea95 returned its ECX argument
ok - it stored timeGetTime()=7, within our 7..7 - our shim again
```

The bracketing is the proof, not the agreement: these return clock readings, which
move, so exact equality is not available. Our clocks count from *process start*, so
their values are small - a real `timeGetTime` returns milliseconds since boot and a
real `QueryPerformanceCounter` a TSC-scale value, either of them orders of magnitude
larger. A reading sandwiched between two of our own samples can only have come from
our implementation.

So original machine code and ported C can now call the same shim, which is the
property incremental porting needs. `__fastcall` (argument in ECX) is exercised on
both sides too.

One more decompiler-versus-header spelling difference turned up here and is fixed
in the importer: Ghidra calls `LARGE_INTEGER`'s nested struct `s`, Wine calls it
`u`, so `value.s.LowPart` does not compile. Rewritten narrowly, on the two field
names that can appear, rather than by replacing `.s.` everywhere.

### A TEB in %fs, so `__try` code runs

MSVC-compiled 32-bit code reaches the TEB implicitly and constantly: `fs:[0]` is
the SEH handler chain, which *every* function with a `__try` pushes onto and pops
off; `fs:[0x2c]` is the TLS array; `fs:[0x34]` is `LastError`. Without a TEB such a
function reads and writes through whatever `%fs` happens to hold, and inline SEH is
the shape most of the C++ in this binary has - so this is the wall between "leaf
functions work" and "call anything".

i386 makes it cheap, and the reason is worth stating because it is the opposite of
what x86_64 habits suggest: on i386 Linux glibc keeps its thread pointer in **`%gs`**,
leaving `%fs` free - which is exactly why Windows uses `%fs` there. So
`modify_ldt()` (the same mechanism Wine uses) points a private LDT entry at a TEB of
our own, `%fs` is loaded with the resulting selector, and libc never notices.
`native/src/win32/teb.c` is a few dozen lines because of that; on x86_64 the roles
swap and the entry points fail cleanly instead.

The test does not settle for "a field changed". It hands the original destructor at
`0x40a1f0` - which MSVC gave an inline SEH prologue - a child object whose vtable
points at a thunk of ours, so the original's own virtual call lands in our code
*while its SEH record is still linked*:

```
ok - inside it, fs:[0] was 0xff814200 - a live SEH record in our TEB
ok - and the record's handler is 0x00770fc8, the address MSVC pushed (0x770fc8)
ok - it restored the chain to 0xffffffff on the way out
ok - the PORTED copy linked its own record too (fs:[0] was 0xff814208 inside)
```

Reading the handler address back out of the record is the part that cannot be
coincidence: `0x770fc8` is a constant in the original's prologue, so the chain head
`%fs` reaches really is the record that code pushed.

Two findings came out of it that are worth more than the test:

- **`0x75d950` is not a function.** Ghidra decompiles it as
  `ExceptionList = auStack_c; return;`, which reads like the perfect TEB test case.
  It is MSVC's `__SEH_prolog`: it pushes an `EXCEPTION_REGISTRATION` record, links
  it, moves the caller's return address to the top of the stack and `ret`s to it -
  returning having permanently consumed 12 bytes of the caller's stack and
  overwritten the caller's saved EBP. Calling it from C smashed the frame, and the
  damage surfaced two functions later as unresolvable addresses plus a stack-canary
  abort at exit, which is a thoroughly misleading place to start debugging. It is
  now in the manifest's excluded list with that explanation.
- **Ghidra's `(*(code *)...)(1)` has lost the calling convention.** The original
  call site is `__thiscall` - `this` in ECX, callee pops the argument - and the
  ported copy compiles to `__cdecl`. Both run here only because the test supplies a
  matching thunk per side. Vtable calls in ported code will need the convention put
  back by hand; the test says so where it does it.

Inline SEH also needed one importer feature: those functions name their handler and
their vtables as *addresses* - `&LAB_00770fc8`, `&PTR_FUN_00784680`. The name is the
address, and the address is valid the moment the PE is mapped at its own base, so
the importer binds the address-of form to a literal. A bare `DAT_...` read is
refused instead, because its width is exactly what Ghidra did not commit to and
guessing dword when it is a byte would corrupt a neighbour silently. `LAB_` names
that are `goto` targets are left alone - Ghidra uses the prefix for both, and
`_strncpy`'s loop is real C labels.

### SEH: signals in, the game's handler chain out

The game binary carries its own exception machinery. Every function with a `__try`
or a C++ `try` links a record onto `fs:[0]`, and the handler in that record is code
*inside the exe*: MSVC's `__except_handler3` and `__CxxFrameHandler` are statically
linked, and each function with C++ EH gets a ten-byte thunk
(`mov eax, <FuncInfo>; jmp __CxxFrameHandler`) whose address is what the prologue
pushes - `0x770fc8` is one of them. None of that is ours to write.

What is ours is the operating system's half, and that is what
`src/win32/exception.c` now is:

- **the dispatcher** - walk `fs:[0]` calling each `__cdecl` handler with
  `(record, frame, context, dispatcher)` until one returns
  `ExceptionContinueExecution`, honouring `NestedException` and refusing to
  continue a non-continuable exception
- **`RaiseException`** - which is how MSVC's `throw` (code `0xe06d7363`) and the
  game's own error paths start one
- **`RtlUnwind`** - the second pass, which pops the frames between the throw and
  the catch with `EXCEPTION_UNWINDING` set so destructors run. It *returns
  normally*, because on i386 the handler is what transfers control: it restores
  esp/ebp from its own registration record and jumps to its `__except` block
- **faults** - SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP become an `EXCEPTION_RECORD`
  plus a `CONTEXT` built from `ucontext_t`, and `ExceptionContinueExecution`
  writes the (possibly edited) context back so the kernel resumes from it
- **the top-level filter** - `SetUnhandledExceptionFilter` is wired up, so a game
  that installs a crash handler still gets called

Two decisions worth stating. There is **no sigaltstack**: handlers run on the
faulting thread's own stack, which is what Windows does and what makes it safe for
a handler to abandon the signal frame by jumping to its `__except` block (with
`SA_NODEFER` so that does not leave the signal blocked forever). The cost is that
`EXCEPTION_STACK_OVERFLOW` cannot work - a fault on the guard page has no stack
left to build a frame on. And an unhandled fault does **not** `abort()`: the
default disposition goes back and the handler returns, so the faulting instruction
re-executes and the process dies exactly where it went wrong, which is the core
dump worth having.

The dispatcher also refuses a chain that does not look like one - records must be
4-byte aligned, inside the thread's stack, and link strictly outwards. `fs:[0]` is
a stack address written by code we did not compile, and the alternative to a
diagnostic is calling a function pointer read out of a random stack slot.

#### What the end-to-end attempt found

Raising an exception from inside a call made by the original `FUN_0040a1f0` - so
that the innermost frame in the chain is the game's own C++ EH thunk - does reach
MSVC's frame handler with a real record and real `FuncInfo`. Then it dies, in a
two-frame recursion inside the game's CRT:

```
__InternalCxxFrameHandler        0x762e0a
  -> _mtinitlocknum(10)          0x762d72   allocate lock 10
       -> _lock(10)              0x762df1   ...which locks lock 10
            -> _mtinitlocknum(10)           ...which is still NULL
```

MSVC's C++ EH takes a CRT lock before allocating its per-thread exception data,
and `_lock(n)` bootstraps a missing lock by calling `_mtinitlocknum(n)`, which
takes lock 10 (`_LOCKTAB_LOCK`) to do it. In a real process `_mtinit()` filled that
entry during CRT startup, so the recursion terminates immediately. Nothing here has
run the game's CRT startup, so `_locktable` at `0x81a340` is all zeroes.

That is a useful result: the next dependency for running real game code is the
game's *own CRT initialisation*, not anything in the Win32 layer. The
reproduction is kept, off by default, as `NFSU2_SEH_CXX_PROBE=1` in the
game-functions selftest.

### The renderer scope: one function in, and what it cost

`../DIRECTX_SCOPE.md` lists 99 D3D9-touching functions as the port's starting
scope. They are all shaped alike - game globals plus a call through a device
pointer held in a global - so the first one in is the interesting one.
`FUN_005b7a30` (a `SetRenderState` caller) is now ported and verified against the
original. Three mechanisms had to exist first.

**Globals bound to the image.** Ghidra names untyped data `DAT_00873370`, and the
name carries the address - so *where* the storage is was never in doubt, and for a
hybrid port it must be exactly there, since original and ported code have to share
it. What the name does not carry is the access width, or whether it is integer or
float, and both matter: a 4-byte store where the original stored 1 byte overwrites
three neighbours, and `float` assigned through a `uint32_t` lvalue converts instead
of reinterpreting. The instruction encoding says both, unambiguously
(`mov DWORD PTR ds:0x873370,ecx`), so `tools/image_globals.ts` disassembles the one
function that needs it and reads the answer off the operands. Where the binary is
ambiguous - the same address touched at two widths, which is exactly what Ghidra's
`_DAT_` prefix warns about - it refuses rather than choosing. A type no instruction
can express (a pointer to an interface, say) goes in the manifest entry's
`globals:` map, which is one line and reviewable.

**The D3D9 ABI, twice.** The differential test builds a fake device - one vtable
entry, no GPU, no DXVK - and it needs *two* of them. MSVC compiled the original
against Windows' COM ABI, where methods are `__stdcall` and the callee pops; the
ported copy is compiled against DXVK Native's headers, where `STDMETHODCALLTYPE`
is empty and methods are `__cdecl`. Both are right for their own side and they
cannot share one vtable. That is not a test artefact: it means ported renderer code
calls DXVK directly and correctly, while any *original* renderer code still in the
process needs a stdcall-to-cdecl shim in front of the device.

**A decompiler bug, found the only way it could be - and a wrong diagnosis of it.**
A second function, `FUN_005bc7b0`, failed its differential test on the second of its
two `SetRenderState` calls. The ground truth, measured by calling the original
machine code with distinguishable arguments against a `__stdcall` fake device:

```
SetRenderState(state=0x0f, value=0xaaaa0001)   <- param_1
SetRenderState(state=0x18, value=0xbbbb0002)   <- param_2
```

Ghidra emits `param_1` for both. The second call reads `[esp+8]` *after* the first
call, so it depends entirely on how many bytes the decompiler thinks that call
purged, and it is getting that wrong by one 4-byte slot.

The first diagnosis here was that `analysis/define_directx_types.py` declared every
vtable slot `__stdcall` but *with no parameters*, leaving the decompiler unable to
know the purge size. That was a real defect and it is fixed - the counts now come
from the SDK headers rather than from memory (`analysis/derive_vtable_args.py`,
whose `--check` verifies the parse against the method order already in
`directx_vtables.py`: 119 methods for `IDirect3DDevice9`, 17 for `IDirect3D9`, 23
and 30 for the DirectDraw pair, exact). It measurably improved the output: 58
renderer functions changed on re-decompile, and `FUN_005ba070` now recovers a second
parameter it had missed entirely.

**It did not fix this bug.** With the declaration exactly as the SDK describes it -
`int __stdcall SetRenderState(void *this, int, int)`, verified by reading the type
back out of the project - the second call still says `param_1`. Adding one more
declared parameter shifts the reading by exactly one slot, so the purge does track
the declared size; it is simply 4 bytes short, as though `this` were not on the
stack. None of the four plausible declarations fixes it (`__stdcall`/`__thiscall`,
with and without an explicit `this`): the SDK-accurate one renders the arguments
correctly and mistracks the stack, and the others mistrack the arguments too. The
cause is not identified.

So the consequence stands, and is now firmer rather than resolved: **a decompiled
renderer function that reads an argument after a vtable call may be off by one
slot.** `FUN_005bc7b0` stays in the excluded list with the measurement above.
Differential testing against the original machine code is not belt-and-braces here -
it is the only thing that catches this, and it caught it on the second function
anyone tried.

#### Game code on a real device

The fake device proves the port is faithful; it does not prove the call survives
contact with a driver. `nfsu2-game-render` does that, in a window, on the GPU:

```
$ ./build32/nfsu2-game-render --exe /path/to/speed2.exe --frames 600
mapped     : .../speed2.exe
             unresolved: DSOUND.dll!#1 (ordinal)
imports    : 250 of 251 resolved (21 by ordinal)

# the game's own renderer code, on a real device
ok - SetRenderState is vtable offset 0xe4, the offset the original calls
ok - the PORTED FUN_005b7a30 returned 1
ok - and DXVK now reports ZWRITEENABLE=0 - the game's code reached the driver
ok - with its globals written in the mapped image (0x00002000, 0x00002001)
ok - the ORIGINAL machine code called our __stdcall shim 1 time(s)
ok - and DXVK reports ZWRITEENABLE=0 again - original code drove Vulkan
ok - the original returned 1, as the ported copy did
```

`GetRenderState` is what makes those checks worth anything: the state is read back
*out of DXVK* rather than out of a recording thunk, so the assertion is that the
driver agrees, not that the call was made.

The original machine code cannot be handed DXVK's device pointer - it pushes
arguments a `__cdecl` callee never removes, and the stack would drift 12 bytes per
call. It gets a shim object: our own vtable of `__stdcall` thunks forwarding to the
real device. That is what a partially-ported binary needs in front of *every*
interface it still reaches from original code, and this is the smallest working
instance of it. One method is filled in; the rest are NULL, so the first call to an
unshimmed method is a crash rather than a silent wrong answer.

Also worth the line it costs: with DXVK and the SDL-backed dinput8/ddraw both
linked, **the only unresolved import left in the whole exe is `DSOUND.dll!#1`** -
audio, the one genuine platform gap.

### What comes next

1. Widen the manifest into the renderer scope, one function at a time rather than
   in bulk, because of the purge bug above: a function whose arguments are all read
   before its first vtable call is safe, and one that reads an argument afterwards
   needs its decompilation checked against the disassembly by hand. The
   differential test is what decides, and it is cheap to add per function now that
   the harness exists.
2. Or find the purge bug. A ten-line reproduction is in the repo history of this
   section; the next thing to try is Ghidra's own compiler-spec model for
   `__stdcall` purges on indirect calls, rather than the data-type declaration.
3. Real SEH, now that the chain is reachable: `RaiseException`/`RtlUnwind` still
   abort rather than walking it (gap 2 above). The TEB was the prerequisite.
4. A TEB per thread as soon as the port creates threads - `nfsu2_teb_install()` is
   per-thread by construction, so `CreateThread` should call it on the new thread.
