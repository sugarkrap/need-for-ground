#!/usr/bin/env -S deno run --allow-read --allow-write
/**
 * Turn Ghidra's decompiled output into compilable translation units.
 *
 * Reads native/game/manifest.yaml plus your local decompiled/ tree and writes
 * native/game/generated/:
 *
 *     <symbol>.c          the function, normalised so it compiles
 *     game_functions.h    declarations of the ported functions
 *     game_originals.h    address constants and function-pointer typedefs for
 *                         calling the ORIGINAL machine code, derived from each
 *                         recovered signature
 *
 * The last of those is what keeps the differential test free of hand-maintained
 * addresses and typedefs: adding an entry to the manifest is enough to be able
 * to call both the ported and the original version and compare them.
 *
 * The generated tree is gitignored, deliberately. Decompiled pseudocode is a
 * direct derivative of the copyrighted game binary - the same category as the
 * binary itself - so it is never committed, exactly as decompiled/ is not. What
 * lives in the repo is this tool, the manifest, and the tests.
 *
 *     deno task import
 *     deno task import -- --list      # what the manifest asks for
 *     deno task import -- --clean
 *
 * Normalisation applied, and why each is needed:
 *
 *   - an include of <nfsu2/ghidra_types.h> for undefined4/byte/uint/... and the
 *     pseudo-intrinsics (SQRT, ABS, NAN, SBORROW4, ...)
 *   - Ghidra's __cdecl / __thiscall / __fastcall annotations mapped onto the
 *     NFSU2_* attribute macros. This is not cosmetic: __thiscall means the first
 *     parameter arrives in ECX, and dropping it would silently change the ABI of
 *     every ported member function.
 *   - float10 left alone (ghidra_types.h maps it to long double, which is what
 *     the x87 80-bit intermediates in the original were)
 *   - a #line directive pointing back at the decompiled source, so a compiler
 *     error names the file you can actually go and look at
 */
import { parse as parseYaml } from "@std/yaml";
import { declarationFor, type GlobalType, recoverImageGlobals } from "./image_globals.ts";

const NATIVE = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const REPO = new URL("../..", import.meta.url).pathname.replace(/\/$/, "");
const MANIFEST = `${NATIVE}/game/manifest.yaml`;
const OUT_DIR = `${NATIVE}/game/generated`;

/**
 * Ghidra's calling-convention annotations, and the macro each maps to. The
 * mapping is the whole reason this is not a copy: see the header comment.
 */
const CONVENTIONS: Record<string, string> = {
  __cdecl: "NFSU2_CDECL",
  __stdcall: "NFSU2_STDCALL",
  __thiscall: "NFSU2_THISCALL",
  __fastcall: "NFSU2_FASTCALL",
};

interface Entry {
  address: string;
  addressValue: number;
  symbol: string;
  kind: string;
  note: string;
  reason?: string;
  /*
   * Types for globals the machine code cannot describe on its own - a pointer to
   * an interface, say, where the instructions only say "four bytes". Keyed by
   * address, as written in the manifest: { "0x870974": "IDirect3DDevice9 *" }.
   */
  globals?: Record<string, string>;
}

interface Manifest {
  functions: Entry[];
  excluded: Entry[];
}

