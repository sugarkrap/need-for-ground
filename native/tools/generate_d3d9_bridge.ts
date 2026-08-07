#!/usr/bin/env -S deno run --allow-read --allow-write --allow-run
/**
 * Generate the __stdcall-to-__cdecl thunks that let the game call DXVK Native.
 *
 * See include/nfsu2/d3d9_bridge.h for why they are needed. Every D3D9 method gets
 * one, so writing them by hand is out of the question - and generating them means
 * the *shape* is reviewed once instead of four hundred times.
 *
 * The method tables come from analysis/derive_vtable_args.py --json, which reads
 * them out of the SDK header. There is deliberately no second parser here: that
 * script's --check compares its parse against the vtable order recorded in
 * directx_vtables.py, so one verified parser feeds both Ghidra's types and this.
 *
 *     deno run -A tools/generate_d3d9_bridge.ts
 *
 * Output is committed, unlike native/game/generated/: it derives from public SDK
 * headers rather than from the game binary, and a bridge is exactly the sort of
 * thing that should be readable in review.
 *
 * How a thunk works, on i386:
 *
 *   - every argument occupies whole 4-byte stack slots, so arguments are forwarded
 *     as `unsigned` regardless of their real type. That is bit-preserving - a float
 *     argument arrives as its four bytes and leaves as the same four bytes - and it
 *     avoids parsing C types, which is where a generator like this would otherwise
 *     spend all its complexity and all its bugs.
 *   - 8-byte arguments occupy two slots; the parser counts them, so they forward as
 *     two `unsigned` and stay intact.
 *   - return values come back in EAX, which `unsigned` matches. The one exception in
 *     D3D9 is GetNPatchMode, which returns a float on the x87 stack, so that thunk
 *     is generated with a float return type.
 */
/* Paths the same way the other tools here do it, rather than pulling in @std/path
 * for two joins (it is not in the import map, and this file needs no more deps). */
const NATIVE = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const REPO = new URL("../..", import.meta.url).pathname.replace(/\/$/, "");
const join = (...parts: string[]) => parts.join("/");
const dirname = (path: string) => path.replace(/\/[^/]*$/, "");

interface Param {
  slot: number;
  slots: number;
  text: string;
  interface: string | null;
  stars: number;
}

interface Method {
  name: string;
  returns: string;
  slots: number;
  params: Param[];
}

/*
 * The interfaces worth bridging: everything D3D9 hands the game. The Ex variants
 * are included because they cost nothing and D3D9Ex objects are what DXVK actually
 * creates internally - if one ever reaches the game it will be through a slot typed
 * as the non-Ex interface, and the vtable prefix is identical.
 */
const WANTED = /^IDirect3D(?:9|Device9|SwapChain9|Resource9|Surface9|Volume9|Texture9|VolumeTexture9|CubeTexture9|BaseTexture9|VertexBuffer9|IndexBuffer9|VertexDeclaration9|VertexShader9|PixelShader9|StateBlock9|Query9)$/;

const readTables = async (): Promise<Record<string, Method[]>> => {
  const command = new Deno.Command("python3", {
    args: [join(REPO, "analysis", "derive_vtable_args.py"), "--json"],
    stdout: "piped",
    stderr: "piped",
  });
  const result = await command.output();
  if (!result.success) {
    console.error(new TextDecoder().decode(result.stderr));
    throw new Error("derive_vtable_args.py failed");
  }
  return JSON.parse(new TextDecoder().decode(result.stdout));
};

const argumentList = (slots: number): string =>
  slots === 0
    ? "struct nfsu2_d3d9_bridge *self"
    : "struct nfsu2_d3d9_bridge *self, " +
      Array.from({ length: slots }, (_, i) => `unsigned a${i + 1}`).join(", ");

const targetType = (slots: number, returns: string): string =>
  `${returns} (*)(void *${slots ? ", " + Array(slots).fill("unsigned").join(", ") : ""})`;

const callArguments = (slots: number): string =>
  slots === 0 ? "" : ", " + Array.from({ length: slots }, (_, i) => `a${i + 1}`).join(", ");

/**
 * One thunk. The interesting lines are the translations either side of the call:
 * a wrapped pointer must not reach DXVK, and a raw pointer must not reach the game.
 */
