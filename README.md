# nfsu2-unwrap

Tools for analyzing and (currently) patching the SafeDisc protection wrapper
on Need for Speed Underground 2 (2004) executables, for legally-owned
copies that otherwise require a physical disc to run.

No game files, executables, or other copyrighted artifacts are stored in
this repository - see `.gitignore`. Everything here operates on files you
supply locally.

## Status

**Patching only, for now.** Given an original protected exe and a reference
exe that's already had its protection removed, `tools/unwrap.py` produces a
working, disc-free exe by performing the PE-level transformation itself
(section removal, entry point rewiring, import directory repointing) while
sourcing the still-encrypted content payloads from the reference. See
`NOTES.md` for exactly what's independently reverse-engineered vs. sourced
from a reference, and what's still open (the actual decryption/checksum
algorithm).

## Workflow

### 1. Find the real entry point (OEP)

```
python3 analysis/emulate_stub.py \
  --exe /path/to/protected/speed2.exe \
  --real-kernel32 /path/to/a/real/32-bit/kernel32.dll
```

This emulates the SafeDisc stub (with a faked-but-consistent Win32
environment - fake TEB/PEB, a working heap, TLS, file-mapping, and a real
GDT so `fs:`-prefixed TEB access actually works under Unicorn) far enough
to observe it self-extract its helper DLL, run that DLL's `DllMain`, and
jump to the real OEP once its checks resolve. It prints the OEP as both a
VA and an RVA - the RVA is what `unwrap.py --entry-rva` wants.

A real `kernel32.dll` (any genuine 32-bit PE build, e.g. from a Wine/Proton
`i386-windows` tree) is needed because some of the stub's code manually
walks kernel32's own export table instead of calling `GetProcAddress` -
mapping the real headers there (without ever executing real kernel32
internals - those get redirected through the same fake dispatcher) is what
lets that code path resolve correctly.

### 2. Patch

```
python3 tools/unwrap.py \
  --original /path/to/protected/speed2.exe \
  --reference /path/to/an/already-unwrapped/speed2.exe \
  --entry-rva 0x35b8d1 \
  --output speed2_unwrapped.exe
```

The reference exe must have `.text`/`.rdata`/`.data` sections at identical
raw file offsets/sizes to the original (true for exes built from the same
source, e.g. different distribution's protection wrapper around the same
underlying game build) - the tool checks this and refuses rather than
silently producing a broken file.

### 3. (optional) Deeper analysis with Ghidra

```
/opt/ghidra/support/analyzeHeadless /path/to/project_dir ProjectName \
  -import /path/to/unwrapped/speed2.exe -overwrite

python3 analysis/ghidra_query.py \
  --project-dir /path/to/project_dir --project-name ProjectName \
  --program speed2.exe --address 0x773262 --decompile
```

`ghidra_query.py` finds the function containing a given address, lists
everything that calls it, and can decompile the callers - useful for
tracing what a string or code location of interest is actually reachable
from (Ghidra's RTTI/exception-table analysis matters here: some string
references turn out to be compiler-generated exception-unwind metadata
rather than reachable gameplay code, so checking the actual call graph
before assuming a string is "live" saves a lot of time).

### 4. Widescreen / ultrawide patch

```
python3 tools/patch_widescreen.py \
  --exe /path/to/unwrapped/speed2.exe \
  --output speed2_widescreen.exe \
  --width 2560 --height 1080
```

Baked into the binary, not a runtime DirectX hook. Patches two independent
hardcoded 4:3-era values found by live-tracing which resolution actually
feeds the real gameplay camera's projection matrix (the video-options
resolution setting alone only affects the D3D backbuffer and two UI/overlay
cameras - it does not reach the world camera's field of view). See
`NOTES.md` for the full trace and why a second, similar-looking write site
turned out to be dead code in normal single-player. Live-verified in an
actual race: correct full-width fill with proper (non-stretched) FOV, not
just backbuffer stretching.

### 5. 2D UI letterboxing (menu/HUD/video aspect correction)

```
python3 tools/patch_letterbox_2d.py \
  --exe /path/to/widescreen-patched/speed2.exe \
  --output speed2_letterboxed.exe
```

Run after step 4 - the widescreen patch fixes the 3D camera's FOV, but the
game's 2D UI (menu, HUD, splash video) has no aspect-ratio logic at all
and just stretches to fill whatever viewport is active. This injects new
code (there's no existing "shrink the viewport for 2D" call to patch) that
narrows the viewport to a centered 4:3 rect before each UI/video draw
call and restores it immediately after, computed from the *live*
backbuffer resolution so it stays correct if that changes at runtime. See
`NOTES.md` for the long list of confirmed-live wrong turns this took
before landing on the actual fix - most of it isn't specific to this game
and is worth reading before attempting a similar DirectX-hooking patch
elsewhere.

### 6. (branch `native-elf-dxvk`) native Linux ELF port

```
native/tools/build_dxvk_native.sh 32
meson setup native/build32 --cross-file native/cross/linux32.txt
ninja -C native/build32 && meson test -C native/build32
```

Scaffolding for building the game as a **native 32-bit Linux ELF** instead of a
patched PE run under Wine: Wine's headers as the Win32/D3D9 declaration source,
a Win32 shim implemented directly on glibc/POSIX, and DXVK Native providing
D3D9 (for now - `DIRECTX_SCOPE.md` is the scope for replacing it with our own
Vulkan renderer). No Wine process is involved at runtime and no winelib is
used - see `native/README.md` for why that shape was chosen and
`NOTES.md` for the calling-convention findings the whole thing rests on.

Working today: the Win32 shim (~40% of the game's import list) with a 35-check
selftest, a D3D9 calling-convention test, a Wine-vs-DXVK header layout probe,
and an end-to-end smoke test that presents frames through DXVK to Vulkan. No
game code is ported yet.

## Setup

```
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

Ghidra (for `ghidra_query.py`) is a separate system install, not pip-
installable - see your distro's package or https://ghidra-sre.org/.

## Repo layout

- `tools/unwrap.py` - the PE patcher (step 2 above)
- `tools/patch_widescreen.py` - the widescreen/ultrawide patcher (step 4 above)
- `tools/patch_letterbox_2d.py` / `patches/letterbox_2d.s` - the 2D UI
  letterboxing patcher (step 5 above)
- `analysis/emulate_stub.py` - the Unicorn-based stub emulator (step 1)
- `analysis/ghidra_query.py` - Ghidra headless query helper (step 3)
- `analysis/retype_and_decompile.py` - fixes a Ghidra type-propagation bug
  (an incorrect DirectX vtable type spreading onto an unrelated
  parameter) and re-decompiles a function
- `native/` - the native Linux ELF port (branch `native-elf-dxvk`): header
  layer, Win32 shim, DXVK Native integration, tests - see `native/README.md`
- `NOTES.md` - reverse-engineering findings, what's solved vs. open
