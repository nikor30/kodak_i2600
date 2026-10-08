#!/usr/bin/env python3
"""Read {id, name} lookup tables out of a 64-bit little-endian ELF shared object.

Used for the vendor driver's name tables (state/command/event ids -> names),
which are plain data: 16-byte records of an integer id and a char pointer.
Pointers in a .so are R_X86_64_RELATIVE relocations, so they are resolved here.

  elftables.py lib.so table <symbol|0xaddr> <count> [--idsize 1|4]
  elftables.py lib.so xref <string>      # where is a pointer to this string stored?
  elftables.py lib.so ptrs <0xaddr> <count> [--stride 8]   # pointer array -> strings
"""
import struct
import sys


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<IIQQQQIIQQ", d, shoff + i * shentsize)
            self.sections.append(dict(name=name, type=typ, addr=addr, off=off, size=size,
                                      link=link, entsize=entsize))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            s["name"] = self.cstr_off(strtab["off"] + s["name"])
        self.rel = {}
        for s in self.sections:
            if s["type"] == 4:  # SHT_RELA
                for o in range(s["off"], s["off"] + s["size"], 24):
                    r_off, r_info, r_add = struct.unpack_from("<QQq", d, o)
                    if r_info & 0xFFFFFFFF == 8:  # R_X86_64_RELATIVE
                        self.rel[r_off] = r_add
        self.syms = {}
        for s in self.sections:
            if s["type"] in (2, 11):
                st = self.sections[s["link"]]
                for o in range(s["off"], s["off"] + s["size"], 24):
                    n, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, o)
                    if value:
                        self.syms[self.cstr_off(st["off"] + n)] = value

    def cstr_off(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode("latin-1")

    def off(self, addr):
        for s in self.sections:
            if s["addr"] and s["addr"] <= addr < s["addr"] + s["size"] and s["type"] != 8:
                return s["off"] + addr - s["addr"]
        return None

    def ptr(self, addr):
        if addr in self.rel:
            return self.rel[addr]
        o = self.off(addr)
        return struct.unpack_from("<Q", self.d, o)[0] if o is not None else 0

    def cstr(self, addr):
        o = self.off(addr)
        return self.cstr_off(o) if o is not None else None

    def resolve(self, s):
        if s.startswith("0x"):
            return int(s, 16)
        hits = [v for k, v in self.syms.items() if s in k]
        if not hits:
            sys.exit(f"symbol {s} not found")
        return hits[0]


def main():
    a = sys.argv[1:]
    e = Elf(a[0])
    cmd = a[1]
    opt = dict(zip(a[4::2], a[5::2])) if len(a) > 4 else {}
    if cmd == "table":
        base, n = e.resolve(a[2]), int(a[3])
        idsize = int(opt.get("--idsize", 4))
        for i in range(n):
            o = e.off(base + 16 * i)
            ident = int.from_bytes(e.d[o:o + idsize], "little")
            print(f"0x{ident:02x} {ident:4d}  {e.cstr(e.ptr(base + 16 * i + 8))}")
    elif cmd == "ptrs":
        base, n = e.resolve(a[2]), int(a[3])
        stride = int(opt.get("--stride", 8))
        for i in range(n):
            p = e.ptr(base + stride * i)
            print(f"{i:3d} 0x{i:02x}  {e.cstr(p) if p else None}")
    elif cmd == "xref":
        needle = a[2].encode() + b"\0"
        pos = -1
        while True:
            pos = e.d.find(needle, pos + 1)
            if pos < 0:
                break
            if pos and e.d[pos - 1] != 0:
                continue
            for s in e.sections:
                if s["off"] <= pos < s["off"] + s["size"] and s["addr"]:
                    va = s["addr"] + pos - s["off"]
                    refs = [hex(k) for k, v in e.rel.items() if v == va]
                    print(f"string at 0x{va:x}; pointer stored at {refs}")


if __name__ == "__main__":
    main()