const thunk = (iface: string, method: Method, slot: number, known: Set<string>,
               descSlot = -1): string => {
  const isFloat = /^(float|double)$/i.test(method.returns);
  const returns = isFloat ? "float" : "unsigned";
  const name = `t_${iface}_${method.name}`;
  const lines: string[] = [];

  lines.push(`static ${returns} NFSU2_D3D9_THUNK ${name}(${argumentList(method.slots)})`);
  lines.push("{");
  lines.push(`    ${returns} result;`);
  lines.push("");

  /* Inbound interface pointers: the game may be handing back a bridge. */
  for (const p of method.params) {
    if (p.interface && p.stars === 1 && known.has(p.interface)) {
      lines.push(
        `    a${p.slot} = (unsigned)(uintptr_t)nfsu2_d3d9_unwrap((void *)(uintptr_t)a${p.slot});` +
          ` /* ${p.interface} * */`,
      );
    }
  }

  /*
   * Buffer locks get an audit call first - see audit_buffer_lock in d3d9_bridge.c.
   * The GetDesc slot is passed as a literal because this generator knows it and the
   * runtime would otherwise have to search for it by name.
   */
  const isBuffer = /^IDirect3D(Vertex|Index)Buffer9$/.test(iface);
  if (isBuffer && method.name === "Lock" && descSlot >= 0)
    lines.push(`    audit_buffer_lock(self, ${descSlot}, a1, a2);`);
  /* Unlock must copy a guarded lock back *before* DXVK is told it is over. */
  if (isBuffer && method.name === "Unlock")
    lines.push("    guard_unlock_pre(self);");

  /*
   * CreateDevice reprograms the FPU on Windows, and it has to happen before the
   * call because DXVK's worker threads are created inside it and inherit the x87
   * state of whoever created them. See fpu_setup in d3d9_bridge.c.
   *
   * The flags parameter is found by name rather than assumed to be the fourth,
   * so a header that spells it differently is a build error rather than a wrong
   * register: CreateDevice, CreateDeviceEx and the Ex interface all name it
   * BehaviorFlags or flags.
   */
  if (/^IDirect3D9(Ex)?$/.test(iface) && /^CreateDevice(Ex)?$/.test(method.name)) {
    const flags = method.params.find((p) => /\b(BehaviorFlags|flags)\b/.test(p.text));
    if (!flags)
      throw new Error(`${iface}::${method.name} has no behaviour-flags parameter`);
    lines.push(`    fpu_setup(a${flags.slot}); /* ${flags.text} */`);
  }

  lines.push(
    `    result = ((${targetType(method.slots, returns)})self->real_vtbl[${slot}])` +
      `(self->real${callArguments(method.slots)});`,
  );

  /*
   * State block recording depth, tracked so that a recorder left open is
   * distinguishable from a nested Begin the game ignores. See bridge.c.
   */
  if (iface === "IDirect3DDevice9" && /^(Begin|End)StateBlock$/.test(method.name)) {
    lines.push("");
    lines.push(`    state_block_${method.name === "BeginStateBlock" ? "begin" : "end"}(result);`);
  }

  /* A guarded lock swaps in our own memory once DXVK has answered. */
  if (isBuffer && method.name === "Lock" && descSlot >= 0) {
    lines.push("");
    lines.push(`    guard_lock_post(self, ${descSlot}, a1, a2, a3, result);`);
  }

  /* Outbound interface pointers: wrap before the game can call through them. */
  const outbound = method.params.filter(
    (p) => p.interface && p.stars === 2 && known.has(p.interface),
  );
  if (outbound.length > 0) {
    lines.push("");
    for (const p of outbound) {
      lines.push(`    if (result == 0 && a${p.slot}) { /* ${p.interface} ** */`);
      lines.push(`        void **out = (void **)(uintptr_t)a${p.slot};`);
      lines.push("");
      lines.push(
        `        *out = nfsu2_d3d9_wrap(*out, NFSU2_D3D9_IFACE_${p.interface});`,
      );
      lines.push("    }");
    }
  }

  /*
   * A failing call, named. Only for HRESULT returns and only on the FAILED() test:
   * plenty of D3D9 methods return a non-zero value that means something else
   * entirely - GetAdapterModeCount returns a count, TestCooperativeLevel returns
   * D3DERR_DEVICELOST as normal operation - so "non-zero is an error" would be
   * noise. This is how a silently-failed initialisation step becomes findable.
   */
  if (!isFloat && /^HRESULT$/i.test(method.returns)) {
    lines.push("");
    lines.push("    if (result & 0x80000000u)");
    lines.push(
      `        nfsu2_shim_trace("d3d9 FAILED 0x%08x  ${iface}::${method.name}", result);`,
    );
  }

  lines.push("    return result;");
  lines.push("}");
  return lines.join("\n");
};

