#!/usr/bin/env python3
"""Emulate an NFS Underground 2 exe's SafeDisc stub with Unicorn to trace its
disc-check / self-extraction flow and locate the real OEP.

This is the "uncompile" half of the workflow: it doesn't decrypt .text on its
own (that needs the disc-verification-derived key, which the emulated stub
computes via a bespoke checksum routine we have not independently solved -
see NOTES.md). What it does do reliably: run the stub far enough, with a
faked-but-consistent Win32 environment, to observe its control flow, resolve
its dynamically-loaded helper DLL, and find the address it jumps to once its
checks pass (the real entry point once decrypted) - which is the number
`tools/unwrap.py --entry-rva` needs.

Usage:
    python3 emulate_stub.py --exe /path/to/speed2.exe \\
        --real-kernel32 /path/to/a/real/32-bit/kernel32.dll

The kernel32.dll only needs to be a genuine PE32 kernel32.dll (e.g. from a
Wine/Proton install's i386-windows tree) - it's mapped passively so code that
manually walks its export table (instead of calling GetProcAddress) sees
valid structures, without us ever actually executing real kernel32 internals.
"""
import argparse
import struct

import pefile
from unicorn import *
from unicorn.x86_const import *

PAGE = 0x1000


def align_up(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def build_emulator(exe_path: str, real_kernel32_path: str):
    pe = pefile.PE(exe_path)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    entry = image_base + pe.OPTIONAL_HEADER.AddressOfEntryPoint

    uc = Uc(UC_ARCH_X86, UC_MODE_32)

    image_size = align_up(pe.OPTIONAL_HEADER.SizeOfImage)
    uc.mem_map(image_base, image_size, UC_PROT_ALL)
    with open(exe_path, "rb") as f:
        data = f.read()
    uc.mem_write(image_base, data[: pe.OPTIONAL_HEADER.SizeOfHeaders])
    for s in pe.sections:
        raw = data[s.PointerToRawData: s.PointerToRawData + s.SizeOfRawData]
        uc.mem_write(image_base + s.VirtualAddress, raw)

    # --- stack ---
    stack_base = 0x00100000
    stack_size = 0x00100000
    uc.mem_map(stack_base, stack_size, UC_PROT_ALL)
    stack_top = stack_base + stack_size - PAGE

    # --- fake TEB/PEB so fs:[0] + PEB->BeingDebugged don't fault ---
    teb_addr = 0x00200000
    peb_addr = 0x00201000
    uc.mem_map(teb_addr, 0x2000, UC_PROT_ALL)
    uc.mem_write(teb_addr, b"\x00" * 0x1000)
    uc.mem_write(peb_addr, b"\x00" * 0x1000)
    uc.mem_write(teb_addr + 0x30, struct.pack("<I", peb_addr))  # TEB.ProcessEnvironmentBlock
    uc.mem_write(peb_addr + 0x02, b"\x00")  # BeingDebugged = 0
    uc.mem_write(teb_addr + 0x04, struct.pack("<I", stack_base + stack_size))
    uc.mem_write(teb_addr + 0x08, struct.pack("<I", stack_base))

    # Real GDT with a proper FS descriptor -> TEB. The UC_X86_REG_FS_BASE
    # pseudo-register alone does NOT work for 32-bit fs:-prefixed access in
    # this Unicorn build (verified via isolated test) - CS/DS/ES/SS get
    # reloaded too since a real segment load requires it.
    gdt_addr = 0x00300000
    uc.mem_map(gdt_addr, PAGE, UC_PROT_ALL)

    def gdt_entry(base, limit, access, flags):
        e = limit & 0xFFFF
        e |= (base & 0xFFFFFF) << 16
        e |= (access & 0xFF) << 40
        e |= ((limit >> 16) & 0xF) << 48
        e |= (flags & 0xF) << 52
        e |= ((base >> 24) & 0xFF) << 56
        return struct.pack("<Q", e)

    def selector(idx, rpl=0):
        return (idx << 3) | rpl

    gdt = (
        b"\x00" * 8
        + gdt_entry(0, 0xFFFFF, 0x9A, 0xC)  # 1: flat code
        + gdt_entry(0, 0xFFFFF, 0x92, 0xC)  # 2: flat data
        + gdt_entry(teb_addr, 0xFFFFF, 0x92, 0xC)  # 3: fs -> TEB
    )
    uc.mem_write(gdt_addr, gdt)
    uc.reg_write(UC_X86_REG_GDTR, (0, gdt_addr, len(gdt) - 1, 0))
    uc.reg_write(UC_X86_REG_CS, selector(1))
    uc.reg_write(UC_X86_REG_DS, selector(2))
    uc.reg_write(UC_X86_REG_ES, selector(2))
    uc.reg_write(UC_X86_REG_SS, selector(2))
    uc.reg_write(UC_X86_REG_FS, selector(3))

    uc.reg_write(UC_X86_REG_ESP, stack_top)
    uc.reg_write(UC_X86_REG_EBP, stack_top)
    uc.reg_write(UC_X86_REG_EIP, entry)

    state = Emulator(
        uc=uc, pe=pe, image_base=image_base, entry=entry, image_size=image_size,
        data=data, stack_top=stack_top, teb_addr=teb_addr,
    )
    state.setup_hooks(real_kernel32_path)
    return state


class Emulator:
    FAKE_MODULE_HANDLE = 0x99999999
    HOOK_BASE = 0xF0000000
    TRAMPOLINE_MARKER = 0xE0000000

    def __init__(self, uc, pe, image_base, entry, image_size, data, stack_top, teb_addr):
        self.uc = uc
        self.pe = pe
        self.image_base = image_base
        self.entry = entry
        self.image_size = image_size
        self.data = data
        self.stack_top = stack_top
        self.teb_addr = teb_addr

        self.hook_slots = {}
        self.name_to_addr = {}
        self.next_slot = 0
        self.named_files = {}
        self.loaded_modules = {}
        self.next_module_base = 0x10000000
        self.pending_resumes = []
        self.file_handles = {}
        self.alloc_sizes = {}
        self.tls_slots = {}
        self.named_mappings = {}
        self.mapping_objs = {}
        self.mapped_views = {}
        self.call_log = []
        self.heap_ptr = 0x02000000
        self.next_handle = 0x1000
        self.tls_next = 1
        self.gfa_seen = {}
        self.stop_reason = None
        self.recent_addrs = []

        self.uc.mem_map(self.HOOK_BASE, 0x00100000, UC_PROT_ALL)
        self.uc.mem_map(self.TRAMPOLINE_MARKER, PAGE, UC_PROT_ALL)
        self.uc.mem_map(0x02000000, 0x02000000, UC_PROT_ALL)

    # ---- helpers ----
    def alloc_hook(self, name):
        key = name.lower()
        if key in self.name_to_addr:
            return self.name_to_addr[key]
        addr = self.HOOK_BASE + self.next_slot * 0x10
        self.next_slot += 1
        self.uc.mem_write(addr, b"\xCC" * 0x10)
        self.hook_slots[addr] = name
        self.name_to_addr[key] = addr
        return addr

    @staticmethod
    def norm_path(p):
        p = p.lower()
        while "\\\\" in p:
            p = p.replace("\\\\", "\\")
        return p

    def read_cstr(self, addr, maxlen=256):
        if addr == 0:
            return ""
        b = self.uc.mem_read(addr, maxlen)
        n = b.find(b"\x00")
        if n >= 0:
            b = b[:n]
        return b.decode("latin1", errors="replace")

    def get_args(self, n):
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        return [
            struct.unpack("<I", self.uc.mem_read(esp + 4 + i * 4, 4))[0]
            for i in range(n)
        ]

    def stdcall_return(self, nargs, retval):
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        ret_addr = struct.unpack("<I", self.uc.mem_read(esp, 4))[0]
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4 + nargs * 4)
        self.uc.reg_write(UC_X86_REG_EAX, retval & 0xFFFFFFFF)
        self.uc.reg_write(UC_X86_REG_EIP, ret_addr)

    def load_pe_module(self, raw_bytes):
        try:
            mpe = pefile.PE(data=bytes(raw_bytes))
        except Exception as e:
            print(f"  (failed to parse extracted file as PE: {e})")
            return None
        base = self.next_module_base
        img_size = align_up(max(mpe.OPTIONAL_HEADER.SizeOfImage, PAGE))
        self.next_module_base += img_size + PAGE
        try:
            self.uc.mem_map(base, img_size, UC_PROT_ALL)
        except UcError as e:
            print(f"  mem_map failed for module at {hex(base)}: {e}")
            return None
        self.uc.mem_write(base, mpe.get_memory_mapped_image(ImageBase=base))
        if hasattr(mpe, "DIRECTORY_ENTRY_IMPORT"):
            for entry in mpe.DIRECTORY_ENTRY_IMPORT:
                for imp in entry.imports:
                    nm = imp.name.decode() if imp.name else f"{entry.dll.decode()}!ord{imp.ordinal}"
                    addr = self.alloc_hook(nm)
                    rva = imp.address - mpe.OPTIONAL_HEADER.ImageBase
                    self.uc.mem_write(base + rva, struct.pack("<I", addr))
        self.loaded_modules[base] = mpe
        print(f"  loaded real PE module at base {hex(base)} "
              f"(entry {hex(base + mpe.OPTIONAL_HEADER.AddressOfEntryPoint)})")
        return base

    def call_into_emulation(self, entry, args, on_return):
        """Redirect execution to `entry` from within a hook, landing back at
        TRAMPOLINE_MARKER when it returns - avoids nested emu_start, which has
        known quirks around shared internal stop-state (observed firsthand:
        an outer emu_start silently ended ~23M instructions in, right after a
        nested call completed, no exception, no hit limit)."""
        saved_esp = self.uc.reg_read(UC_X86_REG_ESP)
        new_esp = saved_esp - 0x4000
        self.uc.mem_write(new_esp, struct.pack("<I", self.TRAMPOLINE_MARKER))
        for i, a in enumerate(args):
            self.uc.mem_write(new_esp + 4 + i * 4, struct.pack("<I", a))
        self.uc.reg_write(UC_X86_REG_ESP, new_esp)
        self.uc.reg_write(UC_X86_REG_EIP, entry)

        def resume():
            self.uc.reg_write(UC_X86_REG_ESP, saved_esp)
            on_return()

        self.pending_resumes.append(resume)

    def run_dllmain(self, base, entry, on_return):
        print(f"  running DllMain at {hex(entry)} ...")

        def done():
            print("  DllMain returned")
            on_return()

        self.call_into_emulation(entry, [base, 1, 0], done)

    def run_thread_sync(self, start_addr, param, on_done):
        self.call_into_emulation(start_addr, [param], on_done)

    # ---- hook setup ----
    def setup_hooks(self, real_kernel32_path):
        uc = self.uc

        try:
            k32pe = pefile.PE(real_kernel32_path)
            k32_size = align_up(max(k32pe.OPTIONAL_HEADER.SizeOfImage, PAGE))
            uc.mem_map(0x77000000, k32_size, UC_PROT_ALL)
            uc.mem_write(0x77000000, k32pe.get_memory_mapped_image(ImageBase=0x77000000))
            print(f"Mapped real kernel32.dll headers/data at 0x77000000 ({k32_size} bytes)")
            if hasattr(k32pe, "DIRECTORY_ENTRY_EXPORT"):
                for sym in k32pe.DIRECTORY_ENTRY_EXPORT.symbols:
                    if sym.name:
                        self.hook_slots[0x77000000 + sym.address] = sym.name.decode(errors="replace")
                uc.hook_add(UC_HOOK_CODE, self._api_dispatch, begin=0x77000000, end=0x77000000 + k32_size - 1)
                print(f"Redirecting {len(k32pe.DIRECTORY_ENTRY_EXPORT.symbols)} real kernel32 "
                      "export addresses through the dispatcher (code that manually walks "
                      "kernel32's own export table instead of calling GetProcAddress)")
        except Exception as e:
            print(f"Could not map real kernel32.dll ({real_kernel32_path}): {e}")

        if hasattr(self.pe, "DIRECTORY_ENTRY_IMPORT"):
            for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
                for imp in entry.imports:
                    nm = imp.name.decode() if imp.name else f"{entry.dll.decode()}!ord{imp.ordinal}"
                    addr = self.alloc_hook(nm)
                    uc.mem_write(imp.address, struct.pack("<I", addr))

        uc.hook_add(UC_HOOK_CODE, self._api_dispatch, begin=self.HOOK_BASE, end=self.HOOK_BASE + 0x00100000 - 1)
        uc.hook_add(UC_HOOK_CODE, self._trampoline_hook, begin=self.TRAMPOLINE_MARKER, end=self.TRAMPOLINE_MARKER + 0xF)

        text = self.pe.sections[0]
        self.text_start = self.image_base + text.VirtualAddress
        self.text_end = self.text_start + text.Misc_VirtualSize
        self.first_text_hit = {"addr": None, "count": 0}
        uc.hook_add(UC_HOOK_CODE, self._text_entry_hook, begin=self.text_start, end=self.text_end - 1)

        self.instr_count = 0
        uc.hook_add(UC_HOOK_CODE, self._count_hook)

    def _trampoline_hook(self, uc, address, size, user_data):
        if self.pending_resumes:
            self.pending_resumes.pop()()
        else:
            print("!! trampoline hit with no pending resume, stopping")
            uc.emu_stop()

    def _text_entry_hook(self, uc, address, size, user_data):
        if self.first_text_hit["addr"] is None:
            self.first_text_hit["addr"] = address
            print(f"\n*** First entry into .text at {hex(address)} after "
                  f"{len(self.call_log)} API calls ***\n")
        self.first_text_hit["count"] += 1

    def _count_hook(self, uc, address, size, user_data):
        self.instr_count += 1
        self.recent_addrs.append(address)
        if len(self.recent_addrs) > 30:
            self.recent_addrs.pop(0)

    def _api_dispatch(self, uc, address, size, user_data):
        name = self.hook_slots.get(address)
        if name is None:
            return
        lname = name.lower()
        self.call_log.append(name)
        if len(self.call_log) % 500 == 0:
            print(f"... {len(self.call_log)} api calls so far, last={name}")
        handler = getattr(self, f"_api_{lname}", None)
        if handler:
            handler()
        elif lname in ARGC:
            self.stdcall_return(ARGC[lname], 1)
        else:
            print(f"!! UNHANDLED API {name} - returning 0 with 0 args popped (may desync stack)")
            self.stdcall_return(0, 0)

    # ---- individual API handlers (kept as _api_<lowercased name>) ----
    def _api_getmodulehandlea(self):
        (lp,) = self.get_args(1)
        s = self.read_cstr(lp) if lp else "(null)"
        self.stdcall_return(1, self.image_base if lp == 0 else 0x77000000)

    def _api_getprocaddress(self):
        hmod, lpname = self.get_args(2)
        if hmod == self.FAKE_MODULE_HANDLE:
            self.stdcall_return(2, 0)
            return
        if hmod in self.loaded_modules:
            mpe = self.loaded_modules[hmod]
            found = 0
            fname = f"ord{lpname}" if lpname < 0x10000 else self.read_cstr(lpname)
            if hasattr(mpe, "DIRECTORY_ENTRY_EXPORT"):
                for sym in mpe.DIRECTORY_ENTRY_EXPORT.symbols:
                    if lpname < 0x10000:
                        if sym.ordinal == lpname:
                            found = hmod + sym.address
                            break
                    elif sym.name and sym.name.decode(errors="replace") == fname:
                        found = hmod + sym.address
                        break
            self.stdcall_return(2, found)
            return
        fname = f"ord{lpname}" if lpname < 0x10000 else self.read_cstr(lpname)
        self.stdcall_return(2, self.alloc_hook(fname))

    def _api_globalalloc(self):
        flags, size = self.get_args(2)
        ptr = self.heap_ptr
        self.heap_ptr += align_up(max(size, 0x10))
        self.stdcall_return(2, ptr)

    def _api_globalfree(self):
        self.get_args(1)
        self.stdcall_return(1, 0)

    def _api_exitprocess(self):
        (code,) = self.get_args(1)
        self.stop_reason = f"ExitProcess({code})"
        print(f"ExitProcess({code}) called")
        self.uc.emu_stop()

    def _api_messageboxa(self):
        hwnd, lptext, lpcaption, utype = self.get_args(4)
        self.stop_reason = f"MessageBoxA: {self.read_cstr(lpcaption)!r} / {self.read_cstr(lptext)!r}"
        print(self.stop_reason)
        self.uc.emu_stop()

    def _api_createfilea(self):
        args = self.get_args(7)
        fname = self.read_cstr(args[0])
        key = self.norm_path(fname)
        # the game's own self-extraction logic hardcodes "speed2.exe" as its own
        # filename internally, regardless of what we happen to name the file on
        # disk while testing - match on that, not our test file's actual name.
        if fname.lower().endswith("speed2.exe"):
            buf = bytearray(self.data)
        elif key in self.named_files:
            buf = self.named_files[key]
        else:
            buf = bytearray()
            self.named_files[key] = buf
        h = self.next_handle
        self.next_handle += 1
        self.file_handles[h] = {"data": buf, "pos": 0}
        print(f"CreateFileA({fname!r}) -> handle {h} ({len(buf)} bytes backing)")
        self.stdcall_return(7, h)

    def _api_getfilesize(self):
        h, lphigh = self.get_args(2)
        fh = self.file_handles.get(h)
        size = len(fh["data"]) if fh else 0
        if lphigh:
            self.uc.mem_write(lphigh, struct.pack("<I", 0))
        self.stdcall_return(2, size)

    def _api_setfilepointer(self):
        h, dist, lphigh, method = self.get_args(4)
        fh = self.file_handles.get(h)
        if fh:
            dist_s = dist - 0x100000000 if dist >= 0x80000000 else dist
            if method == 0:
                fh["pos"] = dist_s
            elif method == 1:
                fh["pos"] += dist_s
            elif method == 2:
                fh["pos"] = len(fh["data"]) + dist_s
            newpos = fh["pos"]
        else:
            newpos = 0
        self.stdcall_return(4, newpos & 0xFFFFFFFF)

    def _api_readfile(self):
        h, lpbuf, ntoread, lpread, lpoverlapped = self.get_args(5)
        fh = self.file_handles.get(h)
        nread = 0
        if fh:
            chunk = fh["data"][fh["pos"]: fh["pos"] + ntoread]
            nread = len(chunk)
            if lpbuf and nread:
                self.uc.mem_write(lpbuf, bytes(chunk))
            fh["pos"] += nread
        if lpread:
            self.uc.mem_write(lpread, struct.pack("<I", nread))
        self.stdcall_return(5, 1)

    def _api_writefile(self):
        h, lpbuf, ntowrite, lpwritten, lpoverlapped = self.get_args(5)
        fh = self.file_handles.get(h)
        nwritten = 0
        if fh and lpbuf:
            chunk = bytes(self.uc.mem_read(lpbuf, ntowrite))
            end = fh["pos"] + ntowrite
            if end > len(fh["data"]):
                fh["data"].extend(b"\x00" * (end - len(fh["data"])))
            fh["data"][fh["pos"]:end] = chunk
            fh["pos"] = end
            nwritten = ntowrite
        if lpwritten:
            self.uc.mem_write(lpwritten, struct.pack("<I", nwritten))
        self.stdcall_return(5, 1)

    def _api_closehandle(self):
        (h,) = self.get_args(1)
        self.file_handles.pop(h, None)
        self.stdcall_return(1, 1)

    def _api_deviceiocontrol(self):
        args = self.get_args(8)
        if args[6]:
            self.uc.mem_write(args[6], struct.pack("<I", 0))
        self.stdcall_return(8, 1)

    def _api_getdrivetypea(self):
        self.get_args(1)
        self.stdcall_return(1, 5)  # DRIVE_CDROM

    def _api_getvolumeinformationa(self):
        args = self.get_args(8)
        lpVolNameBuf, nVolNameSize = args[1], args[2]
        label = b"NFSUG2_DISK1\x00"
        if lpVolNameBuf:
            self.uc.mem_write(lpVolNameBuf, label[:nVolNameSize])
        if args[3]:
            self.uc.mem_write(args[3], struct.pack("<I", 0x12345678))
        self.stdcall_return(8, 1)

    def _api_virtualalloc(self):
        args = self.get_args(4)
        addr, size = args[0], args[1] or 0x1000
        chosen = addr if addr else self.heap_ptr
        try:
            self.uc.mem_map(align_up(chosen, PAGE) - PAGE if chosen % PAGE else chosen, align_up(size))
        except UcError:
            pass
        if not addr:
            self.heap_ptr += align_up(size)
        self.stdcall_return(4, chosen)

    def _api_virtualprotect(self):
        args = self.get_args(4)
        if args[3]:
            self.uc.mem_write(args[3], struct.pack("<I", 0x40))
        self.stdcall_return(4, 1)

    def _api_virtualfree(self):
        self.stdcall_return(3, 1)

    def _api_getlasterror(self):
        self.stdcall_return(0, 0)

    def _api_getcurrentprocess(self):
        self.stdcall_return(0, 0xFFFFFFFF)

    def _api_getcurrentprocessid(self):
        self.stdcall_return(0, 4444)

    def _api_getcurrentthreadid(self):
        self.stdcall_return(0, 4445)

    def _api_getversion(self):
        self.stdcall_return(0, 0x0A280004)

    def _api_getversionexa(self):
        self.get_args(1)
        self.stdcall_return(1, 1)

    def _api_gettickcount(self):
        self.stdcall_return(0, 123456)

    def _api_isdebuggerpresent(self):
        self.stdcall_return(0, 0)

    def _api_outputdebugstringa(self):
        (lp,) = self.get_args(1)
        print(f"OutputDebugStringA: {self.read_cstr(lp)!r}")
        self.stdcall_return(1, 0)

    def _api_gettemppatha(self):
        nsize, lpbuf = self.get_args(2)
        path = b"C:\\temp\\\x00"
        if lpbuf and nsize >= len(path):
            self.uc.mem_write(lpbuf, path)
        self.stdcall_return(2, len(path) - 1)

    def _api_getmodulefilenamea(self):
        hmod, lpfn, nsize = self.get_args(3)
        path = self.exe_path_full().encode() + b"\x00"
        towrite = path[:nsize] if nsize < len(path) else path
        if lpfn:
            self.uc.mem_write(lpfn, towrite)
        self.stdcall_return(3, len(towrite) - 1)

    def exe_path_full(self):
        return getattr(
            self, "_exe_full_path",
            "C:\\Program Files (x86)\\EA GAMES\\Need for Speed Underground 2\\speed2.exe",
        )

    def _api_getwindowsdirectorya(self):
        lpbuf, nsize = self.get_args(2)
        path = b"C:\\windows\x00"
        if lpbuf and nsize >= len(path):
            self.uc.mem_write(lpbuf, path)
        self.stdcall_return(2, len(path) - 1)

    def _api_getfileattributesa(self):
        (lp,) = self.get_args(1)
        self.stdcall_return(1, 0xFFFFFFFF)  # default: not found

    def _api_waitforsingleobject(self):
        self.stdcall_return(2, 0)  # WAIT_OBJECT_0

    def _api_heapcreate(self):
        self.stdcall_return(3, 0x50000000)

    def _api_heapdestroy(self):
        self.stdcall_return(1, 1)

    def _api_heapalloc(self):
        hheap, flags, nbytes = self.get_args(3)
        ptr = self.heap_ptr
        self.heap_ptr += align_up(max(nbytes, 0x10))
        if flags & 0x8:
            self.uc.mem_write(ptr, b"\x00" * nbytes)
        self.alloc_sizes[ptr] = nbytes
        self.stdcall_return(3, ptr)

    def _api_heaprealloc(self):
        hheap, flags, lpmem, nbytes = self.get_args(4)
        newptr = self.heap_ptr
        self.heap_ptr += align_up(max(nbytes, 0x10))
        oldsize = self.alloc_sizes.get(lpmem, 0)
        if oldsize:
            self.uc.mem_write(newptr, bytes(self.uc.mem_read(lpmem, min(oldsize, nbytes))))
        self.alloc_sizes[newptr] = nbytes
        self.stdcall_return(4, newptr)

    def _api_heapfree(self):
        self.stdcall_return(3, 1)

    def _api_heapsize(self):
        hheap, flags, lpmem = self.get_args(3)
        self.stdcall_return(3, self.alloc_sizes.get(lpmem, 0))

    def _api_getprocessheap(self):
        self.stdcall_return(0, 0x50000000)

    def _api_createthread(self):
        args = self.get_args(6)
        _attr, _stacksize, start_addr, param, _flags, lptid = args
        h = self.next_handle
        self.next_handle += 1
        if lptid:
            self.uc.mem_write(lptid, struct.pack("<I", 5000 + h))
        self.run_thread_sync(start_addr, param, lambda: self.stdcall_return(6, h))

    def _api_flushinstructioncache(self):
        self.stdcall_return(3, 1)

    def _api_sethandlecount(self):
        (n,) = self.get_args(1)
        self.stdcall_return(1, n)

    def _api_isprocessorfeaturepresent(self):
        self.stdcall_return(1, 0)

    def _api_getstringtypew(self):
        infotype, lpsrc, cchsrc, lpchartype = self.get_args(4)
        if lpchartype and cchsrc > 0:
            self.uc.mem_write(lpchartype, b"\x01\x00" * cchsrc)
        self.stdcall_return(4, 1)

    def _api_getstringtypea(self):
        locale, infotype, lpsrc, cchsrc, lpchartype = self.get_args(5)
        if lpchartype and cchsrc > 0:
            self.uc.mem_write(lpchartype, b"\x01\x00" * cchsrc)
        self.stdcall_return(5, 1)

    def _lcmapstring(self, wide):
        locale, flags, lpsrc, cchsrc, lpdest, cchdest = self.get_args(6)
        unit = 2 if wide else 1
        if cchsrc < 0:
            raw = bytes(self.uc.mem_read(lpsrc, 4096)).split(b"\x00\x00" if wide else b"\x00", 1)[0]
            nsrc = len(raw) // unit
        else:
            nsrc = cchsrc
            raw = bytes(self.uc.mem_read(lpsrc, nsrc * unit)) if nsrc else b""
        if cchdest == 0:
            self.stdcall_return(6, nsrc)
        else:
            towrite = raw[: cchdest * unit]
            if lpdest and towrite:
                self.uc.mem_write(lpdest, towrite)
            self.stdcall_return(6, len(towrite) // unit)

    def _api_lcmapstringw(self):
        self._lcmapstring(True)

    def _api_lcmapstringa(self):
        self._lcmapstring(False)

    def _api_getfiletype(self):
        self.get_args(1)
        self.stdcall_return(1, 2)  # FILE_TYPE_CHAR

    def _api_widechartomultibyte(self):
        args = self.get_args(8)
        codepage, flags, lpwide, cchwide, lpmb, cbmb, lpdefault, lpused = args
        if cchwide < 0 or cchwide & 0x80000000:
            raw, i = b"", 0
            while i < 8192:
                b2 = bytes(self.uc.mem_read(lpwide + i * 2, 2))
                raw += b2
                if b2 == b"\x00\x00":
                    break
                i += 1
        else:
            raw = bytes(self.uc.mem_read(lpwide, cchwide * 2))
        encoded = raw.decode("utf-16-le", errors="replace").encode("latin1", errors="replace")
        if cbmb == 0:
            self.stdcall_return(8, len(encoded))
        else:
            self.uc.mem_write(lpmb, encoded[:cbmb])
            self.stdcall_return(8, len(encoded[:cbmb]))

    def _api_multibytetowidechar(self):
        args = self.get_args(6)
        codepage, flags, lpmb, cbmb, lpwide, cchwide = args
        if cbmb < 0 or cbmb & 0x80000000:
            raw = bytes(self.uc.mem_read(lpmb, 8192)).split(b"\x00", 1)[0] + b"\x00"
        else:
            raw = bytes(self.uc.mem_read(lpmb, cbmb))
        encoded = raw.decode("latin1", errors="replace").encode("utf-16-le", errors="replace")
        if cchwide == 0:
            self.stdcall_return(6, len(encoded) // 2)
        else:
            self.uc.mem_write(lpwide, encoded[: cchwide * 2])
            self.stdcall_return(6, len(encoded[: cchwide * 2]) // 2)

    def _api_getenvironmentstringsw(self):
        buf = self.heap_ptr
        self.heap_ptr += 0x10
        self.uc.mem_write(buf, b"\x00" * 4)
        self.stdcall_return(0, buf)

    def _api_getenvironmentstringsa(self):
        buf = self.heap_ptr
        self.heap_ptr += 0x10
        self.uc.mem_write(buf, b"\x00" * 2)
        self.stdcall_return(0, buf)

    def _api_freeenvironmentstringsw(self):
        self.stdcall_return(1, 1)

    def _api_freeenvironmentstringsa(self):
        self.stdcall_return(1, 1)

    def _api_getcommandlinea(self):
        path = (self.exe_path_full() + "\x00").encode()
        buf = self.heap_ptr
        self.heap_ptr += align_up(len(path))
        self.uc.mem_write(buf, path)
        self.stdcall_return(0, buf)

    def _api_getcommandlinew(self):
        path = (self.exe_path_full() + "\x00").encode("utf-16-le")
        buf = self.heap_ptr
        self.heap_ptr += align_up(len(path))
        self.uc.mem_write(buf, path)
        self.stdcall_return(0, buf)

    def _api_openfilemappinga(self):
        args = self.get_args(3)
        name = self.read_cstr(args[2]) if args[2] else ""
        h = self.named_mappings.get(name)
        self.stdcall_return(3, h if h else 0)

    def _api_createfilemappinga(self):
        hfile, _attr, protect, size_hi, size_lo, lpname = self.get_args(6)
        name = self.read_cstr(lpname) if lpname else ""
        size = (size_hi << 32) | size_lo
        fh = self.file_handles.get(hfile)
        if fh and size == 0:
            size = len(fh["data"])
        elif size == 0:
            size = PAGE
        h = self.next_handle
        self.next_handle += 1
        self.mapping_objs[h] = {"file_handle": hfile, "size": size}
        if name:
            self.named_mappings[name] = h
        self.stdcall_return(6, h)

    def _api_mapviewoffile(self):
        hmap, access, off_hi, off_lo, nbytes = self.get_args(5)
        mobj = self.mapping_objs.get(hmap)
        if mobj is None:
            self.stdcall_return(5, 0)
            return
        size = nbytes if nbytes else mobj["size"]
        ptr = self.heap_ptr
        self.heap_ptr += align_up(max(size, PAGE))
        fh = self.file_handles.get(mobj["file_handle"])
        if fh:
            content = bytes(fh["data"][:size]) if size else bytes(fh["data"])
            self.uc.mem_write(ptr, content + b"\x00" * max(0, size - len(content)))
        self.mapped_views[ptr] = hmap
        self.stdcall_return(5, ptr)

    def _api_unmapviewoffile(self):
        (ptr,) = self.get_args(1)
        self.mapped_views.pop(ptr, None)
        self.stdcall_return(1, 1)

    def _api_flushviewoffile(self):
        self.stdcall_return(2, 1)

    def _api_tlsalloc(self):
        slot = self.tls_next
        self.tls_next += 1
        self.stdcall_return(0, slot)

    def _api_tlssetvalue(self):
        slot, val = self.get_args(2)
        self.tls_slots[slot] = val
        self.stdcall_return(2, 1)

    def _api_tlsgetvalue(self):
        (slot,) = self.get_args(1)
        self.stdcall_return(1, self.tls_slots.get(slot, 0))

    def _api_tlsfree(self):
        (slot,) = self.get_args(1)
        self.tls_slots.pop(slot, None)
        self.stdcall_return(1, 1)

    def _api_loadlibrarya(self):
        (lp,) = self.get_args(1)
        s = self.read_cstr(lp) if lp else "(null)"
        content = self.named_files.get(self.norm_path(s))
        base = None
        if content and bytes(content[:2]) == b"MZ":
            print(f"LoadLibraryA({s!r}) -> attempting real load ({len(content)} bytes)")
            base = self.load_pe_module(content)
        if base:
            mpe = self.loaded_modules[base]
            if mpe.OPTIONAL_HEADER.AddressOfEntryPoint:
                ep = base + mpe.OPTIONAL_HEADER.AddressOfEntryPoint
                self.run_dllmain(base, ep, lambda: self.stdcall_return(1, base))
            else:
                self.stdcall_return(1, base)
        else:
            self.stdcall_return(1, 0x77000000)


# stdcall argument counts for common APIs without a dedicated handler above.
# cdecl functions (caller cleans stack) are listed with 0.
ARGC = {
    "localfree": 1, "deletefilea": 1, "createdirectorya": 2, "removedirectorya": 1,
    "createmutexa": 3, "createprocessa": 10, "releasemutex": 1, "getcurrentthread": 0,
    "duplicatehandle": 7, "freelibrary": 1, "writeprocessmemory": 5, "isdbcsleadbyte": 1,
    "wsprintfa": 0, "formatmessagea": 7, "lstrcpya": 2, "lstrlena": 1, "lstrcata": 2,
    "lstrcmpia": 2, "sleep": 1, "terminateprocess": 2, "flushfilebuffers": 1,
    "setfileattributesa": 2, "findfirstfilea": 2, "findnextfilea": 2, "findclose": 1,
    "getprivateprofilestringa": 6, "writeprivateprofilestringa": 4, "getsysteminfo": 1,
    "interlockedincrement": 1, "interlockeddecrement": 1, "interlockedexchange": 2,
    "interlockedexchangeadd": 2, "interlockedcompareexchange": 3,
    "interlockedexchangepointer": 2, "interlockedcompareexchangepointer": 3,
    "initializecriticalsection": 1, "entercriticalsection": 1, "leavecriticalsection": 1,
    "deletecriticalsection": 1, "setevent": 1, "resetevent": 1, "createeventa": 4,
    "getmodulefilenameexa": 4, "initializesecuritydescriptor": 2, "setsecuritydescriptordacl": 4,
    "setsecuritydescriptorowner": 3, "initializeacl": 3, "addaccessallowedace": 4,
    "getlengthsid": 1, "allocateandinitializesid": 11, "freesid": 1,
    "getsecuritydescriptorlength": 1, "isvalidsecuritydescriptor": 1, "equalsid": 2,
    "rtlunwind": 4, "setunhandledexceptionfilter": 1, "unhandledexceptionfilter": 1,
    "queryperformancecounter": 1, "queryperformancefrequency": 1, "getstdhandle": 1,
    "setlasterror": 1, "getstartupinfoa": 1, "getacp": 0, "getcpinfo": 2, "getoemcp": 0,
    "isvalidcodepage": 1, "setconsolectrlhandler": 2, "compareobjecthandles": 2,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to the protected exe to trace")
    parser.add_argument("--real-kernel32", required=True, help="path to a real PE32 kernel32.dll")
    parser.add_argument("--instr-limit", type=int, default=50_000_000)
    args = parser.parse_args()

    emu = build_emulator(args.exe, args.real_kernel32)

    print(f"Starting emulation at entry {hex(emu.entry)} ...")
    try:
        emu.uc.emu_start(emu.entry, emu.image_base + emu.image_size, timeout=0, count=args.instr_limit)
    except UcError as e:
        eip = emu.uc.reg_read(UC_X86_REG_EIP)
        print(f"Unicorn error: {e} at EIP={hex(eip)}")
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        try:
            code = emu.uc.mem_read(max(eip - 32, 0), 64)
            for insn in md.disasm(bytes(code), max(eip - 32, 0)):
                marker = " <===" if insn.address <= eip < insn.address + insn.size else ""
                print(f"  {hex(insn.address)}: {insn.mnemonic} {insn.op_str}{marker}")
        except Exception as ex:
            print(f"  (disasm failed: {ex})")

    print(f"\nDone. api_calls={len(emu.call_log)} instr={emu.instr_count} "
          f"stop_reason={emu.stop_reason} first_text_hit={emu.first_text_hit}")
    if emu.first_text_hit["addr"] is not None:
        rva = emu.first_text_hit["addr"] - emu.image_base
        print(f"Candidate real OEP: VA {hex(emu.first_text_hit['addr'])} "
              f"(RVA {hex(rva)}, for tools/unwrap.py --entry-rva)")


if __name__ == "__main__":
    main()
