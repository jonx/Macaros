#!/usr/bin/env python3
"""elfexec2hunk.py — convert a FULLY LINKED m68k ELF executable (GNU ld, linked
at base 0 with --emit-relocs) into a single-hunk AmigaOS executable.

Why single-hunk: with the whole image (text+rodata+data+bss) in ONE hunk, every
PC-relative fixup ld already resolved stays correct at any load address, and the
only records that need HUNK_RELOC32 entries are the absolute 32-bit relocations
(R_68K_32) — whose stored values ARE image offsets (base 0), exactly what the
hunk relocator adds the load base to. bss is emitted as literal zero padding
(run68k's loader requires payload == header size, so no trailing-bss trick).

Usage: elfexec2hunk.py <in.elf> <out.exe>
Requires: the link used `-q/--emit-relocs`, `-e _start`, and a script placing
_start at image offset 0 (checked via e_entry == 0). Fails loudly on any
remaining relocation type other than R_68K_32 / the resolved PC-relatives.
"""
import struct
import sys

R_68K_32 = 1
R_OK_RESOLVED = {2, 3, 4, 5, 6, 7}   # PC32/PC16/PC8/GOT... pc-rel: resolved, position-independent in one hunk

def die(msg):
    sys.stderr.write("elfexec2hunk: %s\n" % msg)
    sys.exit(1)

def main(inp, outp):
    d = open(inp, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
        die("not a 32-bit big-endian ELF")
    (e_type, e_machine) = struct.unpack(">HH", d[16:20])
    (e_entry,) = struct.unpack(">I", d[24:28])
    (e_shoff,) = struct.unpack(">I", d[32:36])
    (e_shentsize, e_shnum, e_shstrndx) = struct.unpack(">HHH", d[46:52])
    if e_machine != 4:
        die("not EM_68K")
    if e_entry != 0:
        die("entry is 0x%x, not 0 — _start must be first in the image" % e_entry)

    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (name, typ, flags, addr, offset, size, link, info, align, entsize) = \
            struct.unpack(">10I", d[off:off + 40])
        secs.append(dict(i=i, name=name, typ=typ, flags=flags, addr=addr,
                         offset=offset, size=size, link=link, info=info))
    shstr = secs[e_shstrndx]
    def sname(s):
        raw = d[shstr["offset"] + s["name"]:shstr["offset"] + s["name"] + 64]
        return raw.split(b"\0")[0].decode()

    # image size = max(addr+size) over SHF_ALLOC sections; build the flat image
    alloc = [s for s in secs if s["flags"] & 2]          # SHF_ALLOC
    imgsize = max(s["addr"] + s["size"] for s in alloc)
    img = bytearray(imgsize)
    for s in alloc:
        if s["typ"] != 8:                                 # not SHT_NOBITS
            img[s["addr"]:s["addr"] + s["size"]] = d[s["offset"]:s["offset"] + s["size"]]

    # collect R_68K_32 offsets from every SHT_RELA section targeting an alloc section
    reloc_offsets = []
    for s in secs:
        if s["typ"] != 4:                                 # SHT_RELA
            continue
        target = secs[s["info"]]
        if not (target["flags"] & 2):
            continue
        n = s["size"] // 12
        for k in range(n):
            (r_off, r_info, r_add) = struct.unpack(">IIi", d[s["offset"] + k * 12:s["offset"] + k * 12 + 12])
            rtype = r_info & 0xFF
            if rtype == R_68K_32:
                reloc_offsets.append(r_off)
            elif rtype in R_OK_RESOLVED:
                pass                                       # resolved, pc-relative: fine in one hunk
            elif rtype == 0:
                pass                                       # R_68K_NONE
            else:
                die("unsupported relocation type %d at 0x%x in %s" % (rtype, r_off, sname(target)))

    # emit: HUNK_HEADER (1 hunk) + HUNK_CODE + HUNK_RELOC32 + HUNK_END
    pad = (-len(img)) % 4
    img += b"\0" * pad
    longs = len(img) // 4
    out = bytearray()
    out += struct.pack(">IIIII", 0x3F3, 0, 1, 0, 0)       # header: 1 hunk, first 0, last 0
    out += struct.pack(">I", longs)                        # size table
    out += struct.pack(">II", 0x3E9, longs) + img          # HUNK_CODE
    if reloc_offsets:
        out += struct.pack(">I", 0x3EC)                    # HUNK_RELOC32
        out += struct.pack(">II", len(reloc_offsets), 0)   # count, target hunk 0
        for off in sorted(reloc_offsets):
            out += struct.pack(">I", off)
        out += struct.pack(">I", 0)                        # terminator
    out += struct.pack(">I", 0x3F2)                        # HUNK_END
    open(outp, "wb").write(out)
    print("elfexec2hunk: %s: %d bytes image, %d RELOC32 entries -> %s (%d bytes)"
          % (inp, imgsize, len(reloc_offsets), outp, len(out)))

if __name__ == "__main__":
    if len(sys.argv) != 3:
        die("usage: elfexec2hunk.py <in.elf> <out.exe>")
    main(sys.argv[1], sys.argv[2])
