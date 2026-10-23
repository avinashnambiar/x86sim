#!/usr/bin/env python3
"""Turn a relocation-free 16-bit boot program (ELF .o) into a disk image.

usage: mkdisk.py prog.o out.img hda|fda

The object's .text becomes the image from sector 0 (the boot signature
0x55AA is placed at offset 510); a floppy image is padded to 1.44 MB, a
hard disk image to 1 MiB."""
import struct
import sys


def text_of(obj):
    d = open(obj, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1, "need an ELF32 object"
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx]
    name = lambda s: d[strtab[4] + s[0]:].split(b"\0", 1)[0].decode()
    text = None
    for s in secs:
        if name(s) == ".text":
            text = d[s[4]:s[4] + s[5]]
        if name(s) in (".rel.text", ".rela.text") and s[5]:
            sys.exit("the boot program must not need relocations")
    return bytearray(text)


img = text_of(sys.argv[1])
if len(img) < 512:
    img += bytes(512 - len(img))
if img[510:512] not in (b"\0\0", b"\x55\xaa"):
    sys.exit("sector 0 code overlaps the boot signature")
img[510:512] = b"\x55\xaa"
size = 1474560 if sys.argv[3] == "fda" else 1 << 20
img += bytes(size - len(img))
open(sys.argv[2], "wb").write(img)
