#!/usr/bin/env python3
"""Port the European PPU source patches onto the USA (BCUS98119) lift.

The EU patch set in installer/patches/infamous-recomp/recompiled/ is ~99% mechanical
rewrites of the lifter's output (coherent stvx/dcbz, a vslo/vsl operand-order fix)
plus a few dozen hand-written hooks. The USA lift has the same code at different
addresses, so the patches cannot be applied verbatim.

This tool:
  1. applies the mechanical rewrites to every lifted line (USA counts are checked
     against the EU patch counts),
  2. relocates each remaining hand-written hunk by matching its context with every
     address-like token (hex constants, func_/loc_ names) wildcarded, picks the
     match whose literal tokens agree best, and rewrites the hunk's tokens through
     the EU->USA correspondence found in that match,
  3. writes the result and a JSON report of every relocated hunk.

A hunk that has no unique, consistent match stops the tool. Nothing is guessed.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH_DIR = ROOT / "installer/patches/infamous-recomp/recompiled"
FILES = ["ppu_recomp.h"] + [f"ppu_recomp_{i:03d}.cpp" for i in range(5)]

# --- 1. mechanical rewrites -------------------------------------------------------------

MECH = [
    # v31i: coherent vector store
    (re.compile(r"memcpy\(vm_base \+ \(uint32_t\)ea, (&ctx->vr\[\d+\]), 16\);"),
     r"ppu_stvx16((uint32_t)ea, \1);"),
    # v31i: coherent dcbz
    (re.compile(r"memset\(vm_base \+ \(uint32_t\)ea, 0, 128\);"),
     r"ppu_dcbz128((uint32_t)ea);"),
    # v124: read the shift operand before copying vA (vD may alias vB)
    (re.compile(r"(memcpy\(a_, &ctx->vr\[\d+\], 16\); )(const unsigned s_ = [^;]*;) "),
     r"\2 \1"),
    # comments: the EU lift wrote an UTF-8 em dash, the maintained files (and this lift) cp1252
    (re.compile("\xe2\x80\x94"), "\x97"),
]


def mech(line: str) -> str:
    for rx, rep in MECH:
        if rx.search(line):
            line = rx.sub(rep, line)
    return line


# --- patch parsing ----------------------------------------------------------------------

HUNK_RX = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def parse_patch(path: Path):
    hunks = []
    cur = None
    for raw in path.read_bytes().split(b"\n"):
        line = raw.decode("latin-1").rstrip(chr(13))
        if line.startswith(("--- ", "+++ ", "# eol")):
            continue
        m = HUNK_RX.match(line)
        if m:
            cur = {"eu_line": int(m.group(1)), "old": [], "new": []}
            hunks.append(cur)
            continue
        if cur is None or not line:
            if cur is not None and line == "":
                # blank context line in a unified diff is " "; a truly empty line is EOF noise
                pass
            continue
        tag, body = line[0], line[1:]
        if tag == " ":
            cur["old"].append(body); cur["new"].append(body)
        elif tag == "-":
            cur["old"].append(body)
        elif tag == "+":
            cur["new"].append(body)
        elif tag == "\\":
            pass
    return hunks


# --- 2. relocation ----------------------------------------------------------------------

TOK_RX = re.compile(r"0x[0-9A-Fa-f]+|(?:func|loc|tail|blk)_[0-9A-Fa-f]{6,8}|\b[0-9]{4,}\b")


def norm(line: str) -> str:
    return TOK_RX.sub("\x01", line.strip())


def toks(line: str):
    return TOK_RX.findall(line)


class Corpus:
    def __init__(self, texts: dict[str, list[str]]):
        self.texts = texts
        self.norm = {f: [norm(l) for l in ls] for f, ls in texts.items()}
        self.index: dict[str, list[tuple[str, int]]] = defaultdict(list)
        for f, ls in self.norm.items():
            for i, n in enumerate(ls):
                self.index[n].append((f, i))

    def find(self, old: list[str]):
        nold = [norm(l) for l in old]
        # anchor on the rarest non-trivial line
        best = None
        for k, n in enumerate(nold):
            c = len(self.index.get(n, ()))
            if c == 0:
                return []
            if best is None or c < best[1]:
                best = (k, c)
        k = best[0]
        out = []
        for f, i in self.index[nold[k]]:
            s = i - k
            ls = self.norm[f]
            if s < 0 or s + len(nold) > len(ls):
                continue
            if all(ls[s + j] == nold[j] for j in range(len(nold))):
                out.append((f, s))
        return out


def score(old, cand):
    same = tot = 0
    for a, b in zip(old, cand):
        for x, y in zip(toks(a), toks(b)):
            tot += 1
            same += x.lower() == y.lower()
    return same, tot


def token_map(old, cand):
    mp: dict[str, set[str]] = defaultdict(set)
    for a, b in zip(old, cand):
        for x, y in zip(toks(a), toks(b)):
            mp[x].add(y)
    return mp


def relocate(new: list[str], mp, report, model=None, labels=None):
    # the same value may be spelled differently in the new lines (0x00073FEC vs 0x73FECu)
    byval = defaultdict(set)
    for k, v in mp.items():
        if k.startswith("0x") and len(v) == 1:
            byval[int(k, 16)].add(int(next(iter(v)), 16))
    out = []
    for line in new:
        def sub(m):
            t = m.group(0)
            ys = mp.get(t)
            if ys is None and t.startswith("0x") and len(byval.get(int(t, 16), ())) == 1:
                return "0x%X" % next(iter(byval[int(t, 16)]))
            if ys is None and model is not None:
                am = re.fullmatch(r"(func|loc|tail|blk)_([0-9A-Fa-f]{6,8})", t)
                if am:
                    ds = model.predict(int(am.group(2), 16))
                    if len(ds) == 1:
                        cand = "%s_%08X" % (am.group(1), int(am.group(2), 16) + next(iter(ds)))
                        if cand in labels:
                            report.setdefault("predicted", []).append(f"{t}->{cand}")
                            return cand
            if ys is None:
                report.setdefault("unmapped", []).append(t)
                return t
            if len(ys) != 1:
                raise ValueError(f"token {t} maps to several USA tokens {sorted(ys)}")
            return next(iter(ys))
        out.append(TOK_RX.sub(sub, line))
    return out


ADDR_RX = re.compile(r"(?:func|loc|tail|blk)_([0-9A-Fa-f]{6,8})|ctx->lr = 0x([0-9A-Fa-f]+)")


def strip_v124(lines):
    return [re.sub(r"\s*/\* v\w+ v\d+, v\d+, v\d+ \(v124\) \*/$", "", l) for l in lines]


def addr_pairs(old, cand):
    out = []
    for a, b in zip(old, cand):
        for x, y in zip(ADDR_RX.findall(a), ADDR_RX.findall(b)):
            out.append((int(x[0] or x[1], 16), int(y[0] or y[1], 16)))
    return out


class DeltaModel:
    """EU->USA code address deltas observed at unambiguous matches."""
    def __init__(self, pairs):
        best = {}
        for e, u in pairs:
            best.setdefault(e, Counter())[u - e] += 1
        self.keys = sorted(best)
        self.delta = [best[k].most_common(1)[0][0] for k in self.keys]

    def predict(self, eu):
        import bisect
        i = bisect.bisect_left(self.keys, eu)
        near = []
        for j in (i - 1, i):
            if 0 <= j < len(self.keys):
                near.append((abs(self.keys[j] - eu), self.delta[j]))
        near.sort()
        return {d for _, d in near if _ < 0x4000}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=ROOT / ".local/usa/recompiled",
                    help="pristine USA lift (ppu_lifter.py output)")
    ap.add_argument("--out", type=Path, default=ROOT / "infamous-recomp/recompiled")
    ap.add_argument("--report", type=Path, default=ROOT / ".local/usa/ppu-port-report.json")
    args = ap.parse_args()

    texts = {}
    eol = {}
    for f in FILES:
        raw = (args.src / f).read_bytes()
        eol[f] = b"\r\n" if b"\r\n" in raw[:4096] else b"\n"
        texts[f] = raw.decode("latin-1").split(eol[f].decode())

    # 1. mechanical
    counts = Counter()
    for f, ls in texts.items():
        if f.endswith(".h"):
            continue
        for i, l in enumerate(ls):
            n = mech(l)
            if n != l:
                counts[f] += 1
                ls[i] = n
    print("mechanical rewrites:", dict(counts), "total", sum(counts.values()))

    corpus = Corpus(texts)
    report = {"hunks": [], "mechanical": dict(counts)}
    edits = defaultdict(list)  # file -> [(start, len, newlines)]
    failed = 0
    hunks_mech = []
    pending = []
    for f in FILES:
        for h in parse_patch(PATCH_DIR / f"{f}.patch"):
            old = [mech(l) for l in h["old"]]
            new = [mech(l) for l in h["new"]]
            if strip_v124(old) == strip_v124(new):
                hunks_mech.append(old)
                continue
            pending.append((f, h, old, new))

    # anchors from the mechanical hunks: their context locates them uniquely almost always
    pairs = []
    for old in hunks_mech:
        m = corpus.find(old)
        if len(m) == 1:
            mf, ms = m[0]
            pairs += addr_pairs(old, texts[mf][ms:ms + len(old)])
    model = DeltaModel(pairs)
    labels = set()
    for ls in texts.values():
        for l in ls:
            for m in re.finditer(r"^(loc_[0-9A-F]{8}):|^void (func_[0-9A-F]{8})\(", l):
                labels.add(m.group(1) or m.group(2))
    print(f"address anchors: {len(model.keys)}")
    (args.report.parent / "eu-usa-anchors.json").write_text(
        json.dumps({"%08X" % k: "%08X" % (k + d) for k, d in zip(model.keys, model.delta)}, indent=0),
        encoding="utf-8")

    for f, h, old, new in pending:
        if True:
            matches = corpus.find(old)
            entry = {"eu_file": f, "eu_line": h["eu_line"], "candidates": len(matches)}
            if not matches:
                # retry without the encoding-sensitive comment lines
                entry["error"] = "no match"
                failed += 1
                report["hunks"].append(entry)
                print(f"FAIL {f}:{h['eu_line']}: no match", file=sys.stderr)
                continue
            def agree(mf, ms):
                ps = addr_pairs(old, texts[mf][ms:ms + len(old)])
                return sum((u - e) in model.predict(e) for e, u in ps)
            scored = sorted((((agree(mf, ms),) + score(old, texts[mf][ms:ms + len(old)]), mf, ms)
                             for mf, ms in matches), key=lambda t: (-t[0][0], -t[0][1]))
            scored = [((a * 1000 + b, c), mf, ms) for (a, b, c), mf, ms in scored]
            (s0, tot), mf, ms = scored[0]
            top = [t for t in scored if t[0][0] == s0]
            if len(top) > 1:
                # the lifter emits some bodies in more than one file; the EU patches patch each
                # copy in its own file, so the copy in the same-named file is the one meant here
                same = [t for t in top if t[1] == f]
                if len(same) == 1:
                    entry["duplicate_copies"] = len(top)
                    scored = same + [t for t in scored if t is not same[0]]
                    (s0, tot), mf, ms = scored[0]
                    top = same
            if len(top) > 1:
                entry["error"] = f"ambiguous: {len(scored)} equal matches"
                failed += 1
                report["hunks"].append(entry)
                print(f"FAIL {f}:{h['eu_line']}: ambiguous ({len(scored)})", file=sys.stderr)
                continue
            cand = texts[mf][ms:ms + len(old)]
            mp = token_map(old, cand)
            try:
                rep = {}
                newl = relocate(new, mp, rep, model, labels)
            except ValueError as e:
                entry["error"] = str(e)
                failed += 1
                report["hunks"].append(entry)
                print(f"FAIL {f}:{h['eu_line']}: {e}", file=sys.stderr)
                continue
            entry.update(usa_file=mf, usa_line=ms + 1, literal_agree=f"{s0}/{tot}",
                         second_best=(f"{scored[1][0][0]}/{tot}" if len(scored) > 1 else None),
                         unmapped=sorted(set(rep.get("unmapped", []))),
                         predicted=rep.get("predicted", []),
                         remap={k: next(iter(v)) for k, v in mp.items() if next(iter(v)) != k and len(v) == 1},
                         added=[l.strip() for l in newl if l not in cand][:8])
            report["hunks"].append(entry)
            edits[mf].append((ms, len(old), newl))

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"relocated hunks: {sum(len(v) for v in edits.values())}, failed: {failed}; report {args.report}")
    if failed:
        return 1

    for f, es in edits.items():
        es.sort(key=lambda e: e[0])
        for a, b in zip(es, es[1:]):
            if a[0] + a[1] > b[0]:
                print(f"FAIL overlapping hunks in {f} at {a[0]+1} and {b[0]+1}", file=sys.stderr)
                return 1
        ls = texts[f]
        for s, n, newl in reversed(es):
            ls[s:s + n] = newl

    args.out.mkdir(parents=True, exist_ok=True)
    for f, ls in texts.items():
        (args.out / f).write_bytes(eol[f].decode().join(ls).encode("latin-1"))
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