const parseManifest = (): Manifest => {
  let text: string;
  try {
    text = Deno.readTextFileSync(MANIFEST);
  } catch {
    console.error(`manifest not found: ${MANIFEST}`);
    Deno.exit(1);
  }

  // deno-lint-ignore no-explicit-any
  const document = (parseYaml(text) ?? {}) as any;

  const normalise = (section: string, raw: unknown[]): Entry[] =>
    (raw ?? []).map((item, index) => {
      // deno-lint-ignore no-explicit-any
      const entry = item as any;
      for (const field of ["address", "symbol"]) {
        if (entry?.[field] === undefined) {
          console.error(`${MANIFEST}: ${section}[${index}] is missing '${field}'`);
          Deno.exit(1);
        }
      }
      /*
       * YAML parses 0x75c440 as a number, so keep both spellings: the value for
       * the generated code, the hex string for anything a human reads. Both
       * lists get this - an excluded entry reported in decimal is a small thing
       * that makes a report look untrustworthy.
       */
      const addressValue = typeof entry.address === "number"
        ? entry.address
        : parseInt(String(entry.address), 16);
      return {
        address: `0x${addressValue.toString(16)}`,
        addressValue,
        symbol: String(entry.symbol),
        kind: String(entry.kind ?? "game"),
        note: String(entry.note ?? ""),
        reason: entry.reason ? String(entry.reason) : undefined,
        globals: entry.globals
          ? Object.fromEntries(
            Object.entries(entry.globals).map(([key, value]) => [
              /*
               * YAML parses a bare `0x870974:` key into the number 8849268, so
               * re-reading it as hex would give the wrong address - and silently,
               * since the override would simply never match. Take the number as
               * the value it already is, and only parse strings.
               */
              `0x${(/^0x/i.test(key) ? parseInt(key, 16) : Number(key)).toString(16)}`,
              String(value),
            ]),
          )
          : undefined,
      };
    });

  return {
    functions: normalise("functions", document.functions),
    excluded: normalise("excluded", document.excluded),
  };
};

/**
 * Locate the decompiled file for a manifest entry.
 *
 * bulk_decompile.py names files <address>_<symbol>.c with the address padded to
 * eight hex digits, so the manifest's 0x43ce40 has to be normalised first.
 */
const findSource = (entry: Entry, decompiled: string): string | null => {
  const prefix = entry.addressValue.toString(16).padStart(8, "0") + "_";
  const candidates: string[] = [];
  for (const item of Deno.readDirSync(decompiled)) {
    if (item.isFile && item.name.startsWith(prefix) && item.name.endsWith(".c")) {
      candidates.push(item.name);
    }
  }
  if (candidates.length === 0) return null;
  candidates.sort();
  // One address can have several spellings if Ghidra renamed a function
  // (FID_conflict_* and friends); prefer an exact symbol match.
  const exact = candidates.find((name) =>
    name.slice(0, -2).slice(prefix.length) === entry.symbol
  );
  return `${decompiled}/${exact ?? candidates[0]}`;
};

/**
 * Return the function's declaration, as one line.
 *
 * Ghidra puts the signature after the comment block, but wraps it when it is
 * long - the return type and calling convention end up on their own line:
 *
 *     void __thiscall
 *     FUN_0048b710(float *param_1, ...)
 *
 * so collecting only the line containing '(' silently drops the return type and
 * the convention, and the declaration then defaults to int. Comments have to be
 * removed rather than skipped line by line, because Ghidra's header block puts
 * the symbol name on its own continuation line, which looks exactly like the
 * start of a declaration.
 */
const extractSignature = (text: string): string | null => {
  const stripped = text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
  const collected: string[] = [];
  let depth = 0;

  for (const line of stripped.split("\n")) {
    const trimmed = line.trim();
    if (collected.length === 0 && (trimmed === "" || trimmed.startsWith("#"))) continue;
    collected.push(trimmed);
    depth += (trimmed.match(/\(/g)?.length ?? 0) - (trimmed.match(/\)/g)?.length ?? 0);
    const joined = collected.join(" ");
    if (joined.includes("(") && depth === 0) return joined;
  }
  return null;
};

/**
 * Ghidra calls LARGE_INTEGER's nested struct `s`; Wine's headers call it `u`
 * (and also leave it anonymous). So `value.s.LowPart` does not compile. Rewritten
 * narrowly - only the two field names that can appear - rather than by replacing
 * `.s.` everywhere, which would hit any struct with a member called `s`.
 *
 * Same class of problem as the GWL_* names in win32_compat.h: the decompiler and
 * the headers disagree about a spelling, and the port has to pick one.
 */
