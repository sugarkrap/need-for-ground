/**
 * Recover the type of a game global from the machine code that touches it.
 *
 * Ghidra hands out names like `DAT_00873370` and `_DAT_0086ec98` for data it
 * found but did not type. The name carries the address, so *where* the storage
 * lives is not in doubt - and for a hybrid port it must be exactly there, because
 * original and ported code have to share the same globals. What the name does not
 * carry is how wide the access is, or whether it is integer or floating point,
 * and getting that wrong is silent corruption: a 4-byte store where the original
 * stored 1 byte overwrites three neighbouring variables, and `float x = 1.5f`
 * assigned through a `uint32_t` lvalue converts instead of reinterpreting.
 *
 * The instruction encoding says both, unambiguously:
 *
 *     mov    DWORD PTR ds:0x873370,ecx      4 bytes, integer
 *     mov    BYTE PTR ds:0x870750,0x1       1 byte,  integer
 *     mov    eax,ds:0x870974                4 bytes (from the register), integer
 *     fld    DWORD PTR ds:0x7ff14c          4 bytes, float
 *     fstp   QWORD PTR ds:0x7ff150          8 bytes, double
 *
 * so this disassembles the one function that needs it and reads the answer off
 * the operands. That is derivation from the binary, not a guess - and where the
 * binary is ambiguous (the same address touched at two different widths, which is
 * exactly what Ghidra's `_DAT_` prefix warns about) it reports the conflict
 * instead of choosing.
 */

export interface GlobalType {
  address: number;
  size: number;
  float: boolean;
  cType: string;
  /* Every distinct width seen, for the error message when they disagree. */
  sizes: number[];
}

/* x87 and SSE scalar mnemonics: these make the operand floating point. The x87
 * set is "anything starting with f" minus the handful that are not FPU
 * instructions at all. */
const NOT_FPU = new Set(["fs", "far"]);

const isFloatMnemonic = (mnemonic: string): boolean => {
  if (/^(movss|movsd|adds[sd]|subs[sd]|muls[sd]|divs[sd]|cvts[si]2s[sdi]|comis[sd]|ucomis[sd]|sqrts[sd]|maxs[sd]|mins[sd])$/.test(mnemonic)) {
    return true;
  }
  return mnemonic.startsWith("f") && !NOT_FPU.has(mnemonic);
};

/* SSE double-precision mnemonics move 8 bytes even though the operand carries no
 * size keyword. */
const isDoubleMnemonic = (mnemonic: string): boolean => /sd$/.test(mnemonic);

const SIZE_KEYWORDS: Record<string, number> = {
  BYTE: 1,
  WORD: 2,
  DWORD: 4,
  QWORD: 8,
  TBYTE: 10,
};

/* Register widths, for the forms that carry no size keyword (`mov eax,ds:0x..`). */
const REGISTER_SIZES: Record<string, number> = {
  al: 1, bl: 1, cl: 1, dl: 1, ah: 1, bh: 1, ch: 1, dh: 1,
  ax: 2, bx: 2, cx: 2, dx: 2, si: 2, di: 2, bp: 2, sp: 2,
  eax: 4, ebx: 4, ecx: 4, edx: 4, esi: 4, edi: 4, ebp: 4, esp: 4,
};

const cTypeFor = (size: number, float: boolean): string | null => {
  if (float) {
    if (size === 4) return "float";
    if (size === 8) return "double";
    if (size === 10) return "long double";
    return null;
  }
  switch (size) {
    case 1: return "uint8_t";
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    case 8: return "uint64_t";
    default: return null;
  }
};

/**
 * Disassemble [start, end) of `exe` and return what each absolutely-addressed
 * global looks like. objdump is the only external dependency, and it is already
 * needed to look at this binary at all.
 */
export const recoverImageGlobals = (
  exe: string,
  start: number,
  end: number,
): { globals: Map<number, GlobalType>; error: string | null } => {
  const globals = new Map<number, GlobalType>();
  const hex = (n: number) => "0x" + n.toString(16);
  let output: string;

  try {
    const command = new Deno.Command("objdump", {
      args: [
        "-d",
        `--start-address=${hex(start)}`,
        `--stop-address=${hex(end)}`,
        "-M",
        "intel",
        exe,
      ],
      stdout: "piped",
      stderr: "null",
    });
    const result = command.outputSync();
    if (!result.success)
      return { globals, error: `objdump failed on ${exe}` };
    output = new TextDecoder().decode(result.stdout);
  } catch (error) {
    return { globals, error: `cannot run objdump: ${error}` };
  }

  for (const line of output.split("\n")) {
    /* `  5b7a3a:\t89 0d 70 33 87 00 \tmov    DWORD PTR ds:0x873370,ecx` */
    const parts = line.split("\t");
    if (parts.length < 3) continue;
    const text = parts[2].trim();
    const mnemonic = text.split(/\s+/)[0];
    if (!mnemonic) continue;

    /* Only absolute (ds:) references are globals; ds: with a register in the
     * brackets is an indexed access to something else. */
    const references = [...text.matchAll(/(?:(BYTE|WORD|DWORD|QWORD|TBYTE)\s+PTR\s+)?ds:0x([0-9a-f]+)\b/g)];
    if (references.length === 0) continue;

    const float = isFloatMnemonic(mnemonic);

    for (const reference of references) {
      const keyword = reference[1];
      const address = parseInt(reference[2], 16);
      let size = keyword ? SIZE_KEYWORDS[keyword] : 0;

      if (!size) {
        /* No size keyword: take it from the other operand, which is a register
         * for every form that can appear here. */
        const operands = text.slice(mnemonic.length).split(",");
        for (const operand of operands) {
          const name = operand.trim().replace(/^%/, "");
          if (REGISTER_SIZES[name]) {
            size = REGISTER_SIZES[name];
            break;
          }
        }
      }
      if (!size && float && isDoubleMnemonic(mnemonic)) size = 8;
      if (!size) continue; /* nothing said how wide it is; leave it unknown */

      const existing = globals.get(address);
      if (!existing) {
        globals.set(address, {
          address,
          size,
          float,
          cType: cTypeFor(size, float) ?? "",
          sizes: [size],
        });
        continue;
      }
      if (!existing.sizes.includes(size)) existing.sizes.push(size);
      /* A float access anywhere wins: integer moves are used to copy float
       * storage around, but nothing loads an integer into the FPU. */
      if (float && !existing.float) {
        existing.float = true;
        existing.size = size;
      }
      existing.cType = cTypeFor(existing.size, existing.float) ?? "";
    }
  }

  return { globals, error: null };
};

/**
 * The C declaration for one recovered global: a macro, because the decompiled
 * code uses these names as both lvalue and rvalue.
 */
export const declarationFor = (name: string, type: GlobalType): string =>
  `#define ${name} (*(${type.cType} *)0x${type.address.toString(16).padStart(8, "0")}u)`;
