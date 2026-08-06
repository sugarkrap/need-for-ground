#!/usr/bin/env python3
"""Turn Ghidra's decompiled output into compilable translation units.

Reads native/game/manifest.txt (a list of addresses and symbol names) plus your
local decompiled/ tree, and writes native/game/generated/: one .c per function
plus a header declaring them all.

The generated tree is gitignored, deliberately. Decompiled pseudocode is a
direct derivative of the copyrighted game binary - the same category as the
binary itself - so it is never committed, exactly as decompiled/ is not. What
lives in the repo is this tool, the manifest, and the tests.

    python3 native/tools/import_decompiled.py
    python3 native/tools/import_decompiled.py --list      # what the manifest asks for
    python3 native/tools/import_decompiled.py --clean

Normalisation applied, and why each is needed:

  * an include of <nfsu2/ghidra_types.h> for undefined4/byte/uint/... and the
    pseudo-intrinsics (SQRT, ABS, NAN, SBORROW4, ...)
  * Ghidra's `__cdecl` / `__thiscall` / `__fastcall` annotations mapped onto the
    NFSU2_* attribute macros. This is not cosmetic: `__thiscall` means the first
    parameter arrives in ECX, and dropping it would silently change the ABI of
    every ported member function.
  * `float10` left alone (ghidra_types.h maps it to long double, which is what
    the x87 80-bit intermediates in the original were)
  * a `#line` directive pointing back at the decompiled source, so a compiler
    error names the file you can actually go and look at
"""
import argparse
import re
import sys
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[1]
REPO = NATIVE.parent
DECOMPILED = REPO / "decompiled"
MANIFEST = NATIVE / "game/manifest.txt"
OUT_DIR = NATIVE / "game/generated"

# "void __thiscall FUN_0048b710(float *param_1, ..." - the calling convention sits
# between the return type and the name, and may be absent.
CONVENTIONS = {
    "__cdecl": "NFSU2_CDECL",
    "__stdcall": "NFSU2_STDCALL",
    "__thiscall": "NFSU2_THISCALL",
    "__fastcall": "NFSU2_FASTCALL",
}


def set_decompiled_dir(path):
    """The manifest resolver reads a module-level path; keep the override in one
    place rather than threading it through every helper."""
    global DECOMPILED
    DECOMPILED = path


