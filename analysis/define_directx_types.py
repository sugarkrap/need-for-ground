#!/usr/bin/env python3
"""Define IDirect3D9/IDirect3DDevice9/IDirectDraw(7) as proper vtable
structures in a Ghidra project, so the decompiler resolves calls through
these interfaces to real method names (e.g. `DrawPrimitive`) instead of raw
vtable offsets (e.g. `(**(code**)(*obj + 0x38))()`) everywhere they're used.

Method orders come from directx_vtables.py, sourced from the public DirectX
SDK headers - this encodes a documented Microsoft ABI, not anything
reverse-engineered from the game binary.

Usage:
    python3 define_directx_types.py --project-dir /path --project-name NAME \\
        --program speed2.exe [--retype-global 0x870970=IDirect3D9]

--retype-global lets you apply a defined interface's pointer type directly
to a known global (find these by locating xrefs to e.g. Direct3DCreate9's
return value, or IDirectDraw7's QueryInterface out-param).
"""
import argparse
import os

from directx_vtables import INTERFACES


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-install-dir", default="/opt/ghidra")
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--retype-global", action="append", default=[],
                         help="ADDRESS=InterfaceName, e.g. 0x870970=IDirect3D9 (repeatable)")
    args = parser.parse_args()

    os.environ["GHIDRA_INSTALL_DIR"] = args.ghidra_install_dir
    import pyghidra
    pyghidra.start()

    from ghidra.base.project import GhidraProject
    from ghidra.program.model.data import (
        StructureDataType, FunctionDefinitionDataType, PointerDataType,
        CategoryPath, IntegerDataType, GenericCallingConvention,
        DataTypeConflictHandler,
    )
    from ghidra.program.model.data import DataUtilities

    project = GhidraProject.openProject(args.project_dir, args.project_name, True)
    program = project.openProgram("/", args.program, False)

    dtm = program.getDataTypeManager()
    cat = CategoryPath("/DirectX")
    ptr_size = 4

    tx_id = program.startTransaction("Define DirectX vtable types")
    obj_types = {}
    try:
        for iface_name, methods in INTERFACES.items():
            vtbl = StructureDataType(cat, iface_name + "Vtbl", 0, dtm)
            for m in methods:
                fdef = FunctionDefinitionDataType(cat, f"{iface_name}_{m}", dtm)
                fdef.setReturnType(IntegerDataType.dataType)
                fdef.setGenericCallingConvention(GenericCallingConvention.stdcall)
                fdef = dtm.resolve(fdef, DataTypeConflictHandler.REPLACE_HANDLER)
                vtbl.add(PointerDataType(fdef, ptr_size, dtm), m, None)
            vtbl = dtm.resolve(vtbl, DataTypeConflictHandler.REPLACE_HANDLER)

            obj = StructureDataType(cat, iface_name, 0, dtm)
            obj.add(PointerDataType(vtbl, ptr_size, dtm), "lpVtbl", None)
            obj = dtm.resolve(obj, DataTypeConflictHandler.REPLACE_HANDLER)
            obj_types[iface_name] = obj
            print(f"Defined {iface_name} ({len(methods)} methods)")

        for spec in args.retype_global:
            addr_str, iface_name = spec.split("=")
            addr = program.getAddressFactory().getDefaultAddressSpace().getAddress(int(addr_str, 0))
            obj_ptr = PointerDataType(obj_types[iface_name], ptr_size, dtm)
            DataUtilities.createData(program, addr, obj_ptr, -1, DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA)
            print(f"Retyped {hex(addr.getOffset())} as {iface_name}*")

        program.endTransaction(tx_id, True)
    except Exception:
        program.endTransaction(tx_id, False)
        raise

    project.save(program)
    project.close()
    print("Saved.")


if __name__ == "__main__":
    main()
