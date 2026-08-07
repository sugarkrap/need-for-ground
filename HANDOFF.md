# Handoff: `native-elf-dxvk`

Where the native Linux port stands, what to run, and what to do next. The full
rationale for every piece lives in `native/README.md`; this is the short version for
picking the work back up.

## The one-line state

**The game runs.** `speed2.exe` runs as native i386 Linux code: its CRT starts, it
reads its registry, passes its disc check, loads its shaders out of its own PE
resources, brings up a fullscreen DXVK swapchain, loads its data files and its
tracks, plays its intro, reaches its menus, and takes keyboard input.

It is playable enough to find gameplay bugs in, which is where the open problems now
are - not in getting it to start.

## Run it

```sh
cd native
meson setup build32 --cross-file cross/linux32.txt -Dnfsu2_exe="<path>/speed2.exe"
ninja -C build32
meson test -C build32                 # 8 suites, must be green with zero diagnostics

# the game itself
NFSU2_DRIVE_J=cdrom:<dir with BIN.DAT> \
  ./build32/nfsu2-game-launch --no-fork --exe "<path>/speed2.exe"
```

Two things that are not optional:

- **`NFSU2_DRIVE_J=cdrom:<dir>`** where the directory holds `BIN.DAT` from disc 2.
  The disc check is `GetDriveTypeA("J:\") == DRIVE_CDROM` plus
  `CreateFileA("J:\bin.dat")`; `J:` comes from the game's own registry. `BIN.DAT` is
  130 bytes and can be read straight out of an ISO without mounting it.
- **A registry store.** `deno run -A native/tools/import_wine_registry.ts --prefix
  <wine prefix> --out "<game dir>/registry.ini"` copies the game's own keys - its
  registration code, its video settings, the `CD Drive` value. That file stays in the
  game directory and must never be committed.

`NFSU2_SHIM_TRACE=1` logs every interesting Win32 call and is how nearly every bug
here has been found. `--no-fork` runs the game in-process; without it the parent
survives to report how the child died.

Diagnostics that are off by default: `NFSU2_D3D9_GUARD_LOCKS=1` (a locked vertex or
index buffer gets our memory with an unmapped page after it, so an overrun faults at
the instruction), `NFSU2_D3D9_TRACE_STATE_BLOCKS=1` (every Begin/EndStateBlock).

## Open problems

1. **The rev gauge does not work** in a race. Not diagnosed. Note that the
   `BeginStateBlock` failures in a trace are *not* the cause - see below - and that a
   trace of a run that reaches a race is the missing evidence, since the in-race HUD
   and its textures are never loaded by a run that only reaches the menus.
2. **Audio is not implemented.** `DirectSoundCreate` answers `DSERR_NODRIVER`, which
   is the honest answer for a machine with no sound card and a path the game handles.
   `src/dsound/dsound.c` is where the real thing goes.
3. **Two of the 53 mapped actions do not bind**: the semantics for "Debug Camera
   Turbo" and "Debug Camera Super Turbo" name DIK codes `0xea` and `0xe5`, which are
   not in `dik_map.c`. Debug controls, reported as unmapped rather than mis-bound.
4. **Saving needs confirming.** "Unable to save NAME." was the host-path round trip
   (fixed in `2a4a36c`); that it now works has not been verified by anyone.

## What was solved, and what it taught

The blocker in the previous handoff - heap corruption from a worker thread, four
classes of cause eliminated by instrumentation and none of them it - was one bug, and
it was not in the heap at all.

`GetCurrentDirectoryA` returned a host path (and, because `game_launch` never called
`nfsu2_win32_init`, latterly just `"."`). The game's file layer is a virtual
filesystem with one device per drive letter from `GetLogicalDrives`, and it picks the
device from the prefix up to the `:` of its search path - which is whatever
`GetCurrentDirectoryA` returns. A path naming no device fell back to the abstract
base device, whose open method is literally `or eax,-1; ret 0xc`. So **no Win32 file
call was ever made**, and a trace showed no file I/O, and it read as a game that had
not asked for its data.

It had. The game requests its memory files asynchronously into a buffer its allocator
fills with `0xee`, and the completion handler at `0x5793c0` relocates the header's
record array *without checking the failure flag it is handed*. With the buffer still
poison the record count read as `0x44443333`, and the handler wrote a pointer every
20 bytes across the heap until it ran off the end - corrupting glibc's arena on the
way and then faulting.

Four lessons worth keeping:

- **Trace successes, not just failures.** Every one of `CreateFileA`, `ReadFile`,
  `dinput` device creation and `dinput` reads traced only its error paths, so "the
  game never asked" and "the shim never said" were indistinguishable. Three separate
  bugs hid in that gap. They all trace successes now.
