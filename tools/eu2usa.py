#!/usr/bin/env python3
"""Map European (BCES00220) PPU code addresses to the USA (BCUS98119) executable.

Uses the anchors written by tools/port_ppu_patches.py (labels and call sites matched
between the EU patch context and the USA lift). An address is "exact" when the nearest
anchors on both sides have the same delta; otherwise it sits in a transition gap and
must be checked by hand.

    python tools/eu2usa.py 0x00028554 func_000285A0 ...
"""
from __future__ import annotations

import bisect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = ROOT / ".local/usa/eu-usa-anchors.json"
FUNCS = ROOT / ".local/usa/ppu/EBOOT.functions.json"


def load():
    a = json.loads(ANCHORS.read_text(encoding="utf-8"))
    ks = sorted(int(k, 16) for k in a)
    ds = [int(a["%08X" % k], 16) - k for k in ks]
    starts = {int(f["start"], 16) for f in json.loads(FUNCS.read_text(encoding="utf-8"))}
    return ks, ds, starts


def map_addr(eu: int, ks, ds):
    i = bisect.bisect_right(ks, eu)
    lo = (ks[i - 1], ds[i - 1]) if i > 0 else None
    hi = (ks[i], ds[i]) if i < len(ks) else None
    if lo and lo[0] == eu:
        return eu + lo[1], "exact", lo, hi
    if lo and hi and lo[1] == hi[1]:
        return eu + lo[1], "exact", lo, hi
    near = min((x for x in (lo, hi) if x), key=lambda x: abs(x[0] - eu))
    return eu + near[1], "gap", lo, hi


def main(argv):
    ks, ds, starts = load()
    for arg in argv:
        m = re.fullmatch(r"(?:(func|loc)_)?(?:0x)?([0-9A-Fa-f]+)u?", arg)
        if not m:
            print(f"{arg}: ?")
            continue
        eu = int(m.group(2), 16)
        us, how, lo, hi = map_addr(eu, ks, ds)
        fn = " fn-start" if us in starts else ""
        extra = ""
        if how == "gap":
            extra = f"  (anchors {lo[0]:#x}{lo[1]:+#x} / {hi[0]:#x}{hi[1]:+#x})" if lo and hi else ""
        print(f"{arg}: EU {eu:#010x} -> USA {us:#010x} [{how}{fn}]{extra}")


if __name__ == "__main__":
    main(sys.argv[1:])
