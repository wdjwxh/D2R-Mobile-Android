#!/usr/bin/env python3
"""Wrap a rewritten image in a loadable Android ARM64 ELF shared object.

The ELF header occupies its own 16 KiB page. Image offset zero is exported as
``dm_guest_image`` at ELF virtual address PAGE; guest-relative offsets retain
their original meaning. Guest relocation belongs to loader.c, not ld.so.
"""
import struct
import sys

PAGE = 0x4000


def align(value, alignment=PAGE):
    return (value + alignment - 1) & -alignment


def main(src, tsv, dst):
    boundaries = {}
    with open(tsv, encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("# ") and "\t" in line:
                key, value = line[2:].split("\t")
                if key in ("TEXT_END", "PATCH_OFF"):
                    boundaries[key] = int(value.strip(), 16)
    with open(src, "rb") as stream:
        image = stream.read()
    text_end = align(boundaries["TEXT_END"])
    patch_start = boundaries["PATCH_OFF"]
    image_end = align(len(image))
    assert 0 < text_end <= patch_start < len(image)
    assert patch_start % PAGE == 0
    metadata_base = PAGE + image_end
    metadata = bytearray()

    def append(data, alignment=8):
        metadata.extend(bytes(align(len(metadata), alignment) - len(metadata)))
        address = metadata_base + len(metadata)
        metadata.extend(data)
        return address, len(data)

    strings = b"\0dm_guest_image\0"
    strtab, strsize = append(strings)
    # Symbol 1 points into .guest.text (section 1), not the ELF header.
    symbols = bytes(24) + struct.pack("<IBBHQQ", 1, 0x11, 0, 1, PAGE, len(image))
    symtab, symsize = append(symbols)
    # SysV hash: one bucket, two chains (null and the single exported symbol).
    hash_table, hashsize = append(struct.pack("<IIIII", 1, 2, 1, 0, 0))
    dynamic = b"".join(struct.pack("<QQ", tag, value) for tag, value in (
        (4, hash_table), (5, strtab), (6, symtab), (10, strsize), (11, 24), (0, 0)))
    dynaddr, dynsize = append(dynamic)
    names = b"\0.guest.text\0.guest.data\0.guest.patch\0.dynstr\0.dynsym\0.hash\0.dynamic\0.shstrtab\0"
    shstrings, shstrsize = append(names, 1)

    def section(name, kind, flags, address, size, link=0, info=0, alignment=8, entsize=0):
        # File offsets equal virtual addresses for every mapped section.
        return struct.pack("<IIQQQQIIQQ", names.index(name.encode() + b"\0"),
                           kind, flags, address if flags & 2 else 0, address,
                           size, link, info, alignment, entsize)

    sections = [bytes(64),
        section(".guest.text", 1, 6, PAGE, text_end, alignment=PAGE),
        section(".guest.data", 1, 3, PAGE + text_end, patch_start - text_end, alignment=PAGE),
        section(".guest.patch", 1, 6, PAGE + patch_start, image_end - patch_start, alignment=PAGE),
        section(".dynstr", 3, 2, strtab, strsize, alignment=1),
        section(".dynsym", 11, 2, symtab, symsize, link=4, info=1, entsize=24),
        section(".hash", 5, 2, hash_table, hashsize, link=5, alignment=4, entsize=4),
        section(".dynamic", 6, 2, dynaddr, dynsize, link=4, entsize=16),
        section(".shstrtab", 3, 0, shstrings, shstrsize, alignment=1)]
    shoff, _ = append(b"".join(sections))
    assert len(metadata) <= PAGE

    def program(kind, flags, address, size, alignment=PAGE):
        return struct.pack("<IIQQQQQQ", kind, flags, address, address, address,
                           size, size, alignment)

    programs = [program(1, 4, 0, PAGE),
                program(1, 5, PAGE, text_end),
                program(1, 6, PAGE + text_end, patch_start - text_end),
                program(1, 5, PAGE + patch_start, image_end - patch_start),
                program(1, 4, metadata_base, PAGE),
                program(2, 4, dynaddr, dynsize, 8)]
    ident = b"\x7fELF\x02\x01\x01\x00" + bytes(8)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 183, 1, 0, 64, shoff,
                         0, 64, 56, len(programs), 64, len(sections), 8)
    with open(dst, "wb") as stream:
        stream.write((header + b"".join(programs)).ljust(PAGE, b"\0"))
        stream.write(image.ljust(image_end, b"\0"))
        stream.write(metadata.ljust(PAGE, b"\0"))
    print(f"{dst}: image={len(image):#x}, dm_guest_image={PAGE:#x}, metadata={metadata_base:#x}")


if __name__ == "__main__":
    main(*sys.argv[1:])
