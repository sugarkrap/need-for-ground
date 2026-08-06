#!/usr/bin/env python3
"""Compare the D3D9 layout as seen through Wine's headers vs DXVK Native's.

Runs both abi_probe binaries and diffs their key/value dumps. Any mismatch
means a struct we pass across the boundary is laid out differently on the two
sides, which would corrupt data at runtime with no obvious cause.

Keys present in only one dump are reported but not fatal: the two header sets
spell a few enum constants differently, and that is a naming difference, not
an ABI one.
"""
import subprocess
import sys


def dump(binary):
    out = subprocess.run([binary], capture_output=True, text=True, check=True).stdout
    result = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            kind, key, value = parts
            result[f"{kind} {key}"] = value
    return result


def main(argv):
    if len(argv) != 3:
        print("usage: abi_diff.py <wine-probe> <dxvk-probe>", file=sys.stderr)
        return 2

    wine, dxvk = dump(argv[1]), dump(argv[2])

    mismatches = []
    for key in sorted(set(wine) & set(dxvk)):
        if wine[key] != dxvk[key]:
            mismatches.append((key, wine[key], dxvk[key]))

    only_wine = sorted(set(wine) - set(dxvk))
    only_dxvk = sorted(set(dxvk) - set(wine))

    print(f"compared {len(set(wine) & set(dxvk))} shared entries")
    for key in only_wine:
        print(f"  note: only in wine dump:  {key} = {wine[key]}")
    for key in only_dxvk:
        print(f"  note: only in dxvk dump:  {key} = {dxvk[key]}")

    if mismatches:
        print(f"\n{len(mismatches)} ABI MISMATCH(ES):")
        for key, w, d in mismatches:
            print(f"  {key}: wine={w} dxvk={d}")
        return 1

    print("all shared sizes, offsets and constants agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
