#!/usr/bin/env python3
"""
Package an EBOOT.PBP from an ELF executable and a PARAM.SFO.

Three different readers parse this header and the file has to satisfy all
of them, because the same PBP must be listed and booted by real firmware's
XMB, by the PPSSPP GUI, and by PPSSPP's kernel HLE:

  * Real firmware and PPSSPP's PBPReader (Core/ELF/PBPReader.cpp) read the
    header as magic, version, then EIGHT contiguous offsets at bytes 0x08
    through 0x24 naming param.sfo, icon0, icon1, pic0, pic1, snd0, data.psp
    and data.psar. Each sub-file spans from its own offset to the next one,
    the last running to end of file. The XMB reads PARAM.SFO through this
    view to decide the game is listable at all -- a wrong 0x0C makes the
    console show the EBOOT as corrupt data.

  * PPSSPP's __KernelLoadModule (Core/HLE/sceKernelModule.cpp) derives
    numfiles from the first offset and then reads entries 1..N from byte
    12 + 4*i, and takes the executable as the range offsets[5]..offsets[6],
    i.e. the values at bytes 0x20 and 0x24.

Both views agree once the layout is: the SFO at 0x08, every absent asset
starting where the ELF starts (so its size computes to zero), and the ELF
at 0x20 ending at 0x24. The version word is what the SDK's own pack-pbp
writes, because that is the header real consoles accept.

The SDK's pack-pbp instead points slots 0x0C..0x24 at the ELF start
uniformly, so the kernel HLE computes the executable's size as zero and
PPSSPP reports "Truncated ELF header, 0 bytes".
"""

import struct
import sys


def read_file(path):
    with open(path, "rb") as fh:
        return fh.read()


def main():
    if len(sys.argv) < 4:
        print(f"usage: {sys.argv[0]} <out.pbp> <param.sfo> <executable.elf>",
              file=sys.stderr)
        return 1

    out_path, sfo_path, elf_path = sys.argv[1], sys.argv[2], sys.argv[3]

    sfo = read_file(sfo_path)
    elf = read_file(elf_path)

    if elf[:4] != b"\x7fELF":
        print("error: executable is not an ELF", file=sys.stderr)
        return 1
    if sfo[:4] != b"\x00PSF":
        print("error: param.sfo is not a PSF", file=sys.stderr)
        return 1

    sfo_offset = 0x28
    # The kernel HLE copies the executable straight out of the PBP, and
    # unaligned sources take a slow path on some hosts, so keep it aligned.
    elf_offset = (sfo_offset + len(sfo) + 3) & ~3

    out = bytearray(elf_offset + len(elf))

    struct.pack_into("<4sI", out, 0x00, b"\x00PBP", 0x00010000)

    struct.pack_into("<I", out, 0x08, sfo_offset)   # param.sfo start
    struct.pack_into("<I", out, 0x0C, elf_offset)   # sfo end = icon0 start (absent)
    struct.pack_into("<I", out, 0x10, elf_offset)   # icon1 (absent)
    struct.pack_into("<I", out, 0x14, elf_offset)   # pic0  (absent)
    struct.pack_into("<I", out, 0x18, elf_offset)   # pic1  (absent)
    struct.pack_into("<I", out, 0x1C, elf_offset)   # snd0  (absent)
    struct.pack_into("<I", out, 0x20, elf_offset)   # data.psp = ELF start
    struct.pack_into("<I", out, 0x24, len(out))     # ELF end = data.psar start

    out[sfo_offset:sfo_offset + len(sfo)] = sfo
    out[elf_offset:elf_offset + len(elf)] = elf

    with open(out_path, "wb") as fh:
        fh.write(out)

    print(f"{out_path}: {len(out)} bytes "
          f"(sfo@{sfo_offset}, elf@{elf_offset}, elf size {len(elf)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
