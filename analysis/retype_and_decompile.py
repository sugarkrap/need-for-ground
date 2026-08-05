#!/usr/bin/env python3
"""Clear an incorrectly-propagated parameter type on a function, then
re-decompile and print it.

Ghidra's type propagation isn't perfect: applying a COM interface pointer
type to one known-correct global (via define_directx_types.py
--retype-global) can sometimes get inferred onto unrelated pointers
elsewhere in the program that happen to flow through similar-looking code,
producing decompiled output that "resolves" vtable calls through the wrong
interface (e.g. showing `ShowCursor` at a vtable slot that's actually some
other object's completely different method). When a function's decompiled
output looks like nonsense method names in plausible-looking places, this
is usually why - clearing the bad type on that specific parameter and
re-decompiling usually restores readable pseudocode (calls show as raw
`(**(code**)(*obj + 0xNN))()` again instead of a wrong name, which is worse
information but not actively misleading).

Usage:
    python3 retype_and_decompile.py --project-dir /path --project-name NAME \\
        --program speed2.exe --function 0x5c5b70 --param 0
"""
import argparse
import os


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-install-dir", default="/opt/ghidra")
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--function", required=True, type=lambda x: int(x, 0),
                         help="entry point address of the function to fix")
    parser.add_argument("--param", type=int, default=0, help="parameter index to clear (0-based)")
    args = parser.parse_args()

    os.environ["GHIDRA_INSTALL_DIR"] = args.ghidra_install_dir
    import pyghidra
    pyghidra.start()

    from ghidra.base.project import GhidraProject
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor
    from ghidra.program.model.data import VoidDataType, PointerDataType
    from ghidra.program.model.symbol import SourceType
    from ghidra.program.model.listing import ParameterImpl

    project = GhidraProject.openProject(args.project_dir, args.project_name, True)
    program = project.openProgram("/", args.program, False)

    fm = program.getFunctionManager()
    addr = program.getAddressFactory().getDefaultAddressSpace().getAddress(args.function)
    func = fm.getFunctionAt(addr)
    if func is None:
        print(f"no function at {hex(args.function)}")
        return

    dtm = program.getDataTypeManager()
    void_ptr = PointerDataType(VoidDataType.dataType, 4, dtm)

    params = func.getParameters()
    tx_id = program.startTransaction(f"Clear param {args.param} type on {func.getName()}")
    try:
        if args.param < len(params):
            p = params[args.param]
            old_type = p.getDataType()
            p.setDataType(void_ptr, SourceType.USER_DEFINED)
            print(f"{func.getName()} @ {hex(args.function)}: param {args.param} retyped "
                  f"{old_type} -> void*")
        else:
            # Ghidra hasn't committed a formal signature for this function - the
            # decompiler's "param_1" is inferred fresh each decompile, with no
            # persistent Function.Parameter to edit. Commit an explicit signature
            # instead, forcing the decompiler to use our type from here on.
            func.addParameter(
                ParameterImpl(f"param_{args.param + 1}", void_ptr, program),
                SourceType.USER_DEFINED)
            print(f"{func.getName()} @ {hex(args.function)}: no formal param {args.param} existed, "
                  f"added one as void*")
        program.endTransaction(tx_id, True)
    except Exception:
        program.endTransaction(tx_id, False)
        raise

    decomp = DecompInterface()
    decomp.openProgram(program)
    monitor = ConsoleTaskMonitor()
    res = decomp.decompileFunction(func, 60, monitor)
    if res.decompileCompleted():
        print("\n--- re-decompiled ---")
        print(res.getDecompiledFunction().getC())
    else:
        print("decompile failed:", res.getErrorMessage())

    project.close()


if __name__ == "__main__":
    main()