const fixLargeIntegerFields = (text: string): string =>
  text.replace(/\.s\.LowPart\b/g, ".u.LowPart").replace(/\.s\.HighPart\b/g, ".u.HighPart");

/**
 * Bind Ghidra's address-named data symbols to the addresses their names encode.
 *
 * Any function with a `__try` - which is most of the C++ in this binary - names
 * its SEH handler as a label and its vtables as pointers:
 *
 *   puStack_8 = &LAB_00770fc8;          the exception handler MSVC emitted
 *   *param_1 = &PTR_FUN_00784680;       a vtable
 *
 * Those are not undeclared variables to be stubbed: the name *is* the address,
 * and the address is valid as soon as the PE is mapped at its own base (see
 * loader/pe_loader.c). So `&LAB_00770fc8` becomes `(void *)0x00770fc8`, which is
 * the same value the original machine code pushes.
 *
 * Only the address-of form is rewritten. A bare `DAT_00819d24` is a *read* of a
 * global whose width Ghidra did not commit to, and inventing one - dword, say -
 * would silently corrupt neighbouring data if it were a byte. Those are reported
 * and the entry is refused, in the same spirit as the excluded list: better a
 * loud "this needs a type" than a plausible wrong guess.
 *
 * `LAB_` needs one more distinction, because Ghidra uses the prefix for two
 * unrelated things. When it cannot express a jump structurally it emits a real C
 * label - `LAB_0075c4d3:` with `goto LAB_0075c4d3;`, which is how _strncpy's
 * word-at-a-time loop comes out - and that is already valid C that must be left
 * exactly as it is. Any name this file declares as a label is therefore skipped
 * entirely, rather than rewritten into an address or reported as untyped.
 */
/*
 * The leading underscore is not optional decoration: Ghidra writes `_DAT_x` when
 * the access at x is a *different size* than the symbol it defined there ("globals
 * starting with '_' overlap smaller symbols at the same address"). Both spellings
 * name storage, so both are matched - and if a file uses two widths at one address
 * the width recovery sees the conflict and refuses, which is the right answer.
 */
const IMAGE_SYMBOL = /(&\s*)?\b(_?(?:LAB|DAT|UNK|PTR)(?:_[A-Za-z][A-Za-z0-9]*)*_([0-9a-f]{8}))\b/g;

/* The address a Ghidra auto-name encodes: the trailing 8 hex digits. */
const addressOfSymbol = (name: string): number | null => {
  const match = /_([0-9a-f]{8})$/.exec(name);
  return match ? parseInt(match[1], 16) : null;
};

const collectLabels = (text: string): Set<string> => {
  const labels = new Set<string>();
  for (const match of text.matchAll(/^\s*(\w+):(?!:)/gm)) labels.add(match[1]);
  for (const match of text.matchAll(/\bgoto\s+(\w+)\s*;/g)) labels.add(match[1]);
  return labels;
};

/*
 * The extent of one function, so only its own code is disassembled: the next
 * decompiled address. bulk_decompile.py writes one file per function named after
 * its address, which makes the sorted list of those names an exact function map -
 * no heuristics about where a function ends.
 */
const functionExtent = (
  addresses: number[],
  start: number,
): { start: number; end: number } => {
  for (const address of addresses) {
    if (address > start) return { start, end: address };
  }
  /* The last function in the image: a generous window, since objdump stops at
   * the end of the section anyway. */
  return { start, end: start + 0x1000 };
};

const bindImageSymbols = (text: string): { text: string; bound: number; unbound: string[] } => {
  const labels = collectLabels(text);
  const unbound = new Set<string>();
  let bound = 0;
  const result = text.replace(IMAGE_SYMBOL, (match, ampersand, name, hex) => {
    if (labels.has(name)) return match;
    if (!ampersand) {
      unbound.add(name);
      return match;
    }
    bound++;
    return `(void *)0x${hex}`;
  });
  return { text: result, bound, unbound: [...unbound] };
};

