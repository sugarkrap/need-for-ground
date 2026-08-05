#!/usr/bin/env python3
"""Query a Ghidra-analyzed NFSU2 exe: find what calls into a given function,
and decompile the callers. Useful for tracing what a specific string/address
of interest is actually reachable from.

Requires a Ghidra project already created + analyzed, e.g.:
    /opt/ghidra/support/analyzeHeadless <project_dir> <project_name> \\
        -import <exe_path> -overwrite

Usage:
    python3 ghidra_query.py --project-dir /path/to/project --project-name NAME \\
        --program speed2.exe --address 0x773262 [--address 0x776dcd ...]
"""
import argparse
import os


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-install-dir", default="/opt/ghidra")
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--program", required=True, help="program name as imported into the project")
    parser.add_argument("--address", action="append", required=True, type=lambda x: int(x, 0),
                         help="address to find the containing function + callers for (repeatable)")
    parser.add_argument("--decompile", action="store_true", help="also decompile each caller")
    args = parser.parse_args()

    os.environ["GHIDRA_INSTALL_DIR"] = args.ghidra_install_dir
    import pyghidra
    pyghidra.start()

    from ghidra.base.project import GhidraProject
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    project = GhidraProject.openProject(args.project_dir, args.project_name, True)
    program = project.openProgram("/", args.program, False)

    monitor = ConsoleTaskMonitor()
    fm = program.getFunctionManager()
    ref_mgr = program.getReferenceManager()

    decomp = DecompInterface()
    decomp.openProgram(program)

    def addr(a):
        return program.getAddressFactory().getDefaultAddressSpace().getAddress(a)

    for a in args.address:
        ea = addr(a)
        f = fm.getFunctionContaining(ea)
        print(f"\n=== {hex(a)}: containing function = "
              f"{f.getName() if f else None} @ "
              f"{hex(f.getEntryPoint().getOffset()) if f else 'N/A'} ===")
        if not f:
            continue

        entry = f.getEntryPoint()
        refs = list(ref_mgr.getReferencesTo(entry))
        print(f"{len(refs)} reference(s):")
        seen = set()
        for r in refs[:15]:
            from_addr = r.getFromAddress()
            caller = fm.getFunctionContaining(from_addr)
            cname = caller.getName() if caller else "?"
            print(f"  referenced from {hex(from_addr.getOffset())} in {cname}")
            if args.decompile and caller and caller.getEntryPoint() not in seen:
                seen.add(caller.getEntryPoint())
                res = decomp.decompileFunction(caller, 30, monitor)
                if res.decompileCompleted():
                    print(f"  --- decompiled {cname} ---")
                    print(res.getDecompiledFunction().getC()[:2000])

    project.close()


if __name__ == "__main__":
    main()
