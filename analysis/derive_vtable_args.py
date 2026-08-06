#!/usr/bin/env python3
"""Derive each COM method's argument size from d3d9.h, for Ghidra's vtable types.

Why this exists, and it is not cosmetic. `define_directx_types.py` declares each
vtable slot as a `__stdcall` function definition - correct - but with *no
parameters*, which leaves the decompiler unable to know how many bytes the callee
pops. Every read of a stack argument *after* a vtable call is then computed from
the wrong esp, silently. It cost a real bug:

    FUN_005bc7b0, machine code                 Ghidra's decompilation
    ---------------------------------------    ---------------------------------
    push [esp+4]  ; param_1                    SetRenderState(dev, 0xf,  param_1)
    push 0xf   -> SetRenderState               SetRenderState(dev, 0x18, param_1)
    push [esp+8]  ; param_2                                            ^^^^^^^
    push 0x18  -> SetRenderState               should be param_2

The differential test against the original machine code caught it (see
native/tests/game_functions_selftest.c), but "caught by a test" only works for
functions someone has already ported - so the decompiler needs the argument sizes
instead.

The sizes are not remembered here, they are read out of the SDK's own header
declarations - the same standard as directx_vtables.py, which encodes the method
*order* from the same source. Output is a Python dict, so it can be checked into
directx_vtables.py or imported directly:

    python3 derive_vtable_args.py --header /usr/include/wine/windows/d3d9.h
    python3 derive_vtable_args.py --check       # names+order must match INTERFACES

--check is the verification that matters: if the parsed method list for an
interface is exactly the list directx_vtables.py already carries - same names,
same order - then the parse understood the header, and the argument counts it read
at the same time are trustworthy.
"""
import argparse
import re
import sys

# Types wider than a pointer, which occupy two argument slots on i386. D3D9 has
# very few; anything not listed is one 4-byte slot, which is true of every
# pointer, enum, handle, float, BOOL and DWORD in these interfaces.
WIDE_TYPES = {"UINT64", "ULONGLONG", "LONGLONG", "__int64", "DWORDLONG", "double"}

# Both spellings: STDMETHOD(Name)(THIS_ ...) for HRESULT-returning methods and
# STDMETHOD_(type, Name)(THIS_ ...) for the others. Missing the opening paren of
# the first form is what made an early version of this parse silently skip every
# HRESULT method - hence --check, which compares against a list built by hand from
# the same headers and would have caught it immediately.
METHOD = re.compile(
    r"STDMETHOD(?:_\s*\(\s*(?P<ret>[^,]+?)\s*,\s*|\s*\(\s*)(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*"
    r"\(\s*(?P<this>THIS_?)(?P<params>[^;]*?)\)\s*PURE",
    re.S,
)
INTERFACE_START = re.compile(r"DECLARE_INTERFACE(?:_IID_|_)?\(\s*([A-Za-z0-9_]+)\s*,")


def split_parameters(parameters: str) -> list[str]:
    """Top-level comma split, so `void (*cb)(int, int)` stays in one piece."""
    text = parameters.strip()
    if not text:
        return []
    out, depth, current = [], 0, ""
    for character in text + ",":
        if character in "([":
            depth += 1
        elif character in ")]":
            depth -= 1
        if character == "," and depth == 0:
            if current.strip():
                out.append(current.strip())
            current = ""
        else:
            current += character
    return out


# A COM interface pointer, which cannot cross an ABI boundary untranslated: one
# star is something coming *in* that we may have handed out wrapped, two stars is
# something going *out* that has to be wrapped before the caller touches it.
INTERFACE_PARAM = re.compile(r"\b(I[A-Za-z0-9_]*?9(?:Ex)?|IDirect3D[A-Za-z0-9_]*)\s*(\*\*?)")


def classify(parameter: str) -> tuple[int, str | None, int]:
    """(slots, interface name, star count) for one parameter."""
    slots = 2 if any(w in parameter for w in WIDE_TYPES) else 1
    match = INTERFACE_PARAM.search(parameter)
    if not match:
        return slots, None, 0
    return slots, match.group(1), len(match.group(2))


def argument_slots(parameters: str) -> int:
    """Number of 4-byte stack slots the parameter list occupies, `this` aside."""
    return sum(classify(p)[0] for p in split_parameters(parameters))


def describe(parameters: str) -> list[dict]:
    """Per-parameter detail, with the stack slot each one starts at (1-based,
    `this` excluded - the thunks take `this` separately)."""
    out, slot = [], 1
    for parameter in split_parameters(parameters):
        slots, interface, stars = classify(parameter)
        out.append({
            "slot": slot,
            "slots": slots,
            "text": parameter,
            "interface": interface,
            "stars": stars,
        })
        slot += slots
    return out