const applyConventions = (text: string): { text: string; used: string | null } => {
  let used: string | null = null;
  let result = text;
  for (const [ghidra, macro] of Object.entries(CONVENTIONS)) {
    const pattern = new RegExp(`(?<![\\w])${ghidra}(?=[\\s\\n])`, "g");
    if (pattern.test(result)) {
      used = ghidra;
      result = result.replace(pattern, macro);
    }
  }
  return { text: result, used };
};

/*
 * Declarations for the globals a function reads, bound to their real addresses in
 * the mapped image - which is the only correct home for them: original and ported
 * code have to share the same storage, and the name already says where it is. The
 * width and the integer/float distinction come from the machine code
 * (image_globals.ts); a type the instructions cannot express - a pointer to an
 * interface, say - comes from the manifest.
 */
const declareGlobals = (
  entry: Entry,
  names: string[],
  exe: string | null,
  addresses: number[],
): { lines: string[]; unresolved: string[] } => {
  const lines: string[] = [];
  const unresolved: string[] = [];
  const overrides = entry.globals ?? {};
  let recovered = new Map<number, GlobalType>();

  const needsRecovery = names.some((name) => {
    const address = addressOfSymbol(name);
    return address !== null && overrides[`0x${address.toString(16)}`] === undefined;
  });

  if (needsRecovery && exe) {
    const extent = functionExtent(addresses, entry.addressValue);
    const result = recoverImageGlobals(exe, extent.start, extent.end);
    if (result.error) console.error(`  ${entry.symbol}: ${result.error}`);
    recovered = result.globals;
  }

  for (const name of names) {
    const address = addressOfSymbol(name);
    if (address === null) {
      unresolved.push(`${name} (no address in the name)`);
      continue;
    }
    const override = overrides[`0x${address.toString(16)}`];
    if (override) {
      lines.push(
        `#define ${name} (*(${override.replace(/\s+\*/g, " *").trim()} *)0x${
          address.toString(16).padStart(8, "0")
        }u)`,
      );
      continue;
    }
    if (!exe) {
      unresolved.push(`${name} (needs --exe to recover its width)`);
      continue;
    }
    const type = recovered.get(address);
    if (!type || !type.cType) {
      unresolved.push(`${name} (the code does not say how wide it is)`);
      continue;
    }
    if (type.sizes.length > 1) {
      unresolved.push(
        `${name} (touched at ${type.sizes.sort().join(" and ")} bytes - Ghidra's ` +
          `overlap warning, needs a union)`,
      );
      continue;
    }
    lines.push(declarationFor(name, type));
  }
  return { lines, unresolved };
};

