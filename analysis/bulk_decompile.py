#!/usr/bin/env python3
"""Dump Ghidra's decompiler output for every function in a program to
individual .c files - a rough, non-compiling first draft of the whole
codebase to triage/plan from, not a finished port.

Output is NOT meant to be committed anywhere - it's a direct derivative of
the (copyrighted) game binary, same category as the binary itself. Keep it
local/gitignored.

Resumable: skips any function whose output file already exists, so an
interrupted run can just be restarted.

Usage:
    python3 bulk_decompile.py --project-dir /path/to/project \\
        --project-name NAME --program speed2.exe --output-dir ./decompiled
"""
import argparse
import os
import re
import time


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:80]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-install-dir", default="/opt/ghidra")
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--timeout-secs", type=int, default=10, help="per-function decompile timeout")
    parser.add_argument("--limit", type=int, default=0, help="stop after N functions (0 = no limit, for testing)")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    os.environ["GHIDRA_INSTALL_DIR"] = args.ghidra_install_dir
    import pyghidra
    pyghidra.start()

    from ghidra.base.project import GhidraProject
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    project = GhidraProject.openProject(args.project_dir, args.project_name, True)
    program = project.openProgram("/", args.program, False)

    fm = program.getFunctionManager()
    decomp = DecompInterface()
    decomp.openProgram(program)
    monitor = ConsoleTaskMonitor()

    funcs = [f for f in fm.getFunctions(True) if not f.isThunk() and not f.isExternal()]
    total = len(funcs) if not args.limit else min(args.limit, len(funcs))
    print(f"{len(funcs)} functions found, processing {total}")

    ok = skipped = failed = 0
    start = time.time()
    for i, f in enumerate(funcs[:total]):
        addr = f.getEntryPoint().getOffset()
        fname = safe_name(f.getName())
        out_path = os.path.join(args.output_dir, f"{addr:08x}_{fname}.c")
        if os.path.exists(out_path):
            skipped += 1
            continue
        try:
            res = decomp.decompileFunction(f, args.timeout_secs, monitor)
            if res.decompileCompleted():
                with open(out_path, "w") as fh:
                    fh.write(f"// {f.getName()} @ {hex(addr)}\n")
                    fh.write(res.getDecompiledFunction().getC())
                ok += 1
            else:
                with open(out_path, "w") as fh:
                    fh.write(f"// DECOMPILE FAILED: {f.getName()} @ {hex(addr)}\n"
                              f"// {res.getErrorMessage()}\n")
                failed += 1
        except Exception as e:
            with open(out_path, "w") as fh:
                fh.write(f"// EXCEPTION during decompile: {f.getName()} @ {hex(addr)}\n// {e}\n")
            failed += 1

        if (i + 1) % 200 == 0:
            elapsed = time.time() - start
            rate = (i + 1) / elapsed
            eta = (total - i - 1) / rate if rate else 0
            print(f"{i + 1}/{total} (ok={ok} skipped={skipped} failed={failed}) "
                  f"{rate:.1f}/s, eta {eta / 60:.1f} min")

    print(f"\nDone. ok={ok} skipped={skipped} failed={failed} total={total} "
          f"in {(time.time() - start) / 60:.1f} min")
    project.close()


if __name__ == "__main__":
    main()
