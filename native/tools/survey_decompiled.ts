#!/usr/bin/env -S deno run --allow-read --allow-write --allow-run --allow-env
/**
 * Measure how much of the decompiled output compiles as-is.
 *
 * Answers the question the port actually needs answered: of the game's
 * functions, how many can be fed to the compiler with only the ghidra_types.h
 * harness, and for the rest, *what* stops them? Guessing at that is how a port
 * ends up with a plan based on the wrong bottleneck.
 *
 *     deno task survey                      # 300-function sample
 *     deno task survey -- --sample 500
 *     deno task survey -- --all --jobs 12
 *
 * Each function is compiled in isolation, so a failure means "this function
 * needs work", not "the build is broken". Two things are neutralised first,
 * because they are artefacts of compiling one function alone rather than port
 * blockers:
 *
 *   - calls to other FUN_ symbols, via -fpermissive: the callee simply is not
 *     declared yet, which says nothing about whether this function is portable
 *   - references to DAT_ globals, by generating `extern int` declarations. The
 *     real port needs their types recovered, but that is data-layout work, and
 *     it is counted separately when the generic declaration causes a type error.
 *
 * Failures are bucketed by cause. The buckets are deliberately specific -
 * "needs a type" and "reads an uninitialised register" are entirely different
 * amounts of work - and anything unrecognised is reported verbatim so the
 * categories grow from evidence rather than from imagination.
 *
 * Note on sampling: the shuffle is a seeded xorshift here, so a run is
 * reproducible, but it does not draw the same sample as the Python version did
 * at the same seed. Percentages are comparable; the exact file list is not.
 */
const NATIVE = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const REPO = new URL("../..", import.meta.url).pathname.replace(/\/$/, "");

/** (bucket, matcher) - order matters, first match wins. */
const BUCKETS: Array<[string, RegExp]> = [
  [
    "in_XX register read: Ghidra missed a parameter",
    /\bin_(EAX|ECX|EDX|EBX|ESI|EDI|ST0|stack)/,
  ],
  [
    "unknown width placeholder (unkbyte/unkuint/...)",
    /unkbyte|unkuint|unkfloat|undefined9|undefined1[0-9]/,
  ],
  ["field-slice syntax on a scalar (_4_4_ etc.)", /_[0-9]+_[0-9]+_/],
  [
    "missing pseudo-intrinsic",
    /implicit declaration of function .(SUB|CONCAT|ZEXT|SEXT|ROUND|NAN|ABS|SQRT|POPCOUNT|CARRY|SBORROW|BREAK|halt_baddata)/,
  ],
  [
    "call through an untyped pointer (code *)",
    /called object is not a function|n'est pas une fonction/,
  ],
  ["undeclared identifier (another function or a global)", /undeclared|non déclar/],
  [
    "type mismatch or unknown type",
    /unknown type name|nom de type|conflicting types|types conflictuels|incompatible/,
  ],
];

const HARNESS = "#include <nfsu2/ghidra_types.h>\n#include <nfsu2/win32_compat.h>\n";

/**
 * Ghidra's names for data it found but did not type: globals, string literals,
 * jump tables, and pointers to them.
 */
const DATA_SYMBOL =
  /\b((?:_?DAT|UNK|PTR(?:_[A-Za-z0-9]+)?|s|u|e|FLOAT|DOUBLE|CSWTCH)_[0-9a-f]{4,8}(?:_[0-9a-f]+)?)\b/g;

const CONVENTIONS: Array<[string, string]> = [
  ["__cdecl", "NFSU2_CDECL"],
  ["__stdcall", "NFSU2_STDCALL"],
  ["__thiscall", "NFSU2_THISCALL"],
  ["__fastcall", "NFSU2_FASTCALL"],
];

const classify = (stderr: string): string => {
  for (const [name, matcher] of BUCKETS) {
    if (matcher.test(stderr)) return name;
  }
  for (const line of stderr.split("\n")) {
    if (line.includes("error") || line.includes("erreur")) {
      const tail = line.split(":").pop()?.trim().slice(0, 70) ?? "";
      if (tail) return `other: ${tail}`;
    }
  }
  return "other";
};