const normaliseSource = (
  entry: Entry,
  sourcePath: string,
  exe: string | null,
  addresses: number[],
): { text: string; used: string | null; bound: number; unbound: string[] } => {
  const original = Deno.readTextFileSync(sourcePath);
  const { text: converted, used } = applyConventions(original);
  const { text: linked, bound, unbound: bare } = bindImageSymbols(fixLargeIntegerFields(converted));
  const body = linked;
  const fileName = sourcePath.split("/").pop() ?? sourcePath;
  const { lines: globalLines, unresolved: unbound } = declareGlobals(
    entry,
    bare,
    exe,
    addresses,
  );

  const header = [
    "/*",
    ` * ${entry.symbol} @ ${entry.address} - imported from decompiled output.`,
    " *",
    entry.note ? ` * ${entry.kind}: ${entry.note}` : ` * ${entry.kind}`,
    " *",
    " * GENERATED by native/tools/import_decompiled.ts - do not edit, and do not",
    " * commit: this is a derivative of the game binary. Edit the decompiled source",
    " * or the manifest instead.",
    " */",
    "#include <nfsu2/ghidra_types.h>",
    /*
     * Renderer functions reach D3D9 through a typed vtable, so they need the ABI
     * keystone header - which is also the only correct way to get those types
     * here, since it is what reconciles Wine's STDMETHODCALLTYPE with DXVK
     * Native's cdecl one (see include/nfsu2/d3d9_native.h). Included only when the
     * function actually mentions a D3D9 interface.
     */
    ...(/\bIDirect3D/.test(body) || globalLines.some((line) => /IDirect3D/.test(line))
      ? ["#include <nfsu2/d3d9_native.h>"]
      : []),
    '#include "game_functions.h"',
    // After game_functions.h, which pulls in windows.h: Wine's NT_TIB has a
    // member called ExceptionList, so the macro must not be defined first.
    "#include <nfsu2/ghidra_teb.h>",
    "",
    // Only where a bound address actually appears, and for the narrowest reason:
    // some of them are stored into slots Ghidra typed `undefined4`, which is what
    // a vtable pointer looks like to a decompiler that has not seen a class. The
    // pointer-to-integer assignment is the intended meaning, so the diagnostic
    // has nothing to say here - and stays on in every other generated file.
    ...(bound > 0
      ? [
        "/* Image addresses below are bound to literals (see",
        " * tools/import_decompiled.ts); some land in `undefined4` slots. */",
        '#pragma GCC diagnostic ignored "-Wint-conversion"',
        "",
      ]
      : []),
    ...(globalLines.length > 0
      ? [
        "/* Game globals, bound to their addresses in the mapped image. Widths and",
        " * float-ness recovered from the instructions that touch them; see",
        " * tools/image_globals.ts. */",
        ...globalLines,
        "",
      ]
      : []),
    `#line 1 "${fileName}"`,
    "",
  ].join("\n");

  return { text: header + body, used, bound, unbound };
};

/**
 * Turn a declaration into a function-pointer typedef for the original.
 *
 * `char * NFSU2_CDECL _strncpy(char *a, char *b, size_t n)` becomes
 * `typedef char * NFSU2_CDECL (*nfsu2_original__strncpy)(char *a, ...)`.
 *
 * The symbol is replaced where it is immediately followed by '(' - the same
 * place the declarator sits - so a parameter that happens to share the name
 * cannot be hit.
 */
