#!/usr/bin/env python3
"""Categorize every decompiled function by its Win32/DirectX dependency, as
a starting map for porting priority.

Two detection tiers:
  - direct: the function's own decompiled text calls a named Win32 import,
    or (post define_directx_types.py) a named DirectX interface method via
    `->lpVtbl->MethodName`.
  - transitive: the function doesn't call anything platform-specific
    itself, but calls another function that (directly or transitively)
    does - computed via a plain call-graph walk over `FUN_xxxxxxxx(...)`
    references in the decompiled text (approximate: relies on Ghidra's own
    naming, not a full instruction-level call graph).

Everything else is bucketed as "pure" - candidates for straightforward
logic-only porting with no OS/rendering-layer rewrite needed.

Usage:
    python3 triage.py --decompiled-dir ../decompiled --output triage.json
"""
import argparse
import json
import re
from collections import defaultdict

# Import categories - which imported DLL a Win32 call belongs to determines
# what kind of porting work it implies.
DLL_CATEGORY = {
    "kernel32": "posix", "advapi32": "posix", "user32": "windowing",
    "gdi32": "windowing", "shfolder": "posix", "shell32": "posix",
    "ws2_32": "posix", "winmm": "posix", "tapi32": "posix", "netapi32": "posix",
    "d3d9": "directx", "ddraw": "directx", "dinput8": "directx",
    "dsound": "directx",
}

# DirectX interface method names (from directx_vtables.py) - matched via
# the `->lpVtbl->MethodName` pattern define_directx_types.py produces.
def load_directx_methods():
    import directx_vtables
    names = set()
    for methods in directx_vtables.INTERFACES.values():
        names.update(methods)
    return names


CALL_RE = re.compile(r"\bFUN_([0-9a-fA-F]{8})\s*\(")
NAMED_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
VTBL_CALL_RE = re.compile(r"->lpVtbl->([A-Za-z0-9_]+)\)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decompiled-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--win32-names", help="path to a newline-separated list of known Win32 import names "
                                                "(if omitted, only DirectX vtable calls are detected directly)")
    args = parser.parse_args()

    directx_methods = load_directx_methods()
    win32_names = set()
    if args.win32_names:
        with open(args.win32_names) as f:
            win32_names = {line.strip() for line in f if line.strip()}

    import os
    files = [f for f in os.listdir(args.decompiled_dir) if f.endswith(".c")]
    print(f"{len(files)} decompiled functions to triage")

    # addr -> {name, direct_win32: set, direct_directx: set, calls: set(addrs)}
    functions = {}
    for fn in files:
        addr = fn[:8]
        path = os.path.join(args.decompiled_dir, fn)
        with open(path, errors="replace") as f:
            text = f.read()

        direct_directx = {m for m in VTBL_CALL_RE.findall(text) if m in directx_methods}
        direct_win32 = set()
        if win32_names:
            for name in NAMED_CALL_RE.findall(text):
                if name in win32_names:
                    direct_win32.add(name)
        calls = set(CALL_RE.findall(text))

        functions[addr] = {
            "file": fn,
            "direct_win32": direct_win32,
            "direct_directx": direct_directx,
            "calls": calls,
        }

    # transitive closure: BFS from any function with a direct dependency
    category = {}
    for addr, info in functions.items():
        if info["direct_directx"]:
            category[addr] = "directx"
        elif info["direct_win32"]:
            category[addr] = "win32"

    changed = True
    while changed:
        changed = False
        for addr, info in functions.items():
            if addr in category:
                continue
            for called in info["calls"]:
                if called in category:
                    category[addr] = f"transitive-{category[called]}"
                    changed = True
                    break

    counts = defaultdict(int)
    for addr in functions:
        counts[category.get(addr, "pure")] += 1

    print("\nTriage summary:")
    for cat, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {cat:20s} {n:6d} ({100 * n / len(functions):.1f}%)")

    result = {
        addr: {"file": info["file"], "category": category.get(addr, "pure")}
        for addr, info in functions.items()
    }
    with open(args.output, "w") as f:
        json.dump(result, f, indent=1)
    print(f"\nWrote {args.output}")


if __name__ == "__main__":
    main()
