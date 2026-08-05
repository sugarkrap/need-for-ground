#!/usr/bin/env python3
"""Produce a concrete, readable inventory of every function triage.py
classified as directx/transitive-directx - the actual starting scope for
the renderer-porting subproject.

For direct callers, lists which DirectX methods they invoke. For
transitive callers, lists the chain to the nearest direct DirectX call so
it's clear *why* each one is in scope.

Usage:
    python3 list_directx_functions.py --decompiled-dir ../decompiled \\
        --triage triage.json --output directx_scope.md
"""
import argparse
import json
import os
import re

VTBL_CALL_RE = re.compile(r"->lpVtbl->([A-Za-z0-9_]+)\)")
CALL_RE = re.compile(r"\bFUN_([0-9a-fA-F]{8})\s*\(")
HEADER_RE = re.compile(r"^// (\S+) @ (0x[0-9a-fA-F]+)")


def load_functions(decompiled_dir, addrs):
    out = {}
    for addr in addrs:
        # filenames are "<addr8>_<name>.c"
        matches = [f for f in os.listdir(decompiled_dir) if f.startswith(addr)]
        if not matches:
            continue
        path = os.path.join(decompiled_dir, matches[0])
        with open(path, errors="replace") as f:
            text = f.read()
        header = HEADER_RE.match(text)
        name = header.group(1) if header else f"FUN_{addr}"
        out[addr] = {
            "name": name,
            "file": matches[0],
            "directx_methods": sorted(set(VTBL_CALL_RE.findall(text))),
            "calls": set(CALL_RE.findall(text)),
        }
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decompiled-dir", required=True)
    parser.add_argument("--triage", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    with open(args.triage) as f:
        triage = json.load(f)

    directx_addrs = [a for a, v in triage.items() if v["category"] in ("directx", "transitive-directx")]
    direct_addrs = [a for a in directx_addrs if triage[a]["category"] == "directx"]
    transitive_addrs = [a for a in directx_addrs if triage[a]["category"] == "transitive-directx"]

    all_info = load_functions(args.decompiled_dir, directx_addrs)
    direct_set = set(direct_addrs)

    def find_chain(addr, visited=None):
        visited = visited or set()
        info = all_info.get(addr)
        if not info or addr in visited:
            return []
        visited = visited | {addr}
        if addr in direct_set:
            return [(addr, info["name"])]
        for called in sorted(info["calls"]):
            if called in all_info and called not in visited:
                chain = find_chain(called, visited)
                if chain:
                    return [(addr, info["name"])] + chain
        return []

    lines = [
        "# DirectX-touching function inventory",
        "",
        f"Auto-generated from `triage.json` - {len(directx_addrs)} functions total "
        f"({len(direct_addrs)} direct, {len(transitive_addrs)} transitive). "
        "This is the concrete starting scope for the D3D9->Vulkan renderer port.",
        "",
        "## Direct callers",
        "",
        "Functions that call a DirectX interface method themselves.",
        "",
    ]
    for addr in sorted(direct_addrs):
        info = all_info[addr]
        methods = ", ".join(info["directx_methods"]) or "(vtable call detected, method name unresolved)"
        lines.append(f"- `{info['name']}` @ `0x{addr}` - {methods}")

    lines += ["", "## Transitive callers", "",
              "Functions that reach a direct caller through the call graph "
              "(chain shown to the nearest direct call).", ""]
    for addr in sorted(transitive_addrs):
        info = all_info[addr]
        chain = find_chain(addr)
        chain_str = " -> ".join(name for _, name in chain) if chain else "(chain not resolved)"
        lines.append(f"- `{info['name']}` @ `0x{addr}`: {chain_str}")

    with open(args.output, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {args.output} ({len(directx_addrs)} functions)")


if __name__ == "__main__":
    main()
