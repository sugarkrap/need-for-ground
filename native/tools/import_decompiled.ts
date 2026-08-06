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
}

interface Manifest {
  functions: Entry[];
  excluded: Entry[];
}

function parseManifest(): Manifest {
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
      };
    });

  return {
    functions: normalise("functions", document.functions),
    excluded: normalise("excluded", document.excluded),
  };
}

/**
 * Locate the decompiled file for a manifest entry.
 *
 * bulk_decompile.py names files <address>_<symbol>.c with the address padded to
 * eight hex digits, so the manifest's 0x43ce40 has to be normalised first.
 */
function findSource(entry: Entry, decompiled: string): string | null {
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
  const exact = candidates.find((name) => name.slice(0, -2).slice(prefix.length) === entry.symbol);
  return `${decompiled}/${exact ?? candidates[0]}`;
}

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
function extractSignature(text: string): string | null {
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
}

function applyConventions(text: string): { text: string; used: string | null } {
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
}

function normaliseSource(entry: Entry, sourcePath: string): { text: string; used: string | null } {
  const original = Deno.readTextFileSync(sourcePath);
  const { text: body, used } = applyConventions(original);
  const fileName = sourcePath.split("/").pop() ?? sourcePath;

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
    '#include "game_functions.h"',
    "",
    `#line 1 "${fileName}"`,
    "",
  ].join("\n");

  return { text: header + body, used };
}

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
function functionPointerTypedef(signature: string, symbol: string): string | null {
  const pattern = new RegExp(`(?<![\\w])${symbol}\\s*\\(`);
  if (!pattern.test(signature)) return null;
  return `typedef ${signature.replace(pattern, `(*nfsu2_original_${symbol})(`)};`;
}

function writeOriginalsHeader(declarations: Array<[Entry, string]>): number {
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
}

function writeFunctionsHeader(declarations: Array<[Entry, string]>): void {
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
}

function main(): number {
  const args = Deno.args;
  const wants = (flag: string) => args.includes(flag);
  const option = (flag: string, fallback: string) => {
    const index = args.indexOf(flag);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
  };

  const decompiled = option("--decompiled", `${REPO}/decompiled`);
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
        `${entry.address.padStart(10)}  ${entry.symbol.padEnd(16)} ${"EXCL".padEnd(5)} ${reason}...`,
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

  const declarations: Array<[Entry, string]> = [];
  const missing: Entry[] = [];
  let written = 0;

  for (const entry of functions) {
    const source = findSource(entry, decompiled);
    if (!source) {
      missing.push(entry);
      continue;
    }

    const { text, used } = normaliseSource(entry, source);
    Deno.writeTextFileSync(`${OUT_DIR}/${entry.symbol}.c`, text);
    written++;

    const signature = extractSignature(Deno.readTextFileSync(source));
    if (signature) {
      declarations.push([entry, applyConventions(signature).text.replace(/\{\s*$/, "").trim()]);
    }
    const fileName = source.split("/").pop();
    console.log(
      `  ${entry.symbol.padEnd(16)} <- ${fileName}` + (used ? `  [${used}]` : ""),
    );
  }

  writeFunctionsHeader(declarations);
  const originalCount = writeOriginalsHeader(declarations);

  console.log(`\nwrote ${written} function(s) to ${OUT_DIR.replace(REPO + "/", "")}`);
  console.log(`  ${originalCount} original-code typedef(s) in game_originals.h`);

  if (excluded.length > 0) {
    console.log(`\n${excluded.length} entr${excluded.length === 1 ? "y" : "ies"} deliberately excluded:`);
    for (const entry of excluded) {
      const reason = (entry.reason ?? "").split(/\s+/).join(" ");
      console.log(`  ${entry.address} ${entry.symbol}: ${reason}`);
    }
  }

  if (missing.length > 0) {
    console.log(`\n${missing.length} not found in ${decompiled}:`);
    for (const entry of missing) console.log(`  ${entry.address} ${entry.symbol}`);
    console.log("Decompile them first, or drop them from the manifest.");
    return 1;
  }
  return 0;
}

if (import.meta.main) Deno.exit(main());
