#!/usr/bin/env python3
# Minimal VA<->file-offset disassembler for th123.exe (x86, 32-bit, ImageBase 0x400000, no ASLR).
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"F:/Games/Touhou/SokuLauncher/Soku/th123.exe"

def load():
    data = open(EXE, "rb").read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew+4] == b"PE\0\0"
    fh = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, fh+2)[0]
    opt = fh + 20
    image_base = struct.unpack_from("<I", data, opt+28)[0]
    sec = opt + struct.unpack_from("<H", data, fh+16)[0]  # opt header size -> section table
    sections = []
    for i in range(num_sections):
        off = sec + i*40
        name = data[off:off+8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off+8)
        sections.append((name, vaddr, vsize, rawptr, rawsize))
    return data, image_base, sections

DATA, BASE, SECS = load()

def va_to_off(va):
    rva = va - BASE
    for name, vaddr, vsize, rawptr, rawsize in SECS:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None

def off_to_va(off):
    for name, vaddr, vsize, rawptr, rawsize in SECS:
        if rawptr <= off < rawptr + rawsize:
            return BASE + vaddr + (off - rawptr)
    return None

def disasm(va, length=0x80):
    off = va_to_off(va)
    if off is None:
        print("VA not mapped:", hex(va)); return
    code = DATA[off:off+length]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False
    for insn in md.disasm(code, va):
        b = " ".join(f"{x:02x}" for x in insn.bytes)
        print(f"0x{insn.address:08x}: {b:<24} {insn.mnemonic} {insn.op_str}")

def find_refs_to(target_va, kind="all"):
    """Scan .text for instructions whose operand references target_va (abs disp or rel call/jmp)."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    hits = []
    for name, vaddr, vsize, rawptr, rawsize in SECS:
        if name not in (".text", "text", ".rdata"):
            continue
        code = DATA[rawptr:rawptr+rawsize]
        start = BASE + vaddr
        for insn in md.disasm(code, start):
            ops = insn.op_str
            th = f"0x{target_va:x}"
            th2 = f"{target_va:x}h"
            if th in ops or th2 in ops:
                hits.append((insn.address, insn.mnemonic, insn.op_str))
    return hits

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "d"
    if cmd == "d":
        va = int(sys.argv[2], 16); length = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80
        disasm(va, length)
    elif cmd == "refs":
        tgt = int(sys.argv[2], 16)
        for a, m, o in find_refs_to(tgt):
            print(f"0x{a:08x}: {m} {o}")
    elif cmd == "secs":
        for s in SECS: print(s[0], hex(BASE+s[1]), "vsize", hex(s[2]), "raw", hex(s[3]), hex(s[4]))
