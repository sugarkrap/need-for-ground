#!/usr/bin/env python3
"""Report Win32 shim coverage against the game's real import list.

Cross-references analysis/win32_imports.txt (the imports the unwrapped exe
actually pulls in) with the entry points implemented in native/src/win32/, so
"what is left to shim" is a measured number rather than a guess.

    python3 native/tools/win32_coverage.py            # summary + what is missing
    python3 native/tools/win32_coverage.py --done     # what is implemented
    python3 native/tools/win32_coverage.py --extra    # implemented but not imported
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
IMPORTS = REPO / "analysis/win32_imports.txt"
# Every DLL we implement: src/win32 (kernel32 and friends), src/user32, ...
SHIM_ROOT = REPO / "native/src"

# A definition looks like:  RETTYPE WINAPI Name(args)  - possibly with the
# return type split over the previous line, which we do not do in this tree.
DEF_RE = re.compile(r"^[A-Za-z_][\w \t*]*\bWINAPI\s+(\w+)\s*\(", re.MULTILINE)

# Imports that are satisfied without a shim of ours, and by what.
PROVIDED_ELSEWHERE = {
    "Direct3DCreate9": "DXVK Native (libdxvk_d3d9.so)",
}

# Groups for the missing-list, in the rough order a port needs them. An entry
# matches if the name contains the substring, case-sensitively - "Rect" must
# not swallow "Direct3DCreate9".
GROUPS = [
    ("window/message loop", ("Window", "Message", "Paint", "Cursor", "Rect", "Focus",
                             "Capture", "Icon", "Class", "Iconic", "SystemMetrics",
                             "DesktopWindow", "ForegroundWindow", "ActiveWindow")),
    ("gdi/text", ("Bitmap", "DC", "Font", "TextOut", "BkColor", "BkMode", "TextColor",
                  "SelectObject", "DeleteObject", "GetPixel", "BitBlt")),
    ("registry", ("Reg",)),
    ("locale/CRT support", ("Locale", "CPInfo", "ACP", "OEMCP", "StringType", "CodePage",
                            "CompareString", "LCMapString", "MultiByte", "WideChar",
                            "EnvironmentStrings", "wsprintf", "wvsprintf", "lstrcmp")),
    ("shell/paths", ("SHGet", "ShellExecute", "LongPathName", "DiskFreeSpace",
                     "DriveType", "LogicalDrives")),
    ("sockets/net", ("WSA", "socket", "bind", "listen", "accept", "connect", "recv",
                     "send", "select", "shutdown", "sockopt", "hostbyname", "peername",
                     "sockname", "ioctlsocket", "closesocket", "Netbios")),
    ("telephony/serial", ("line", "Comm", "Overlapped", "Setup")),
    ("process/toolhelp", ("Toolhelp", "Process32", "ExitProcess", "TerminateProcess")),
    ("resources", ("Resource", "FindResource", "LoadResource", "LockResource", "Sizeof")),
    ("exceptions/unwind", ("Unwind", "Exception", "RaiseException")),
    ("d3d/input entry points", ("Direct3DCreate9", "DirectDrawCreate", "DirectInput8Create")),
    ("timers", ("timeSetEvent", "timeKillEvent", "timeGetDevCaps")),
]


def implemented():
    names = {}
    for path in sorted(SHIM_ROOT.glob("*/*.c")):
        for name in DEF_RE.findall(path.read_text()):
            names[name] = path.parent.name
    return names


def imported():
    if not IMPORTS.exists():
        sys.exit(f"import list not found: {IMPORTS}")
    return {line.strip() for line in IMPORTS.read_text().splitlines() if line.strip()}


def classify(names):
    remaining = set(names)
    out = []
    for label, needles in GROUPS:
        hit = {n for n in remaining if any(k in n for k in needles)}
        if hit:
            out.append((label, sorted(hit)))
            remaining -= hit
    if remaining:
        out.append(("other", sorted(remaining)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--done", action="store_true", help="list implemented imports")
    ap.add_argument("--extra", action="store_true",
                    help="list shim functions not in the import list")
    args = ap.parse_args()

    have_map = implemented()
    have, want = set(have_map), imported()
    external = {n for n in want if n in PROVIDED_ELSEWHERE}
    done = sorted(have & want)
    missing = sorted(want - have - external)
    extra = sorted(have - want)

    covered = len(done) + len(external)
    print(f"imports needed  : {len(want)}")
    print(f"implemented     : {len(done)} shimmed"
          f" + {len(external)} provided externally"
          f" = {covered} ({100.0 * covered / len(want):.1f}%)")
    for name in sorted(external):
        print(f"                  {name} <- {PROVIDED_ELSEWHERE[name]}")
    print(f"still missing   : {len(missing)}")
    if extra:
        print(f"shimmed but not imported: {len(extra)}"
              " (helpers, or names the import list does not cover)")

    by_dll = {}
    for name in done:
        by_dll.setdefault(have_map[name], []).append(name)
    print("by shim module  : " +
          ", ".join(f"{d}={len(v)}" for d, v in sorted(by_dll.items())))

    if args.done:
        print("\nimplemented:")
        for name in done:
            print(f"  {name:<32} ({have_map[name]})")
        return
    if args.extra:
        print("\nnot in the import list:")
        for name in extra:
            print(f"  {name}")
        return

    print("\nmissing, grouped:")
    for label, names in classify(missing):
        print(f"\n  {label} ({len(names)})")
        for name in names:
            print(f"    {name}")


if __name__ == "__main__":
    main()
