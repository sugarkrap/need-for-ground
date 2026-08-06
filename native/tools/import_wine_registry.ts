#!/usr/bin/env -S deno run --allow-read --allow-write
/**
 * Bring the game's own registry keys across from a Wine prefix.
 *
 * A real install writes what the game needs to read back later: its registration
 * code, its video settings, and - the reason this exists - the drive it was
 * installed from. Our shim starts with an empty store (advapi32/registry.c), so a
 * game that has been installed and played under Wine looks, natively, like one that
 * has never been installed at all. It then asks for its disc.
 *
 * This copies those keys out of the prefix's `system.reg`. Nothing is invented and
 * nothing is guessed: the values are the user's own, from their own install, and
 * they stay on their machine - `registry.ini` lives in the game directory and is
 * not part of this repository (their registration code is in it).
 *
 *     deno run -A tools/import_wine_registry.ts \
 *         --prefix /mnt/games/NFSU2/pfx --out "<game dir>/registry.ini"
 *
 * Two translations matter:
 *
 *   - **Wow6432Node is dropped.** Wine's prefix is 64-bit, so a 32-bit installer's
 *     writes to HKLM\Software land under Wow6432Node. This port is natively 32-bit
 *     and has no such redirection, and the paths the game actually asks for -
 *     visible in NFSU2_SHIM_TRACE - have no Wow6432Node in them.
 *   - **The default value** (`@=` in a .reg file) becomes an empty name, which is
 *     how RegQueryValueExA asks for it. The registration code is stored that way.
 */

interface Value {
  name: string;
  text: string;
}

const WANTED = [
  // What the trace shows the game looking for, plus the settings block it writes.
  /^Software\\(?:Wow6432Node\\)?EA GAMES\\Need for Speed Underground 2/i,
  /^Software\\(?:Wow6432Node\\)?Electronic Arts\\EA Games\\Need for Speed Underground 2/i,
];

const stripRedirection = (path: string): string =>
  path.replace(/^Software\\Wow6432Node\\/i, "Software\\");

/** Wine writes `"name"="value"` with C-style escapes; undo them. */
const unquote = (raw: string): string =>
  raw.slice(1, -1).replace(/\\(.)/g, (_, ch) => (ch === "n" ? "\n" : ch));

const parseValue = (line: string): Value | null => {
  if (line.startsWith("@=")) {
    const payload = line.slice(2).trim();
    return payload.startsWith('"') ? { name: "", text: `sz:${unquote(payload)}` } : null;
  }
  const match = /^"((?:[^"\\]|\\.)*)"\s*=\s*(.*)$/.exec(line);
  if (!match) return null;
  const name = match[1].replace(/\\(.)/g, "$1");
  const payload = match[2].trim();

  if (payload.startsWith('"')) return { name, text: `sz:${unquote(payload)}` };
  if (payload.startsWith("dword:")) return { name, text: `dword:${payload.slice(6)}` };
  if (payload.startsWith("hex")) {
    /* hex:xx,yy or hex(2):xx,yy - the type is dropped, since our store keeps
     * only the three shapes the game reads. */
    const bytes = payload.replace(/^hex(?:\([0-9a-f]+\))?:/i, "").replace(/\\\s*$/, "");
    return { name, text: `hex:${bytes.replace(/\s+/g, "")}` };
  }
  return null;
};

const main = (): number => {
  const args = Deno.args;
  const option = (flag: string, fallback: string) => {
    const index = args.indexOf(flag);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
  };
  const prefix = option("--prefix", "");
  const out = option("--out", "");

  if (!prefix || !out) {
    console.error(
      "usage: import_wine_registry.ts --prefix <wine prefix> --out <registry.ini>",
    );
    return 2;
  }

  let text: string;
  try {
    text = Deno.readTextFileSync(`${prefix}/system.reg`);
  } catch (error) {
    console.error(`cannot read ${prefix}/system.reg: ${error}`);
    return 1;
  }

  const keys = new Map<string, Value[]>();
  let current: string | null = null;
  let continued = "";

  for (const raw of text.split("\n")) {
    const line = (continued + raw).trimEnd();
    continued = "";
    if (line.endsWith("\\")) { /* a hex value split across lines */
      continued = line.slice(0, -1);
      continue;
    }
    const header = /^\[(.+?)\]/.exec(line);
    if (header) {
      const path = header[1].replace(/\\\\/g, "\\");
      current = WANTED.some((re) => re.test(path)) ? stripRedirection(path) : null;
      if (current && !keys.has(current)) keys.set(current, []);
      continue;
    }
    if (!current || !line || line.startsWith("#") || line.startsWith(";")) continue;
    const value = parseValue(line);
    if (value) keys.get(current)!.push(value);
  }

  if (keys.size === 0) {
    console.error("no NFSU2 keys in that prefix - is it the right one?");
    return 1;
  }

  const lines = [
    "# Imported from a Wine prefix by native/tools/import_wine_registry.ts.",
    "# These are your own values from your own install - a registration code is",
    "# among them, so this file belongs in the game directory, not in a repository.",
    "",
  ];
  let valueCount = 0;
  for (const [path, values] of keys) {
    lines.push(`[HKEY_LOCAL_MACHINE\\${path}]`);
    for (const value of values) {
      lines.push(`${value.name}=${value.text}`);
      valueCount++;
    }
    lines.push("");
  }
  Deno.writeTextFileSync(out, lines.join("\n"));

  console.log(`wrote ${keys.size} key(s), ${valueCount} value(s) to ${out}`);
  for (const [path, values] of keys) {
    console.log(`  ${path}  (${values.length} value(s))`);
    const drive = values.find((v) => /^CD Drive$/i.test(v.name));
    if (drive) {
      console.log(
        `      CD Drive = ${drive.text.replace(/^sz:/, "")} - map it with ` +
          `NFSU2_DRIVE_<letter>=<host dir> when running`,
      );
    }
  }
  return 0;
};

if (import.meta.main) Deno.exit(main());