def parse_detailed(header_text: str) -> dict[str, list[dict]]:
    """{interface: [{name, slots, params:[...]}, ...]} in vtable order."""
    interfaces: dict[str, list[dict]] = {}
    starts = [(m.start(), m.group(1)) for m in INTERFACE_START.finditer(header_text)]
    for index, (offset, name) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(header_text)
        body = header_text[offset:end]
        methods = []
        for match in METHOD.finditer(body):
            method_name = match.group("name")
            this_marker = match.group("this")
            parameters = match.group("params")
            described = describe(parameters) if this_marker == "THIS_" else []
            methods.append({
                "name": method_name,
                # HRESULT when STDMETHOD() carried no explicit type. The only ones
                # that matter to a thunk are float/double, which come back on the
                # x87 stack rather than in EAX.
                "returns": (match.group("ret") or "HRESULT").strip(),
                "slots": sum(p["slots"] for p in described),
                "params": described,
            })
        if methods:
            interfaces[name] = methods
    return interfaces


def parse(header_text: str) -> dict[str, list[tuple[str, int]]]:
    """{interface: [(method, argument_slots), ...]} in declaration order."""
    interfaces: dict[str, list[tuple[str, int]]] = {}
    starts = [(m.start(), m.group(1)) for m in INTERFACE_START.finditer(header_text)]
    for index, (offset, name) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(header_text)
        body = header_text[offset:end]
        methods = []
        for match in METHOD.finditer(body):
            method_name = match.group("name")
            this_marker = match.group("this")
            parameters = match.group("params")
            # `THIS` means no further arguments; `THIS_` means a list follows.
            methods.append(
                (method_name, argument_slots(parameters) if this_marker == "THIS_" else 0)
            )
        if methods:
            interfaces[name] = methods
    return interfaces


def flatten_declared(name: str) -> list[str] | None:
    """The method order directx_vtables.py already records, for --check."""
    try:
        import directx_vtables
    except ImportError:
        return None
    attribute = name.upper().replace("IDIRECT3D", "IDIRECT3D")
    for candidate in (attribute, name.upper()):
        if hasattr(directx_vtables, candidate):
            return list(getattr(directx_vtables, candidate))
    table = getattr(directx_vtables, "INTERFACES", {})
    return list(table[name]) if name in table else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", default="/usr/include/wine/windows/d3d9.h")
    parser.add_argument("--interface", action="append", default=[],
                        help="limit output to these interfaces (repeatable)")
    parser.add_argument("--check", action="store_true",
                        help="compare parsed method order against directx_vtables.py")
    parser.add_argument("--json", action="store_true",
                        help="full per-method detail, for the bridge generator")
    args = parser.parse_args()

    with open(args.header, encoding="utf-8", errors="replace") as handle:
        interfaces = parse(handle.read())

    if args.interface:
        interfaces = {k: v for k, v in interfaces.items() if k in args.interface}

    if args.json:
        import json
        with open(args.header, encoding="utf-8", errors="replace") as handle:
            detailed = parse_detailed(handle.read())
        if args.interface:
            detailed = {k: v for k, v in detailed.items() if k in args.interface}
        print(json.dumps(detailed, indent=1))
        return 0

    if args.check:
        failures = 0
        for name, methods in sorted(interfaces.items()):
            declared = flatten_declared(name)
            if declared is None:
                continue
            parsed = [m for m, _ in methods]
            # directx_vtables.py includes the inherited IUnknown methods; the
            # header declares them too, so the lists are directly comparable.
            if parsed == declared:
                print(f"ok   {name}: {len(parsed)} methods, order matches")
            else:
                failures += 1
                print(f"FAIL {name}: parsed {len(parsed)} vs declared {len(declared)}")
                for i, (a, b) in enumerate(zip(parsed, declared)):
                    if a != b:
                        print(f"       first difference at slot {i}: {a} vs {b}")
                        break
        return 1 if failures else 0

    print("# Generated by derive_vtable_args.py from the SDK header - method name")
    print("# to the number of 4-byte stack slots its arguments occupy, `this`")
    print("# excluded. Feeds define_directx_types.py so the decompiler knows how")
    print("# many bytes each __stdcall vtable call pops.")
    print("ARGUMENT_SLOTS = {")
    for name, methods in sorted(interfaces.items()):
        print(f'    "{name}": {{')
        for method, slots in methods:
            print(f'        "{method}": {slots},')
        print("    },")
    print("}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