def parse_manifest():
    if not MANIFEST.exists():
        sys.exit(f"manifest not found: {MANIFEST}")
    entries = []
    for line_number, raw in enumerate(MANIFEST.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        if len(parts) < 3:
            sys.exit(f"{MANIFEST}:{line_number}: expected '<address> <symbol> <kind> [note]'")
        address, symbol, kind = parts[0], parts[1], parts[2]
        note = parts[3] if len(parts) > 3 else ""
        entries.append({"address": address, "symbol": symbol, "kind": kind, "note": note})
    return entries


def find_source(entry):
    """Locate the decompiled file for a manifest entry.

    bulk_decompile.py names files <address>_<symbol>.c with the address padded to
    eight hex digits, so the manifest's 0x43ce40 has to be normalised first.
    """
    address = int(entry["address"], 16)
    candidates = sorted(DECOMPILED.glob(f"{address:08x}_*.c"))
    if not candidates:
        return None
    # A single address can have several spellings if Ghidra renamed a function
    # (FID_conflict_* and friends); prefer an exact symbol match.
    for path in candidates:
        if path.stem.split("_", 1)[1] == entry["symbol"]:
            return path
    return candidates[0]


def extract_signature(text):
    """Return the function's declaration, as one line.

    Ghidra puts the signature after the comment block, but wraps it when it is
    long - the return type and calling convention end up on their own line:

        void __thiscall
        FUN_0048b710(float *param_1, ...)

    so collecting only the line containing '(' silently drops the return type and
    the convention, and the declaration then defaults to int. Accumulate lines
    from the first non-comment one until the parameter list closes.
    """
    # Comments have to be removed rather than skipped line by line: Ghidra's
    # header block puts the symbol name on its own continuation line, which looks
    # exactly like the start of a declaration.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)

    collected = []
    depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if not collected and (not stripped or stripped.startswith("#")):
            continue
        collected.append(stripped)
        depth += stripped.count("(") - stripped.count(")")
        if "(" in " ".join(collected) and depth == 0:
            return " ".join(collected)
    return None


def normalise(entry, source_path):
    original = source_path.read_text()
    body = original

    # Map the convention annotation onto our attribute macros.
    convention_used = None
    for ghidra_name, macro in CONVENTIONS.items():
        pattern = re.compile(rf"(?<![\w]){re.escape(ghidra_name)}(?=[\s\n])")
        if pattern.search(body):
            convention_used = ghidra_name
            body = pattern.sub(macro, body)

    header = [
        "/*",
        f" * {entry['symbol']} @ {entry['address']} - imported from decompiled output.",
        " *",
        f" * {entry['kind']}: {entry['note']}" if entry["note"] else f" * {entry['kind']}",
        " *",
        " * GENERATED by native/tools/import_decompiled.py - do not edit, and do not",
        " * commit: this is a derivative of the game binary. Edit the decompiled source",
        " * or the manifest instead.",
        " */",
        "#include <nfsu2/ghidra_types.h>",
        f'#include "game_functions.h"',
        "",
        f'#line 1 "{source_path.name}"',
        "",
    ]
    return "\n".join(header) + body, convention_used


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true", help="show the manifest and exit")
    parser.add_argument("--clean", action="store_true", help="remove the generated tree")
    parser.add_argument("--decompiled", type=Path, default=DECOMPILED,
                        help=f"decompiled output directory (default {DECOMPILED})")
    args = parser.parse_args()

    set_decompiled_dir(args.decompiled)

    entries = parse_manifest()

    if args.list:
        for entry in entries:
            print(f"{entry['address']:>10}  {entry['symbol']:<16} {entry['kind']:<5} "
                  f"{entry['note']}")
        return 0

    if args.clean:
        if OUT_DIR.exists():
            for path in OUT_DIR.iterdir():
                path.unlink()
            OUT_DIR.rmdir()
            print(f"removed {OUT_DIR}")
        return 0

    if not DECOMPILED.is_dir():
        sys.exit(f"decompiled output not found: {DECOMPILED}\n"
                 f"Run analysis/bulk_decompile.py first (see README.md step 3).")

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    declarations = []
    written = 0
    missing = []

    for entry in entries:
        source = find_source(entry)
        if source is None:
            missing.append(entry)
            continue

        text, convention = normalise(entry, source)
        (OUT_DIR / f"{entry['symbol']}.c").write_text(text)
        written += 1

        signature = extract_signature(source.read_text())
        if signature:
            for ghidra_name, macro in CONVENTIONS.items():
                signature = signature.replace(ghidra_name, macro)
            declarations.append((entry, signature.rstrip("{ ").rstrip()))
        print(f"  {entry['symbol']:<16} <- {source.name}"
              + (f"  [{convention}]" if convention else ""))

    header = [
        "/*",
        " * game_functions.h - declarations for the imported functions.",
        " *",
        " * GENERATED by native/tools/import_decompiled.py - do not edit, do not commit.",
        " */",
        "#ifndef NFSU2_GAME_FUNCTIONS_H",
        "#define NFSU2_GAME_FUNCTIONS_H",
        "",
        "#include <nfsu2/ghidra_types.h>",
        "#include <nfsu2/win32_compat.h>",
        "",
    ]
    for entry, signature in declarations:
        header.append(f"/* {entry['address']} ({entry['kind']}) {entry['note']} */")
        header.append(f"{signature};")
        header.append("")
    header += ["#endif /* NFSU2_GAME_FUNCTIONS_H */", ""]
    (OUT_DIR / "game_functions.h").write_text("\n".join(header))

    print(f"\nwrote {written} function(s) to {OUT_DIR.relative_to(REPO)}")
    if missing:
        print(f"\n{len(missing)} not found in {DECOMPILED}:")
        for entry in missing:
            print(f"  {entry['address']} {entry['symbol']}")
        print("Decompile them first, or drop them from the manifest.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
