#!/usr/bin/env python3
"""Read big-endian words from the USA executable's loaded image: usa_peek.py <va> [count]."""
import struct, sys
from pathlib import Path
ELF = Path(__file__).resolve().parents[1] / ".local/usa/EBOOT.elf"
b = ELF.read_bytes()
phoff = struct.unpack(">Q", b[0x20:0x28])[0]
segs = []
for i in range(struct.unpack(">H", b[0x38:0x3A])[0]):
    t, fl, off, va, pa, fs, ms, al = struct.unpack(">IIQQQQQQ", b[phoff + i * 56:phoff + i * 56 + 56])
    if t == 1 and ms:
        segs.append((va, off, fs, ms))
def word(va):
    for s, off, fs, ms in segs:
        if s <= va < s + ms:
            return struct.unpack(">I", b[off + va - s:off + va - s + 4])[0] if va - s < fs else 0
    raise ValueError(hex(va))
def find(words, lo=0x720000, hi=0x7B8280):
    out = []
    for va in range(lo, hi - 4 * len(words), 4):
        if all(w is None or word(va + 4 * i) == w for i, w in enumerate(words)):
            out.append(va)
    return out
if __name__ == "__main__":
    va = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    print(" ".join("%08X" % word(va + 4 * i) for i in range(n)))
