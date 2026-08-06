#!/usr/bin/env python3
"""Measure how much of the decompiled output compiles as-is.

Answers the question the port actually needs answered: of the game's functions,
how many can be fed to the compiler with only the ghidra_types.h harness, and for
the rest, *what* stops them? Guessing at that is how a port ends up with a plan
based on the wrong bottleneck.

    python3 native/tools/survey_decompiled.py --sample 400
    python3 native/tools/survey_decompiled.py --all --jobs 8

Each function is compiled in isolation, so a failure means "this function needs
work", not "the build is broken". Two things are neutralised first, because they
are artefacts of compiling one function alone rather than port blockers:

  * calls to other FUN_ symbols, via -fpermissive - the callee simply is not
    declared yet, which says nothing about whether this function is portable
  * references to DAT_ globals, by generating `extern int` declarations - the
    real port needs their types recovered, but that is data-layout work, not a
    decompiler-output problem, and it is counted separately when the generic
    declaration causes a type error

Failures are bucketed by cause. The buckets are deliberately specific - "needs
a type" and "reads an uninitialised register" are entirely different amounts of
work - and anything unrecognised is reported verbatim so the categories can grow
from evidence rather than from imagination.
"""
import argparse
import random
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[1]
REPO = NATIVE.parent

# (bucket, matcher) - order matters, first match wins.
BUCKETS = [
    ("in_XX register read: Ghidra missed a parameter",
     re.compile(r"\bin_(EAX|ECX|EDX|EBX|ESI|EDI|ST0|stack)")),
    ("unknown width placeholder (unkbyte/unkuint/...)",
     re.compile(r"unkbyte|unkuint|unkfloat|undefined9|undefined1[0-9]")),
    ("field-slice syntax on a scalar (_4_4_ etc.)",
     re.compile(r"_[0-9]+_[0-9]+_")),
    ("missing pseudo-intrinsic",
     re.compile(r"implicit declaration of function .(SUB|CONCAT|ZEXT|SEXT|ROUND|NAN|ABS|SQRT|"
                r"POPCOUNT|CARRY|SBORROW|BREAK|halt_baddata)")),
    ("call through an untyped pointer (code *)",
     re.compile(r"called object is not a function|n'est pas une fonction")),
    ("undeclared identifier (another function or a global)",
     re.compile(r"undeclared|non déclar")),
    ("type mismatch or unknown type",
     re.compile(r"unknown type name|nom de type|conflicting types|types conflictuels|"
                r"incompatible")),
]

HARNESS = """#include <nfsu2/ghidra_types.h>
#include <nfsu2/win32_compat.h>
"""

# Ghidra's names for data it found but did not type: globals, string literals,
# jump tables, and pointers to them.
DATA_SYMBOL = re.compile(r"\b((?:_?DAT|UNK|PTR(?:_[A-Za-z0-9]+)?|s|u|e|FLOAT|DOUBLE|CSWTCH)"
                         r"_[0-9a-f]{4,8}(?:_[0-9a-f]+)?)\b")


def classify(stderr):
    for name, matcher in BUCKETS:
        if matcher.search(stderr):
            return name
    first = ""
    for line in stderr.splitlines():
        if "error" in line or "erreur" in line:
            first = line.split(":")[-1].strip()[:70]
            break
    return f"other: {first}" if first else "other"


def compile_one(path, wine_headers, keep_going):
    source = path.read_text()
    # Ghidra's conventions map onto our attribute macros, as the importer does.
    for ghidra, macro in (("__cdecl", "NFSU2_CDECL"), ("__stdcall", "NFSU2_STDCALL"),
                          ("__thiscall", "NFSU2_THISCALL"), ("__fastcall", "NFSU2_FASTCALL")):
        source = re.sub(rf"(?<![\w]){ghidra}(?=[\s\n])", macro, source)

    # Declare the data symbols this function references, generically. A wrong
    # type here surfaces as a type error, which is the honest bucket for it:
    # recovering those types is real work the port has to do.
    declarations = "".join(f"extern int {name};\n"
                           for name in sorted(set(DATA_SYMBOL.findall(source))))

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as handle:
        handle.write(HARNESS)
        handle.write(declarations)
        handle.write(source)
        temp = Path(handle.name)
    try:
        result = subprocess.run(
            ["gcc", "-m32", "-c", "-o", "/dev/null",
             "-I", str(NATIVE / "include"), "-isystem", str(wine_headers),
             "-D_GNU_SOURCE", "-w",
             # An undeclared callee is not a portability problem; a genuinely
             # unportable construct still fails.
             "-fpermissive", str(temp)],
            capture_output=True, text=True)
        if result.returncode == 0:
            return path.name, None
        return path.name, classify(result.stderr)
    finally:
        temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decompiled", type=Path, default=REPO / "decompiled")
    parser.add_argument("--wine-headers", type=Path, default=Path("/usr/include/wine/windows"))
    parser.add_argument("--sample", type=int, default=300,
                        help="how many functions to try (default 300)")
    parser.add_argument("--all", action="store_true", help="try every function")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--seed", type=int, default=1,
                        help="sampling seed, so a run is reproducible")
    parser.add_argument("--list-failures", metavar="BUCKET",
                        help="print the files in one bucket")
    args = parser.parse_args()

    if not args.decompiled.is_dir():
        sys.exit(f"decompiled output not found: {args.decompiled}")

    files = sorted(args.decompiled.glob("*.c"))
    if not files:
        sys.exit(f"no .c files in {args.decompiled}")
    population = len(files)

    if not args.all and args.sample < population:
        random.Random(args.seed).shuffle(files)
        files = files[:args.sample]

    print(f"compiling {len(files)} of {population} decompiled functions "
          f"(-m32, ghidra_types.h harness, no linking)\n")

    buckets = {}
    compiled = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, bucket in pool.map(lambda p: compile_one(p, args.wine_headers, True), files):
            if bucket is None:
                compiled += 1
            else:
                buckets.setdefault(bucket, []).append(name)

    print(f"compiles as-is : {compiled}/{len(files)} ({100.0 * compiled / len(files):.1f}%)")
    print(f"needs work     : {len(files) - compiled}\n")
    print("why the rest fail:")
    for bucket, names in sorted(buckets.items(), key=lambda item: -len(item[1])):
        share = 100.0 * len(names) / len(files)
        print(f"  {len(names):5d}  ({share:4.1f}%)  {bucket}")
        if args.list_failures and args.list_failures in bucket:
            for name in names[:40]:
                print(f"           {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
