#!/usr/bin/env -S deno run --allow-run
/**
 * Compare the D3D9 layout as seen through Wine's headers vs DXVK Native's.
 *
 * Runs both abi_probe binaries and diffs their key/value dumps. Any mismatch
 * means a struct we pass across the boundary is laid out differently on the two
 * sides, which would corrupt data at runtime with no obvious cause.
 *
 * Keys present in only one dump are reported but not fatal: the two header sets
 * spell a few enum constants differently, and that is a naming difference, not
 * an ABI one.
 *
 *     deno run --allow-run native/tools/abi_diff.ts <wine-probe> <dxvk-probe>
 */

const dump = async (binary: string): Promise<Map<string, string>> => {
  const command = new Deno.Command(binary, { stdout: "piped", stderr: "piped" });
  const { code, stdout, stderr } = await command.output();
  if (code !== 0) {
    console.error(`${binary} exited ${code}: ${new TextDecoder().decode(stderr)}`);
    Deno.exit(2);
  }

  const result = new Map<string, string>();
  for (const line of new TextDecoder().decode(stdout).split("\n")) {
    const parts = line.trim().split(/\s+/);
    if (parts.length === 3) result.set(`${parts[0]} ${parts[1]}`, parts[2]);
  }
  return result;
};

const main = async (): Promise<number> => {
  if (Deno.args.length !== 2) {
    console.error("usage: abi_diff.ts <wine-probe> <dxvk-probe>");
    return 2;
  }

  const [wine, dxvk] = await Promise.all([dump(Deno.args[0]), dump(Deno.args[1])]);

  const shared = [...wine.keys()].filter((key) => dxvk.has(key)).sort();
  const mismatches = shared
    .filter((key) => wine.get(key) !== dxvk.get(key))
    .map((key) => ({ key, wine: wine.get(key)!, dxvk: dxvk.get(key)! }));

  const onlyWine = [...wine.keys()].filter((key) => !dxvk.has(key)).sort();
  const onlyDxvk = [...dxvk.keys()].filter((key) => !wine.has(key)).sort();

  console.log(`compared ${shared.length} shared entries`);
  for (const key of onlyWine) {
    console.log(`  note: only in wine dump:  ${key} = ${wine.get(key)}`);
  }
  for (const key of onlyDxvk) {
    console.log(`  note: only in dxvk dump:  ${key} = ${dxvk.get(key)}`);
  }

  if (mismatches.length > 0) {
    console.log(`\n${mismatches.length} ABI MISMATCH(ES):`);
    for (const { key, wine: a, dxvk: b } of mismatches) {
      console.log(`  ${key}: wine=${a} dxvk=${b}`);
    }
    return 1;
  }

  console.log("all shared sizes, offsets and constants agree");
  return 0;
};

if (import.meta.main) Deno.exit(await main());
