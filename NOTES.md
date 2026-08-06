# Findings

Working notes from reverse-engineering NFS Underground 2's (2004, French
retail release) SafeDisc protection. No game files or binaries are stored
here - addresses/offsets below are specific to that build and will differ
for other releases/languages.

## Protection structure

- `speed2.exe` ships with two extra PE sections, `stxt774` and `stxt371`,
  containing an encrypted/obfuscated SafeDisc stub. The real `.text` is
  encrypted at rest (verified: disassembling it statically produces
  nonsensical opcode soup, not real code).
- The PE entry point points into `stxt371`, not `.text`. The stub:
  1. Resolves a small set of KERNEL32 functions itself (via
     `GetModuleHandleA`/`GetProcAddress`, not the normal import table -
     the import table only lists ~16 decoy functions across the DLLs the
     real game eventually needs, presumably so those DLLs get loaded).
  2. Self-extracts several files into a temp directory (`~e5.NNNN.dir.NNNN\`)
     by reading chunks out of its own exe file - `PfdRun.pfd`, a couple of
     `.tmp` files, one of which is a genuine ~800KB PE DLL.
  3. Loads that DLL for real and runs its `DllMain`.
  4. That DLL does the actual verification. Deep inside (a ~4300-instruction
     routine, decompiled address `0x10054b20` in our emulation's address
     space) it computes some value and compares it against a hardcoded
     32-bit constant (`0x410a02bc` in this build) at approximately
     `0x100134c4`.
  5. If that comparison passes, control returns up through several stack
     frames to the stub, which jumps to the real OEP (`0x75b8d1` in this
     build, `.text`+`0x1b8d1`).

## The checksum is very likely key material, not just a gate

Forcing the `0x100134c4` comparison to "pass" (by directly overwriting the
compared memory with the expected constant) does make the stub take the
success path and jump to the real OEP - but `.text` is still garbage at
that point. Byte-diffing our emulator's `.text` against a real
unwrapped/decrypted reference exe showed 99.6% of bytes differ, with no
repeating pattern (ruling out a trivial XOR obfuscation - this is a real
cipher with proper diffusion).

Conclusion: the checksum computed at `0x100134c4` almost certainly doubles
as (or derives) the decryption key for `.text`, not just a boolean
pass/fail flag. That's consistent with why forcing the comparison gets
you *past the check* but not to *working decrypted code* - and why a
"crack" for this class of protection is realistically produced by running
the real exe with a real disc under a debugger and dumping memory after
genuine hardware-driven decryption completes, then rebuilding a standalone
PE from that - not by patching the comparison.

**We have not solved the actual algorithm.** Standard checksums (CRC32,
Adler32, byte/dword sum, MD5/SHA1-truncated) do not match, confirming it's
a bespoke routine. This is the open thread if anyone wants to pick it back
up: fully reverse `0x10054b20` (and whatever it calls) to determine exactly
what it hashes and whether the input is derivable without genuine disc
hardware, or whether it depends on raw/weak-sector data a plain `.iso`
structurally can't contain.

## What `tools/unwrap.py` does and doesn't do

Given the above, `unwrap.py` does not decrypt anything itself. It performs
the PE-level transformation only:

- drops the `stxt*` stub sections
- rewires `AddressOfEntryPoint` to the real OEP (found via
  `analysis/emulate_stub.py`, independent of any reference file)
- recomputes `SizeOfImage`/section count
- repoints the Import Directory
- sources `.text`/`.rdata`/`.data` *content* from a reference exe whose
  protection has already been removed (matched by identical raw
  offset/size per section - if a reference's section layout doesn't match
  the original's, the tool refuses rather than silently producing garbage)

This reproduces "no-disc-needed" functionality using a known-good
reference for the two payloads we haven't independently derived
(decrypted `.text`, rebuilt import table) - feature-equivalent, not
byte-identical, and fully inspectable/understood for every other part of
the transformation.

## Native Linux ELF port: ABI findings

Branch `native-elf-dxvk`. Goal: stop shipping a patched PE that needs Wine, and
instead build a native i386 Linux ELF, using Wine's *headers* for Win32/D3D9
declarations and DXVK Native for D3D9 (until the renderer is ours - see
`DIRECTX_SCOPE.md`). Scaffolding, tests and the full rationale live in
`native/README.md`; the findings that took measurement rather than reading are
here.

1. **Winelib is not an option on this system.** Wine 11 here is new-WoW64:
   `/usr/lib/wine` has `i386-windows`, `x86_64-unix`, `x86_64-windows` but no
   `i386-unix`, so `winegcc -m32` cannot link anything (`cannot find
   -lkernel32 / -lwinecrt0 / -lntdll`). That rules out the "ELF that links
   against libwine" shape and leaves headers-only + native gcc.
2. **Wine's headers compile natively at -m32 and -m64.** `<windows.h>` and
   `<d3d9.h>` from `/usr/include/wine/windows` build cleanly with plain gcc
   against glibc, in the same TU as `<stdio.h>`, `<math.h>` and
   `<SDL2/SDL.h>`. No header fork needed.
3. **DXVK Native's COM ABI is cdecl, Wine's is stdcall.** DXVK's
   `include/native/windows/windows_base.h` defines `STDMETHODCALLTYPE`,
   `__stdcall` and `WINAPI` as *empty*, so on i386 its vtable slots are cdecl
   (caller pops). Wine's `minwindef.h:157` makes `WINAPI` =
   `__attribute__((stdcall)) __attribute__((force_align_arg_pointer))`.
   Confirmed by generated asm for `dev->lpVtbl->SetViewport(dev, vp)` at
   `gcc -m32 -O1 -S`: `addl $20, %esp` after the call with Wine's macros
   (callee popped the 8 bytes), `addl $28, %esp` with them neutralised (caller
   popped). Mixing the two would corrupt the stack by 8 bytes per D3D9 call.
   `native/include/nfsu2/d3d9_native.h` neutralises the two macros for the
   d3d9 includes only and restores them after; no thunk layer and no DXVK
   patch. Everything else stays stdcall deliberately, so ported code calls
   Win32 exactly as the original MSVC code did (and so hybrid execution of
   not-yet-ported machine code stays possible).
   Note the `force_align_arg_pointer` in Wine's definition - that is the same
   16-byte stack alignment issue the letterbox patch hit live (item 5 below),
   handled for us on this path.
4. **Wine headers hide their prototypes by default.** `WINBASEAPI` expands to
   `DECLSPEC_IMPORT`, which under GCC on a non-PE target is
   `__attribute__((visibility("hidden")))` (`winnt.h:58`). A shim built that
   way links, but its symbols are absent from the dynamic symbol table, so
   `GetProcAddress` (implemented over `dlsym(RTLD_DEFAULT, ...)`) silently
   finds nothing. Defining `_KERNEL32_` / `_USER32_` / `_ADVAPI32_` /
   `_WINMM_` before including any Wine header - the same mechanism Wine uses
   for its own DLL builds - switches them to default visibility.
5. **`DWORD` != `UINT` at -m32.** On i386 Wine's `DWORD` is `unsigned long`
   and `UINT` is `unsigned int`: same width, different types, so a shim
   signature that disagrees with the header is a hard error at 32-bit while
   compiling silently at 64-bit (e.g. `ExitProcess(DWORD)` vs
   `TerminateProcess(HANDLE, UINT)`). Any shim work should be compiled at
   -m32 before being believed.
6. **Struct layouts already agree.** 47 D3D9 struct sizes, field offsets and
   enum constants compared between Wine's headers and DXVK Native's own
   headers match exactly at both 32- and 64-bit
   (`native/tests/abi_probe_*.c`, run by `meson test`). So Wine's headers can
   be used as the type source with no adaptation beyond the convention fix.
7. **DXVK Native picks its own window-system backend.** It builds every WSI
   backend it finds (SDL3, SDL2, GLFW) and chooses one at load time. If that
   choice differs from the toolkit that owns the window, `Direct3DCreate9`
   throws an uncaught `dxvk::DxvkError` with no message - which is what a
   first run looks like. `DXVK_WSI_DRIVER=SDL2` fixes it; the smoke host sets
   it via `setenv(..., 0)` so the environment can still override.

8. **HWND has to be an `SDL_Window*`.** DXVK casts
   `D3DPRESENT_PARAMETERS.hDeviceWindow` straight to its backend window
   pointer, and the game gets that HWND from `CreateWindowExA`. So the user32
   shim cannot use a handle type of its own - it makes HWND *be* the
   SDL_Window and hangs per-window state off `SDL_SetWindowData`. Anything
   else crashes inside DXVK's WSI layer rather than in our code, which would be
   an unpleasant thing to debug.
9. **Map keys by scancode, not keycode.** The game hardcodes VK_W/VK_A/VK_S/
   VK_D, meaning the physical cluster on the left of the keyboard. Translating
   SDL keycodes would faithfully deliver VK_Z/VK_Q on the AZERTY layout this
   French build shipped for - correct by the letter and useless in practice.
   Characters are separate: they arrive as WM_CHAR from SDL_TEXTINPUT, which is
   where the platform does layout and dead-key composition.
10. **Two Wine header quirks that only bite at one width.** `CreateWindowA` is
   a macro over `CreateWindowExA` (winuser.h:4005), so defining it as a
   function is a syntax error; and the legacy `GWL_USERDATA`/`GWL_WNDPROC`/
   `GWL_HINSTANCE` names are absent at 64-bit (Win64 dropped them for the
   pointer-sized `GWLP_*`) while `GWL_ID` is kept. `win32_compat.h` aliases the
   missing ones individually.

Verified end to end at the real target: an **i386** native ELF (no Wine loader)
presenting 300 frames at 2560x1080 through a 32-bit DXVK 3.0.2 build -> Vulkan
on an RTX 2060 SUPER, adapter identifier read back correctly through the D3D9
vtable. That last part is what makes the convention finding above more than a
reading of the headers: a stdcall/cdecl mismatch could not survive 300 frames of
vtable dispatch. The 64-bit build does the same, and all four test suites pass at
-m32 and -m64 with no warnings.

The user32 shim closes the loop on that: `nfsu2-smoke-game-loop` registers a
class, creates a window through our own `CreateWindowExA`, hands that HWND to
`CreateDevice`, and runs a `PeekMessageA`/`DispatchMessageA` frame loop without
touching SDL anywhere - the same sequence the game's own startup performs -
presenting 200 frames at 2560x1080 with WM_CREATE, WM_ACTIVATEAPP and real
keyboard input arriving at a stdcall WNDPROC.

## Widescreen/ultrawide FOV patch

Goal: correct field-of-view for arbitrary aspect ratios, baked into the
binary. The game's in-game video-options resolution setting is a red
herring for this - it only affects the D3D backbuffer size and two
UI/overlay camera slots, not the actual driving camera's FOV.

Found by live-tracing (gdb, breaking on `FUN_005bda20` at `0x5bda20`,
which dispatches `SetTransform` for `D3DTS_VIEW`/`D3DTS_PROJECTION` per
camera slot each frame) which camera slot is the real gameplay camera
during an actual race: slot index 4 (the argument to `FUN_005bda20`,
`FUN_0048b140`, etc. throughout this camera subsystem - a small array at
`DAT_00832de0`, stride `0x70`, base + `index*0x70` per slot).

The projection matrix for a slot is built by `FUN_005c7ac0` (`0x5c7ac0`),
called once per active slot per frame from the main render function
`FUN_005d24a0`. It reads a small resolution-reference struct via
`*(camera_struct + 0x58)`, and uses `[struct+0x14]` (width) /
`[struct+0x18]` (height) - specifically `height/width`, folded through a
BAM-style angle/`sin`/`cos`/`cot` pipeline - to compute both diagonal
scale terms of the projection matrix (`m11` at `matrix+0x40`, `m22` at
`matrix+0x54`; the two terms are chained, `m11`'s angle is derived using
`sin()` of `m22`'s angle, so both depend on this ratio, not just one).

That resolution-reference struct is populated by `FUN_005b9ea0`
(`0x5b9ea0`), called once per frame, unconditionally, looping over 10
small camera-context slots (its own local index 0-9, a *different*
indexing than the `DAT_00832de0` array - correlated by address, not by
value) and hardcoding a literal resolution per slot. Slot 0 uses live
globals (`DAT_00870980`/`DAT_00870984`, the same ones the D3D backbuffer
size gets set from - this is why UI/overlay camera slots 0/1 already
"support" arbitrary resolutions with no patch needed). Slot 1 - which
resolves to the same struct as world-camera array index 4 - hardcodes a
literal `320x240`. That literal is the actual widescreen bug: the real
driving camera always computes FOV as if rendering at a fixed 4:3
reference regardless of the real output resolution.

There's a second, similar-looking write in `FUN_005c1de0` (`0x5c1de0`,
also called every frame from `FUN_005d24a0`) that *also* hardcodes
`320`/`240` into what looks like the same struct field - but only inside
a branch gated by `DAT_008707e0 == 1` or `== 2`. Patching only this site
was tried first and produced **zero** change in the live projection
matrix (confirmed via a watchpoint on the matrix memory, bit-identical
`xScale` before/after across multiple runs) - that flag apparently never
takes either value during normal single-player racing, making this
write dead code in practice. `FUN_005b9ea0`'s unconditional write is the
one that actually matters; patching it alone was independently confirmed
(direct memory read of the resolution struct, no live race needed, since
this write isn't gated on any game state) and then visually confirmed in
an actual race (correct full-width, non-stretched FOV).

`tools/patch_widescreen.py` patches both `FUN_005bf210` (the display
resolution table feeding the backbuffer/UI cameras - needed too, or the
3D view stretches to fill the corrected-but-not-FOV-adjusted backbuffer)
and `FUN_005b9ea0`'s hardcoded `320x240` (recomputed as
`height=240, width=round(240 * target_width/target_height)` - i.e. a
standard "Hor+" fix: vertical FOV stays exactly what the original 4:3
design intended, horizontal FOV widens to match the real aspect ratio).
It deliberately does not touch `FUN_005c1de0`'s dead-in-practice write.

All addresses above are specific to the 2004 French retail build (same
build `tools/unwrap.py`'s example `--entry-rva` applies to).

## 2D UI letterboxing (menu/HUD/video aspect correction)

Goal, distinct from the FOV patch above: the 3D world camera renders
correctly across the full widened backbuffer (that's what the FOV fix is
for), but the game's 2D UI system (menu, HUD, splash video) has *no*
aspect-ratio logic at all - it was written once, in 2004, for 4:3, and
every quad it draws just fills whatever viewport is active with a plain
0..1 normalized rect. Widening the backbuffer for the 3D fix directly
widens what these quads stretch into. `tools/patch_ui_scale.py` (kept in
the repo, not the active fix) was tried first: the UI coordinate system's
X scale is a static, compile-time `2/640` value, and adjusting it alone
seemed like the obvious fix. It doesn't work - the underlying formula is
`x_ndc = px*scaleX - 1.0`, which pins pixel-0 to the screen's left edge
unconditionally regardless of scaleX, so scaling alone shrinks the
*right* edge inward instead of centering. True centering needs the
viewport itself narrowed to a centered 4:3 rect - there's no existing
"shrink the viewport for 2D" call anywhere in the game to patch, so
`tools/patch_letterbox_2d.py` / `patches/letterbox_2d.s` inject new code
into a confirmed-empty 2319-byte padding gap at the end of `.text`
instead.

The functions that needed hooking were found the same way as the FOV fix
- live breakpoint sweeps, not guessing from static decompilation (Ghidra
mistyped several DirectX vtable pointers along the way, e.g. propagating
`IDirect3DDevice9*` onto an unrelated engine object's parameter -
`analysis/retype_and_decompile.py` is a small reusable fix for that class
of bug). Sweeping every direct-DirectX-calling function while sitting on
the static title screen found `FUN_005c4c20` (the general-purpose "draw
one UI element" function) overwhelmingly the most active - by far,
~1900 calls in a single menu frame - with two more, `FUN_005c5b70` and
`FUN_005c6d40`, specific to video playback.

The mechanism (three hooks narrow the viewport before each of those
draws; two more hooks widen it and clear the two resulting bar regions to
black, once per frame and once after a device Reset()) sounds simple, but
getting a genuinely artifact-free result took a long sequence of
confirmed-live wrong turns, each fixing something real before exposing
the next issue:

1. **Null device pointer.** The very first version crashed intermittently
   - the hooked functions can run before device/backbuffer globals are
   initialized. Fixed with a null/zero guard before touching anything.
2. **`cdq` clobbering the device pointer.** `cdq` (needed for the
   `height*4/3` signed division) unconditionally sign-extends `eax` into
   `edx` - and `edx` was where the device pointer lived. Since `height*4`
   is always positive, this silently zeroed it, causing a *second*,
   differently-shaped crash after the first fix.
3. **EDX not preserved across real D3D9 calls.** Once the code held the
   device pointer in `edx` across multiple `SetViewport`/`Clear` calls,
   it crashed again - because per the stdcall convention, `SetViewport`/
   `Clear` (real D3D9 methods, not our code) are free to clobber
   `eax`/`ecx`/`edx`, same as any other stdcall callee. Fixed by
   reloading the device pointer from memory immediately before each
   vtable dispatch instead of trusting a register to survive the call.
4. **Doing everything (widen, clear bars, narrow) on every 2D-quad draw
   call** - including the ~1900-calls/frame function - caused two
   separate bugs: 3D world geometry inheriting a leftover narrow viewport
   (since the hooks never regained control to restore it, and 3D content
   doesn't re-set the viewport itself), and garbled bar-region content
   from doing the clear that often. Fixed by splitting into a cheap
   per-draw "narrow only" routine and a once-per-frame "widen + clear"
   routine.
5. **A persistent torn/garbled artifact in the bar regions**, surviving
   the fix above, turned into the longest part of this investigation.
   Eight independent, live-verified fix attempts aimed at the bar-clear
   itself all failed to change it at all: confirming the D3DRECT values
   passed to `Clear()` were byte-for-byte correct against a live
   2560x1080 backbuffer; confirming the render target was always already
   correct (2000/2000 live samples); forcing scissor test off; preserving
   `xmm0`-`xmm7` around the injected routines (real D3D9 methods are free
   to clobber them, same as any volatile register, and this game keeps
   live matrix/vector state in them between operations); a heuristic to
   exempt "full-width" quads from letterboxing (wrong - it also exempted
   real full-screen content like video and the menu background,
   regressing them to stretched); and forcing 16-byte stack alignment
   before every D3D9 call (a real, live-confirmed misalignment ~19% of
   the time, but not the cause of this particular artifact). Removing the
   bar-clear entirely and confirming the artifact persisted regardless
   was the deciding clue: the bug was never in what the clear did, it was
   in state `set_narrow_viewport` left behind. The actual fix was
   structural: the "leave the viewport narrow until next frame" design
   assumed nothing else draws between two hooked calls in the same frame
   - an assumption that doesn't hold (this game's own bloom/post-process
   pass, or DXVK/wined3d's own internal state, can run in between). The
   three per-element hooks now use a return-address-swap trampoline to
   restore the wide viewport immediately after each individual draw call
   returns, scoping the narrow viewport to the minimum possible window.
6. **`.text` isn't writable.** The return-address-swap trampoline needs a
   few bytes of persistent scratch memory (to stash the real return
   address across the original function's body). Placed inside the same
   `.text` code cave as everything else, it crashed immediately - PE
   `.text` sections are CODE|EXECUTE|READ by default, not writable, and
   the very first write faulted with a genuine page-protection SIGSEGV.
   `patch_letterbox_2d.py` now flips on `IMAGE_SCN_MEM_WRITE` for `.text`
   as part of applying the patch.

Live-verified stable across the main menu, HUD, video playback, and
active racing, including through compositor-triggered redraws (e.g. a
workspace switch, which can force a device-lost/Reset() cycle).

Debugging methodology worth keeping for future patches on this game:
GDB attached directly as the *parent* process (`gdb -batch -x script --
wine speed2.exe`, not `gdb -p <pid>`) was required throughout, since this
system's `ptrace_scope=1` blocks attaching to an already-running,
non-child process. Raw-address breakpoints can't be set before the PE
module loads (`gdb` errors with "Cannot access memory"), so scripts
started with a bare `run speed2.exe` (no breakpoints), and an external
`kill -INT <gdb-pid>` a few seconds later interrupted execution -
equivalent to pressing Ctrl-C - at which point later `break`/`continue`
commands in the same batch script worked normally. Camera/RenderDoc-based
GPU frame capture was investigated as an alternative but doesn't work in
this environment (RenderDoc's injection fails even for a plain native
Vulkan test app, unrelated to Wine) - live GDB sampling (breakpoint hit
counts, parameter/memory dumps across hundreds to thousands of calls) was
the only tool that consistently worked here.
