# Handoff: `native-elf-dxvk`

Where the native Linux port stands, what to run, and what to do next. The full
rationale for every piece lives in `native/README.md`; this is the short version for
picking the work back up.

## The one-line state

**The game boots.** `speed2.exe` runs as native i386 Linux code: its CRT starts, it
reads its registry, passes its disc check, loads its shaders out of its own PE
resources, brings up a fullscreen 2560x1080 DXVK swapchain, creates 519 D3D9 objects
(shaders, textures, vertex and index buffers, state blocks), prints its own log line -
and then dies of heap corruption that has not been found yet.

No window with gameplay in it. Everything up to the point of drawing works.

## Run it

```sh
cd native
meson setup build32 --cross-file cross/linux32.txt -Dnfsu2_exe="<path>/speed2.exe"
ninja -C build32
meson test -C build32                 # 8 suites, must be green with zero diagnostics

# the game itself
NFSU2_SHIM_TRACE=1 NFSU2_DRIVE_J=cdrom:<dir with BIN.DAT> \
  ./build32/nfsu2-game-launch --no-fork --exe "<path>/speed2.exe"
```

Three things that are not optional:

- **`NFSU2_SHIM_TRACE=1`.** Without it the interesting traces are silent, and a run
  looks like it got nowhere when it got all the way. This cost real confusion; see
  the correction in `native/README.md`.
- **`NFSU2_DRIVE_J=cdrom:<dir>`** where the directory holds `BIN.DAT` from disc 2.
  The disc check is `GetDriveTypeA("J:\") == DRIVE_CDROM` plus
  `CreateFileA("J:\bin.dat")`; `J:` comes from the game's own registry. `BIN.DAT` is
  130 bytes and can be read straight out of an ISO without mounting it.
- **A registry store.** `deno run -A native/tools/import_wine_registry.ts --prefix
  <wine prefix> --out "<game dir>/registry.ini"` copies the game's own keys - its
  registration code, its video settings, the `CD Drive` value. That file stays in the
  game directory and must never be committed.

`--no-fork` runs the game in-process; without it the parent survives to report how the
child died, which is usually what you want when it crashes early.

## The open problem

Heap corruption. glibc reports `malloc(): invalid size` / `unsorted double linked list
corrupted` from a worker thread, and the faulting address moves between runs while the
*progress* does not (519 objects wrapped, 23 resources found, every run).

Four things are eliminated **by instrumentation, not argument**. All four checks are
permanent, and all four stay silent:

| ruled out | by | where |
| --- | --- | --- |
| a small write past a block we allocated | canary in the slack between the requested size and `malloc_usable_size` | `src/win32/heap.c` |
| a lock request larger than its buffer | `GetDesc` audit in front of every buffer `Lock` | `src/d3d9_bridge/bridge.c` |
| a write past a locked buffer *region* | `NFSU2_D3D9_GUARD_LOCKS=1` - the game gets our memory with an unmapped page after it, copied back at `Unlock` | `src/d3d9_bridge/bridge.c` |
| `GetDeviceState` overflowing the caller's buffer | read: keyboard, mouse and joystick all clamp to the caller's size | `src/dinput8/device.c` |

The third is worth knowing about as a *tool*: with `NFSU2_D3D9_GUARD_LOCKS=1`, a write
one byte past a locked buffer faults at the instruction that makes it, instead of
surfacing as a corrupted arena in an unrelated thread later. It found nothing on
vertex and index buffers; it has not been extended to `LockRect`.

What is left, in the order worth trying:

1. **`LockRect` and pitch.** A game that computes its own row stride instead of using
   the returned pitch writes past the last row. The guard-lock machinery above is the
   thing to extend - `LockRect` needs `pitch * height` from the surface descriptor
   rather than a buffer size, and then it is the same trick.
2. **Something other than a heap overrun**: a write into memory that has already been
   freed, or a double free. Neither a canary nor a guard page sees those. glibc's
   `MALLOC_PERTURB_` is the cheap probe.
3. **Our own shim writing past some *other* game-supplied struct.** `GetDeviceState`
   was the best candidate and is clean, but every `Get*` that fills a caller's buffer
   deserves the same read. Note that `abi-layout-match` compares Wine against *DXVK* -
   never against the 2004 SDK the game was built with, which is where a size
   difference would come from.

Also open, and possibly related: `IDirect3DDevice9::BeginStateBlock` fails repeatedly
with `D3DERR_INVALIDCALL`, which is what DXVK returns when a state block is already
recording.

## What was built, and the load-bearing bits

- **Win32 shim** over POSIX/SDL2 - 251 of 251 imports resolve. Audio answers
  `DSERR_NODRIVER` honestly; everything else is implemented.
- **PE loader** (`src/loader/pe_loader.c`) maps the exe at its own base, headers
  included (an `HMODULE` *is* the image base, and the CRT startup reads `MZ` through
  it), resolves the IAT, and points unresolved imports at stubs that name themselves
  when called.
- **A TEB in `%fs`** (`src/win32/teb.c`), because `fs:[0]` is the SEH chain and every
  `__try` touches it. i386 only, and cheap *because* i386 glibc keeps its thread
  pointer in `%gs`.
- **SEH** (`src/win32/exception.c`): the dispatcher, `RaiseException`, `RtlUnwind`,
  and signals translated into `EXCEPTION_RECORD`s. The game's own handlers run.
- **The D3D9 bridge** (`src/d3d9_bridge/`, generated by
  `tools/generate_d3d9_bridge.ts`): 274 `__stdcall` + `force_align_arg_pointer` thunks
  in front of DXVK Native's `__cdecl` vtables, with COM pointers translated both ways.
  Both halves are mandatory - MSVC does not keep the stack 16-byte aligned and DXVK
  is full of `movaps`.
- **Ported game functions** (`native/game/manifest.yaml` plus
  `tools/import_decompiled.ts`): 10 functions, each verified twice - against a
  reference and against the original machine code with identical inputs, 283
  comparisons. The decompiled code and the generated ports are never committed.

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

## Next steps, in the order I would take them

1. Find the corruption - hypothesis 2 above is the cheapest test, hypothesis 1 the
   most likely to be decisive.
2. Fix `BeginStateBlock`, which may fall out of the same cause.
3. Re-export the renderer scope from Ghidra now that the vtable method definitions
   carry argument counts, then widen the manifest into `DIRECTX_SCOPE.md`'s 99
   functions - one at a time, differential test each.
4. Real audio, when something needs it. `src/dsound/dsound.c` is where it goes.
