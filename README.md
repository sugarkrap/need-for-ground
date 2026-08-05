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

## Setup

```
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

Ghidra (for `ghidra_query.py`) is a separate system install, not pip-
installable - see your distro's package or https://ghidra-sre.org/.

## Repo layout

- `tools/unwrap.py` - the PE patcher (step 2 above)
- `analysis/emulate_stub.py` - the Unicorn-based stub emulator (step 1)
- `analysis/ghidra_query.py` - Ghidra headless query helper (step 3)
- `NOTES.md` - reverse-engineering findings, what's solved vs. open
