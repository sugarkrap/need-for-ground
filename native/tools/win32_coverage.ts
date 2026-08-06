#!/usr/bin/env -S deno run --allow-read
/**
 * Report Win32 shim coverage against the game's real import list.
 *
 * Cross-references analysis/win32_imports.txt (the imports the unwrapped exe
 * actually pulls in) with the entry points implemented under native/src/, so
 * "what is left to shim" is a measured number rather than a guess.
 *
 *     deno task coverage              # summary + what is missing
 *     deno task coverage -- --done    # what is implemented
 *     deno task coverage -- --extra   # implemented but not imported
 */

const NATIVE = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const REPO = new URL("../..", import.meta.url).pathname.replace(/\/$/, "");
const IMPORTS = `${REPO}/analysis/win32_imports.txt`;
const SHIM_ROOT = `${NATIVE}/src`;

/*
 * A definition looks like:  RETTYPE WINAPI Name(args) - or WINAPIV for the cdecl
 * variadic ones such as wsprintfA. The return type is never split across lines
 * in this tree, so a single-line pattern is enough.
 */
const DEFINITION = /^[A-Za-z_][\w \t*]*\bWINAPIV?\s+(\w+)\s*\(/gm;

/** Imports satisfied without a shim of ours, and by what. */
const PROVIDED_ELSEWHERE: Record<string, string> = {
  Direct3DCreate9: "DXVK Native (libdxvk_d3d9.so)",
};

/*
 * Buckets for the missing list, in the rough order a port needs them. An entry
 * matches if the name contains the substring, case-sensitively - "Rect" must not
 * swallow "Direct3DCreate9".
 */
const GROUPS: Array<[string, string[]]> = [
  ["window/message loop", [
    "Window",
    "Message",
    "Paint",
    "Cursor",
    "Rect",
    "Focus",
    "Capture",
    "Icon",
    "Class",
    "Iconic",
    "SystemMetrics",
    "DesktopWindow",
    "ForegroundWindow",
    "ActiveWindow",
  ]],
  ["gdi/text", [
    "Bitmap",
    "DC",
    "Font",
    "TextOut",
    "BkColor",
    "BkMode",
    "TextColor",
    "SelectObject",
    "DeleteObject",
    "GetPixel",
    "BitBlt",
  ]],
  ["registry", ["Reg"]],
  ["locale/CRT support", [
    "Locale",
    "CPInfo",
    "ACP",
    "OEMCP",
    "StringType",
    "CodePage",
    "CompareString",
    "LCMapString",
    "MultiByte",
    "WideChar",
    "EnvironmentStrings",
    "wsprintf",
    "wvsprintf",
    "lstrcmp",
  ]],
  ["shell/paths", [
    "SHGet",
    "ShellExecute",
    "LongPathName",
    "DiskFreeSpace",
    "DriveType",
    "LogicalDrives",
  ]],
  ["sockets/net", [
    "WSA",
    "socket",
    "bind",
    "listen",
    "accept",
    "connect",
    "recv",
    "send",
    "select",
    "shutdown",
    "sockopt",
    "hostbyname",
    "peername",
    "sockname",
    "ioctlsocket",
    "closesocket",
    "Netbios",
  ]],
  ["telephony/serial", ["line", "Comm", "Overlapped", "Setup"]],
  ["process/toolhelp", ["Toolhelp", "Process32", "ExitProcess", "TerminateProcess"]],
  ["resources", ["Resource", "FindResource", "LoadResource", "LockResource", "Sizeof"]],
  ["exceptions/unwind", ["Unwind", "Exception", "RaiseException"]],
  ["d3d/input entry points", ["Direct3DCreate9", "DirectDrawCreate", "DirectInput8Create"]],
  ["timers", ["timeSetEvent", "timeKillEvent", "timeGetDevCaps"]],
];

/** symbol -> the shim module (directory) that defines it. */
const implemented = (): Map<string, string> => {
  const found = new Map<string, string>();
  for (const module of Deno.readDirSync(SHIM_ROOT)) {
    if (!module.isDirectory) continue;
    for (const file of Deno.readDirSync(`${SHIM_ROOT}/${module.name}`)) {
      if (!file.isFile || !file.name.endsWith(".c")) continue;
      const text = Deno.readTextFileSync(`${SHIM_ROOT}/${module.name}/${file.name}`);
      for (const match of text.matchAll(DEFINITION)) found.set(match[1], module.name);
    }
  }
  return found;
};

const imported = (): Set<string> => {
  let text: string;
  try {
    text = Deno.readTextFileSync(IMPORTS);
  } catch {
    console.error(`import list not found: ${IMPORTS}`);
    Deno.exit(1);
  }
  return new Set(text.split("\n").map((line) => line.trim()).filter((line) => line));
};

const classify = (names: string[]): Array<[string, string[]]> => {
  const remaining = new Set(names);
  const out: Array<[string, string[]]> = [];

  for (const [label, needles] of GROUPS) {
    const hit = [...remaining].filter((name) =>
      needles.some((needle) => name.includes(needle))
    );
    if (hit.length > 0) {
      out.push([label, hit.sort()]);
      for (const name of hit) remaining.delete(name);
    }
  }
  if (remaining.size > 0) out.push(["other", [...remaining].sort()]);
  return out;
};

const main = (): number => {
  const wantsDone = Deno.args.includes("--done");
  const wantsExtra = Deno.args.includes("--extra");

  const have = implemented();
  const want = imported();

  const external = [...want].filter((name) => name in PROVIDED_ELSEWHERE);
  const done = [...want].filter((name) => have.has(name)).sort();
  const missing = [...want]
    .filter((name) => !have.has(name) && !(name in PROVIDED_ELSEWHERE))
    .sort();
  const extra = [...have.keys()].filter((name) => !want.has(name)).sort();

  const covered = done.length + external.length;
  console.log(`imports needed  : ${want.size}`);
  console.log(
    `implemented     : ${done.length} shimmed + ${external.length} provided externally ` +
      `= ${covered} (${(100 * covered / want.size).toFixed(1)}%)`,
  );
  for (const name of external.sort()) {
    console.log(`                  ${name} <- ${PROVIDED_ELSEWHERE[name]}`);
  }
  console.log(`still missing   : ${missing.length}`);
  if (extra.length > 0) {
    console.log(
      `shimmed but not imported: ${extra.length}` +
        " (helpers, or names the import list does not cover)",
    );
  }

  const byModule = new Map<string, number>();
  for (const name of done) {
    const module = have.get(name)!;
    byModule.set(module, (byModule.get(module) ?? 0) + 1);
  }
  console.log(
    "by shim module  : " +
      [...byModule.entries()].sort().map(([m, n]) => `${m}=${n}`).join(", "),
  );

  if (wantsDone) {
    console.log("\nimplemented:");
    for (const name of done) console.log(`  ${name.padEnd(32)} (${have.get(name)})`);
    return 0;
  }
  if (wantsExtra) {
    console.log("\nnot in the import list:");
    for (const name of extra) console.log(`  ${name}`);
    return 0;
  }

  console.log("\nmissing, grouped:");
  for (const [label, names] of classify(missing)) {
    console.log(`\n  ${label} (${names.length})`);
    for (const name of names) console.log(`    ${name}`);
  }
  return 0;
};

if (import.meta.main) Deno.exit(main());