- **A comment asserting what other software does is a claim that can be wrong.**
  "Games that use action mapping fall back to explicit binding" - this one does not,
  and that sentence cost the keyboard.
- **Measure before concluding, then check the arithmetic.** I called the
  `BeginStateBlock` failures the likely cause of the gauge on the strength of every
  refusal reporting "1 recording already open". The Begin/End counts - 326 and 326 -
  say the recordings are balanced and the nesting is the game's own, ignored, and
  refused identically by Windows.
- **An overflow warning is a cause, not bookkeeping.** "d3d9 bridge table is full"
  appeared 5435 times and was the reason DXVK was being handed our own bridge
  pointers to call as its objects.

## What was built, and the load-bearing bits

- **Win32 shim** over POSIX/SDL2 - 251 of 251 imports resolve.
- **PE loader** (`src/loader/pe_loader.c`) maps the exe at its own base, headers
  included (an `HMODULE` *is* the image base, and the CRT startup reads `MZ` through
  it), resolves the IAT, and points unresolved imports at stubs that name themselves
  when called. It maps each section's full `SizeOfRawData`, not `VirtualSize`: this
  exe has a steering-wheel wrapper baked into it whose code lives in `.text`'s
  padding, and truncating there left a detour jumping into zeros.
- **A TEB in `%fs`** (`src/win32/teb.c`), because `fs:[0]` is the SEH chain and every
  `__try` touches it. i386 only, and cheap *because* i386 glibc keeps its thread
  pointer in `%gs`.
- **SEH** (`src/win32/exception.c`): the dispatcher, `RaiseException`, `RtlUnwind`,
  and signals translated into `EXCEPTION_RECORD`s. The game's own handlers run.
- **The D3D9 bridge** (`src/d3d9_bridge/`, generated by
  `tools/generate_d3d9_bridge.ts`): 274 `__stdcall` + `force_align_arg_pointer` thunks
  in front of DXVK Native's `__cdecl` vtables, with COM pointers translated both ways.
  Both halves are mandatory - MSVC does not keep the stack 16-byte aligned and DXVK
  is full of `movaps`. It also does what D3D9 does to the FPU at `CreateDevice`
  (single precision, round to nearest, all exceptions masked), without which the
  game's unmasked control word reaches DXVK's worker threads and the NVIDIA driver
  takes a SIGFPE.
- **DirectInput action mapping** (`src/dinput8/`), which is the only way this game
  reads its keyboard: it never creates a keyboard device. All 53 of its actions are
  `DIKEYBOARD_*` semantics, so the low byte is a DIK code and no genre default-mapping
  tables are needed.
- **Ported game functions** (`native/game/manifest.yaml` plus
  `tools/import_decompiled.ts`): 10 functions, each verified twice - against a
  reference and against the original machine code with identical inputs. The
  decompiled code and the generated ports are never committed.

## Traps, all of which cost time here

- **A weak undefined reference does not pull a member out of a static archive.** Use
  `link_whole`. This bit twice (the bridge, and `--as-needed` dropping DXVK).
- **The generated tree is globbed at configure time.** Adding a manifest entry needs
  `meson setup --reconfigure`.
- **The game persists state that changes its startup path** (`NotFirstTime`,
  `SwapSize`, `Language` in `registry.ini`). Re-import before any A/B comparison.
- **Ghidra's decompilation of a vtable-calling function is suspect after the first
  call.** `FUN_005bc7b0` passes `param_1` where the machine code passes `param_2`; the
  cause is not the argument counts (those are fixed now, from the SDK headers via
  `analysis/derive_vtable_args.py`) and remains unidentified. Differential testing is
  the only thing that catches it, so widen the manifest one function at a time.
- **Keep the Ghidra project somewhere durable.** The first one lived in `/tmp` and did
  not survive a reboot. It is at `~/ghidra-projects/NFSU2` now, with `pyghidra` in a
  venv beside it.
- **We hand the game paths in one alphabet and must accept them back in another.**
  Two bugs were this: the VFS search path, and `SHGetFolderPathA` returning a host
  path the game then appends `\NFS Underground 2\` to. `path.c` now splits on the
  separator - a leading `/` is a host path of ours, a leading `\` is Windows for
  "root of the current drive" and means the game root - and both are tested.

## Next steps, in the order I would take them

1. The rev gauge. Get a trace from a run that reaches a race (`NFSU2_SHIM_TRACE=1`,
   drive one), and check first whether the in-race HUD's own files
   (`GLOBAL/HUD_CustomTextures_*.bin`) are opened and what D3D9 calls fail.
2. Confirm saving works, now that the save directory resolves.
3. Audio, which is the largest remaining gap and the most visible one.
4. Re-export the renderer scope from Ghidra now that the vtable method definitions
   carry argument counts, then widen the manifest into `DIRECTX_SCOPE.md`'s 99
   functions - one at a time, differential test each.
