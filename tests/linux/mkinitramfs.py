#!/usr/bin/env python3
"""Build a tiny initramfs for x86sim's Linux boot test.

usage: mkinitramfs.py init.o out.cpio

init.o is an x86-64 ELF object whose .text is position independent and has
no relocations (see init.S). Its .text becomes a static ELF executable
/init; the archive also holds /dev/console and an empty /proc."""
import struct
import sys


def text_of(obj):
    d = open(obj, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2, "need an ELF64 object"
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx]
    name = lambda s: d[strtab[4] + s[0]:].split(b"\0", 1)[0].decode()
    text = None
    for s in secs:
        if name(s) == ".text":
            text = d[s[4]:s[4] + s[5]]
        if name(s) in (".rela.text", ".rel.text") and s[5]:
            sys.exit("init.o must not need relocations")
    return text


def elf_exec(code, base=0x400000):
    ehsize, phsize = 64, 56
    off = ehsize + phsize
    entry = base + off
    eh = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, b"\0" * 7,
                     2, 62, 1, entry, ehsize, 0, 0, ehsize, phsize, 1, 64, 0, 0)
    memsz = off + len(code) + 4096  # room for .space data past the file
    ph = struct.pack("<IIQQQQQQ", 1, 7, 0, base, base, off + len(code), memsz, 4096)
    return eh + ph + code


def cpio(entries):
    out = b""
    ino = 1
    for path, mode, data, rdev in entries + [("TRAILER!!!", 0, b"", (0, 0))]:
        nm = path.encode() + b"\0"
        hdr = "070701" + "".join("%08X" % v for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(nm), 0))
        ino += 1
        out += hdr.encode() + nm
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
    return out


def main():
    code = text_of(sys.argv[1])
    entries = [
        ("dev", 0o040755, b"", (0, 0)),
        ("dev/console", 0o020600, b"", (5, 1)),
        ("proc", 0o040755, b"", (0, 0)),
        ("init", 0o100755, elf_exec(code), (0, 0)),
    ]
    open(sys.argv[2], "wb").write(cpio(entries))


main()