const functionPointerTypedef = (signature: string, symbol: string): string | null => {
  const pattern = new RegExp(`(?<![\\w])${symbol}\\s*\\(`);
  if (!pattern.test(signature)) return null;
  return `typedef ${signature.replace(pattern, `(*nfsu2_original_${symbol})(`)};`;
};

const writeOriginalsHeader = (declarations: Array<[Entry, string]>): number => {
  const lines = [
    "/*",
    " * game_originals.h - how to call the ORIGINAL machine code.",
    " *",
    " * For each manifest entry: the address it lives at, and a function-pointer",
    " * typedef built from the signature Ghidra recovered - including the calling",
    " * convention, which is the part that cannot be guessed. NFSU2_ORIGINAL()",
    " * resolves one against a mapped image.",
    " *",
    " * GENERATED by native/tools/import_decompiled.ts - do not edit, do not commit.",
    " */",
    "#ifndef NFSU2_GAME_ORIGINALS_H",
    "#define NFSU2_GAME_ORIGINALS_H",
    "",
    "#include <nfsu2/ghidra_types.h>",
    "#include <nfsu2/pe_loader.h>",
    "#include <nfsu2/win32_compat.h>",
    "",
    "/* Resolve `symbol` in a mapped image, typed and ready to call. */",
    "#define NFSU2_ORIGINAL(symbol, image) \\",
    "    ((nfsu2_original_##symbol)nfsu2_pe_function((image), NFSU2_ORIGINAL_ADDR_##symbol))",
    "",
  ];

  let count = 0;
  for (const [entry, signature] of declarations) {
    const typedef = functionPointerTypedef(signature, entry.symbol);
    if (!typedef) {
      console.log(`  warning: cannot build a pointer typedef for ${entry.symbol}`);
      continue;
    }
    lines.push(`/* ${entry.address} (${entry.kind}) ${entry.note} */`);
    lines.push(`#define NFSU2_ORIGINAL_ADDR_${entry.symbol} ${entry.address}u`);
    lines.push(typedef, "");
    count++;
  }

  lines.push(
    "/* Every imported symbol, for a harness that wants to iterate. */",
    `#define NFSU2_ORIGINAL_COUNT ${count}`,
    "",
    "#endif /* NFSU2_GAME_ORIGINALS_H */",
    "",
  );
  Deno.writeTextFileSync(`${OUT_DIR}/game_originals.h`, lines.join("\n"));
  return count;
};

const writeFunctionsHeader = (declarations: Array<[Entry, string]>): void => {
  const lines = [
    "/*",
    " * game_functions.h - declarations for the imported functions.",
    " *",
    " * GENERATED by native/tools/import_decompiled.ts - do not edit, do not commit.",
    " */",
    "#ifndef NFSU2_GAME_FUNCTIONS_H",
    "#define NFSU2_GAME_FUNCTIONS_H",
    "",
    "#include <nfsu2/ghidra_types.h>",
    "#include <nfsu2/win32_compat.h>",
    "",
  ];
  for (const [entry, signature] of declarations) {
    lines.push(`/* ${entry.address} (${entry.kind}) ${entry.note} */`);
    lines.push(`${signature};`, "");
  }
  lines.push("#endif /* NFSU2_GAME_FUNCTIONS_H */", "");
  Deno.writeTextFileSync(`${OUT_DIR}/game_functions.h`, lines.join("\n"));
};

const main = (): number => {
  const args = Deno.args;
  const wants = (flag: string) => args.includes(flag);
  const option = (flag: string, fallback: string) => {
    const index = args.indexOf(flag);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
  };

  const decompiled = option("--decompiled", `${REPO}/decompiled`);
  /*
   * The exe is only needed to recover the width of untyped globals, from the
   * instructions that touch them. Nothing is read out of it into the repo, and a
   * manifest whose functions have no such globals does not need it at all.
   */
  const exe = option("--exe", Deno.env.get("NFSU2_EXE") ?? "") || null;
  const { functions, excluded } = parseManifest();

  if (wants("--list")) {
    for (const entry of functions) {
      console.log(
        `${entry.address.padStart(10)}  ${entry.symbol.padEnd(16)} ${entry.kind.padEnd(5)} ` +
          entry.note,
      );
    }
    for (const entry of excluded) {
      const reason = (entry.reason ?? "").split(/\s+/).join(" ").slice(0, 60);
      console.log(
        `${entry.address.padStart(10)}  ${entry.symbol.padEnd(16)} ${
          "EXCL".padEnd(5)
        } ${reason}...`,
      );
    }
    return 0;
  }

  if (wants("--clean")) {
    try {
      Deno.removeSync(OUT_DIR, { recursive: true });
      console.log(`removed ${OUT_DIR}`);
    } catch {
      // Nothing to remove is a success, not a failure.
    }
    return 0;
  }

  try {
    if (!Deno.statSync(decompiled).isDirectory) throw new Error("not a directory");
  } catch {
    console.error(
      `decompiled output not found: ${decompiled}\n` +
        "Run analysis/bulk_decompile.py first (see README.md step 3).",
    );
    return 1;
  }

  Deno.mkdirSync(OUT_DIR, { recursive: true });

  /*
   * Every decompiled function's address, sorted: one file per function named
   * after its address makes this an exact function map, which is how the width
   * recovery knows where a function ends.
   */
  const addresses: number[] = [];
  for (const item of Deno.readDirSync(decompiled)) {
    const match = /^([0-9a-f]{8})_/.exec(item.name);
    if (match) addresses.push(parseInt(match[1], 16));
  }
  addresses.sort((a, b) => a - b);

  const declarations: Array<[Entry, string]> = [];
  const missing: Entry[] = [];
  const untyped: Array<[Entry, string[]]> = [];
  let written = 0;

  for (const entry of functions) {
    const source = findSource(entry, decompiled);
    if (!source) {
      missing.push(entry);
      continue;
    }

    const { text, used, bound, unbound } = normaliseSource(entry, source, exe, addresses);
    if (unbound.length > 0) {
      untyped.push([entry, unbound]);
      continue;
    }
    Deno.writeTextFileSync(`${OUT_DIR}/${entry.symbol}.c`, text);
    written++;

    const signature = extractSignature(Deno.readTextFileSync(source));
    if (signature) {
      declarations.push([entry, applyConventions(signature).text.replace(/\{\s*$/, "").trim()]);
    }
    const fileName = source.split("/").pop();
    const notes = [used, bound > 0 ? `${bound} image address(es)` : null].filter(Boolean);
    console.log(
      `  ${entry.symbol.padEnd(16)} <- ${fileName}` +
        (notes.length > 0 ? `  [${notes.join(", ")}]` : ""),
    );
  }

  /*
   * Remove generated files whose manifest entry has gone. Without this the tree
   * keeps compiling code for a function that was *excluded* - meson globs whatever
   * .c files are here - so a function withdrawn because its decompilation is
   * wrong would quietly stay in the build. Found exactly that way: __SEH_prolog
   * was still being compiled two commits after it was excluded.
   */
  const expected = new Set(functions.map((entry) => `${entry.symbol}.c`));
  const stale: string[] = [];
  for (const item of Deno.readDirSync(OUT_DIR)) {
    if (item.isFile && item.name.endsWith(".c") && !expected.has(item.name)) {
      Deno.removeSync(`${OUT_DIR}/${item.name}`);
      stale.push(item.name);
    }
  }
  if (stale.length > 0) {
    console.log(`\nremoved ${stale.length} file(s) with no manifest entry:`);
    for (const name of stale) console.log(`  ${name}`);
    console.log("Re-run meson setup --reconfigure: the source list is a configure-time glob.");
  }

  writeFunctionsHeader(declarations);
  const originalCount = writeOriginalsHeader(declarations);

  console.log(`\nwrote ${written} function(s) to ${OUT_DIR.replace(REPO + "/", "")}`);
  console.log(`  ${originalCount} original-code typedef(s) in game_originals.h`);

  if (excluded.length > 0) {
    console.log(
      `\n${excluded.length} entr${excluded.length === 1 ? "y" : "ies"} deliberately excluded:`,
    );
    for (const entry of excluded) {
      const reason = (entry.reason ?? "").split(/\s+/).join(" ");
      console.log(`  ${entry.address} ${entry.symbol}: ${reason}`);
    }
  }

  if (untyped.length > 0) {
    console.log(
      `\n${untyped.length} entr${
        untyped.length === 1 ? "y" : "ies"
      } touch a global this cannot type:`,
    );
    for (const [entry, names] of untyped) {
      console.log(`  ${entry.address} ${entry.symbol}`);
      for (const name of names) console.log(`      ${name}`);
    }
    console.log(
      exe
        ? "Widths come from the instructions that touch each address; where those\n" +
          "disagree, or where the type is not something an instruction can express,\n" +
          "put it in the manifest entry's `globals:` map."
        : "Pass --exe <speed2.exe> (or set NFSU2_EXE) so widths can be recovered from\n" +
          "the machine code, or give the types in the manifest entry's `globals:` map.",
    );
    return 1;
  }

  if (missing.length > 0) {
    console.log(`\n${missing.length} not found in ${decompiled}:`);
    for (const entry of missing) console.log(`  ${entry.address} ${entry.symbol}`);
    console.log("Decompile them first, or drop them from the manifest.");
    return 1;
  }
  return 0;
};

if (import.meta.main) Deno.exit(main());