const main = async (): Promise<number> => {
  const tables = await readTables();
  const interfaces = Object.keys(tables).filter((name) => WANTED.test(name)).sort();
  const known = new Set(interfaces);

  if (interfaces.length === 0) {
    console.error("no D3D9 interfaces in the parsed header");
    return 1;
  }

  /* The enum, in its own header so d3d9_bridge.h can build it with a macro. */
  const ifaceLines = [
    "/*",
    " * GENERATED by native/tools/generate_d3d9_bridge.ts - do not edit.",
    " * One line per bridged interface; the including file defines NFSU2_D3D9_IFACE.",
    " */",
    ...interfaces.map((name) => `NFSU2_D3D9_IFACE(${name})`),
    "",
  ];
  Deno.writeTextFileSync(
    join(NATIVE, "include", "nfsu2", "d3d9_bridge_ifaces.h"),
    ifaceLines.join("\n"),
  );

  const out: string[] = [
    "/*",
    " * GENERATED by native/tools/generate_d3d9_bridge.ts - do not edit.",
    " *",
    " * One __stdcall + force_align_arg_pointer thunk per D3D9 method, forwarding to",
    " * DXVK Native's __cdecl vtable, plus a vtable of them per interface. Arguments",
    " * forward as raw 4-byte slots (bit-preserving on i386, and no C type parsing);",
    " * COM pointers are translated in both directions. See d3d9_bridge.c for the",
    " * runtime and include/nfsu2/d3d9_bridge.h for why any of this is necessary.",
    " */",
    "",
  ];

  let methodCount = 0;
  let wrapCount = 0;
  let unwrapCount = 0;

  for (const iface of interfaces) {
    const methods = tables[iface];
    out.push(`/* --- ${iface}: ${methods.length} methods ------------------------ */`);
    out.push("");
    /* Slots 0..2 are IUnknown's, which the runtime handles generically: refcounting
     * has to own the bridge's lifetime, and QueryInterface has to be told about. */
    for (let slot = 3; slot < methods.length; slot++) {
      const method = methods[slot];
      out.push(thunk(iface, method, slot, known,
                     methods.findIndex((m) => m.name === "GetDesc")));
      out.push("");
      methodCount++;
      for (const p of method.params) {
        if (p.interface && known.has(p.interface)) {
          if (p.stars === 2) wrapCount++;
          else if (p.stars === 1) unwrapCount++;
        }
      }
    }

    out.push(`static const void *g_vtbl_${iface}[] = {`);
    out.push("    (const void *)nfsu2_d3d9_QueryInterface,");
    out.push("    (const void *)nfsu2_d3d9_AddRef,");
    out.push("    (const void *)nfsu2_d3d9_Release,");
    for (let slot = 3; slot < methods.length; slot++)
      out.push(`    (const void *)t_${iface}_${methods[slot].name},`);
    out.push("};");
    out.push("");
  }

  /* The wrap dispatch: interface id to vtable. */
  out.push("static const void **g_vtables[NFSU2_D3D9_IFACE_COUNT] = {");
  out.push("    NULL, /* NFSU2_D3D9_IFACE_NONE */");
  for (const iface of interfaces) out.push(`    g_vtbl_${iface},`);
  out.push("};");
  out.push("");
  out.push("static const char *const g_iface_names[NFSU2_D3D9_IFACE_COUNT] = {");
  out.push('    "none",');
  for (const iface of interfaces) out.push(`    "${iface}",`);
  out.push("};");
  out.push("");

  const path = join(NATIVE, "src", "d3d9_bridge", "bridge_generated.h");
  Deno.mkdirSync(dirname(path), { recursive: true });
  Deno.writeTextFileSync(path, out.join("\n"));

  console.log(`wrote ${interfaces.length} interface(s), ${methodCount} thunk(s)`);
  console.log(`  ${wrapCount} interface out-parameter(s) wrapped`);
  console.log(`  ${unwrapCount} interface in-parameter(s) unwrapped`);
  console.log(`  ${path.replace(REPO + "/", "")}`);
  for (const iface of interfaces)
    console.log(`  ${iface.padEnd(28)} ${tables[iface].length} methods`);
  return 0;
};

if (import.meta.main) Deno.exit(await main());