const compileOne = async (path: string, wineHeaders: string): Promise<string | null> => {
  let source = Deno.readTextFileSync(path);
  for (const [ghidra, macro] of CONVENTIONS) {
    source = source.replace(new RegExp(`(?<![\\w])${ghidra}(?=[\\s\\n])`, "g"), macro);
  }

  /*
   * Declare the data symbols this function references, generically. A wrong type
   * here surfaces as a type error, which is the honest bucket for it: recovering
   * those types is real work the port has to do.
   */
  const symbols = new Set<string>();
  for (const match of source.matchAll(DATA_SYMBOL)) symbols.add(match[1]);
  const declarations = [...symbols].sort().map((name) => `extern int ${name};\n`).join("");

  const temp = await Deno.makeTempFile({ suffix: ".c" });
  try {
    Deno.writeTextFileSync(temp, HARNESS + declarations + source);
    const command = new Deno.Command("gcc", {
      args: [
        "-m32",
        // The same C dialect the build uses (project default_options in
        // meson.build). It matters more than it looks: GCC 15+ defaults to C23,
        // where `void f()` means `void f(void)` rather than "parameters
        // unspecified", so Ghidra's `typedef void code();` plus its
        // `(*(code *)x)(a, b)` vtable calls become "too many arguments" errors -
        // a portability failure invented by the survey rather than found by it.
        "-std=gnu11",
        "-c",
        "-o",
        "/dev/null",
        "-I",
        `${NATIVE}/include`,
        "-isystem",
        wineHeaders,
        "-D_GNU_SOURCE",
        "-w",
        // An undeclared callee is not a portability problem; a genuinely
        // unportable construct still fails.
        "-fpermissive",
        temp,
      ],
      stdout: "null",
      stderr: "piped",
    });
    const { code, stderr } = await command.output();
    if (code === 0) return null;
    return classify(new TextDecoder().decode(stderr));
  } finally {
    await Deno.remove(temp).catch(() => {});
  }
};

/** Deterministic shuffle, so --sample is reproducible run to run. */
const shuffle = <T>(items: T[], seed: number): T[] => {
  let state = seed || 1;
  const next = () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 4294967296;
  };
  const out = [...items];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(next() * (i + 1));
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
};

/** Run `worker` over `items` with at most `limit` in flight. */
const pool = async <T, R>(
  items: T[],
  limit: number,
  worker: (item: T) => Promise<R>,
): Promise<R[]> => {
  const results = new Array<R>(items.length);
  let next = 0;

  const runners = Array.from({ length: Math.min(limit, items.length) }, async () => {
    for (;;) {
      const index = next++;
      if (index >= items.length) return;
      results[index] = await worker(items[index]);
    }
  });
  await Promise.all(runners);
  return results;
};

const main = async (): Promise<number> => {
  const args = Deno.args;
  const flag = (name: string) => args.includes(name);
  const value = (name: string, fallback: string) => {
    const index = args.indexOf(name);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
  };

  const decompiled = value("--decompiled", `${REPO}/decompiled`);
  const wineHeaders = value("--wine-headers", "/usr/include/wine/windows");
  const sample = parseInt(value("--sample", "300"), 10);
  const jobs = parseInt(value("--jobs", "8"), 10);
  const seed = parseInt(value("--seed", "1"), 10);
  const listFailures = value("--list-failures", "");

  let files: string[];
  try {
    files = [...Deno.readDirSync(decompiled)]
      .filter((entry) => entry.isFile && entry.name.endsWith(".c"))
      .map((entry) => `${decompiled}/${entry.name}`)
      .sort();
  } catch {
    console.error(`decompiled output not found: ${decompiled}`);
    return 1;
  }
  if (files.length === 0) {
    console.error(`no .c files in ${decompiled}`);
    return 1;
  }

  const population = files.length;
  if (!flag("--all") && sample < population) files = shuffle(files, seed).slice(0, sample);

  console.log(
    `compiling ${files.length} of ${population} decompiled functions ` +
      "(-m32, ghidra_types.h harness, no linking)\n",
  );

  const outcomes = await pool(files, jobs, (path) => compileOne(path, wineHeaders));

  const buckets = new Map<string, string[]>();
  let compiled = 0;
  outcomes.forEach((bucket, index) => {
    if (bucket === null) {
      compiled++;
      return;
    }
    const name = files[index].split("/").pop()!;
    buckets.set(bucket, [...(buckets.get(bucket) ?? []), name]);
  });

  console.log(
    `compiles as-is : ${compiled}/${files.length} ` +
      `(${(100 * compiled / files.length).toFixed(1)}%)`,
  );
  console.log(`needs work     : ${files.length - compiled}\n`);
  console.log("why the rest fail:");
  for (
    const [bucket, names] of [...buckets.entries()].sort((a, b) => b[1].length - a[1].length)
  ) {
    const share = (100 * names.length / files.length).toFixed(1);
    console.log(`  ${String(names.length).padStart(5)}  (${share.padStart(4)}%)  ${bucket}`);
    if (listFailures && bucket.includes(listFailures)) {
      for (const name of names.slice(0, 40)) console.log(`           ${name}`);
    }
  }
  return 0;
};

if (import.meta.main) Deno.exit(await main());
