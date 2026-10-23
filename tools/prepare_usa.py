"""Prepare private BCUS98119 sources and audit the EU patch set against them.

This is a command-line binary preparation tool; only the Python standard library
is needed. It never modifies the dump or applies European patches to USA code.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KNOWN_USA = "4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_sfo(path: Path) -> dict[str, str]:
    data = path.read_bytes()
    if data[:4] != b"\0PSF":
        raise ValueError(f"Invalid SFO: {path}")
    keys, values, count = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(count):
        key, fmt, length, _, offset = struct.unpack_from("<HHIII", data, 20 + i * 16)
        end = data.index(0, keys + key)
        name = data[keys + key:end].decode("utf-8")
        value = data[values + offset:values + offset + length]
        out[name] = (str(struct.unpack("<I", value)[0]) if fmt == 0x404
                     else value.rstrip(b"\0").decode("utf-8"))
    return out


def run(*args: object, cwd: Path = ROOT, log: Path | None = None) -> None:
    print("Running:", " ".join(map(str, args)), flush=True)
    if log:
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(list(map(str, args)), cwd=cwd, stdout=stream,
                           stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def decrypt(game: Path, work: Path, rpcs3: Path | None) -> Path:
    elf = work / "EBOOT.elf"
    original = game / "PS3_GAME/USRDIR/EBOOT.BIN"
    fingerprint = sha(original.read_bytes())
    provenance = work / "input.sha256"
    if elf.is_file() and provenance.is_file() and provenance.read_text().strip() == fingerprint:
        return elf
    supplied = game / "PS3_GAME/USRDIR/EBOOT.elf"
    if supplied.is_file():
        shutil.copy2(supplied, elf)
    else:
        if rpcs3 is None:
            candidates = [ROOT / ".local/rpcs3/rpcs3.exe"]
            rpcs3 = next((p for p in candidates if p.is_file()), None)
        if rpcs3 is None or not rpcs3.is_file():
            raise ValueError("Pass --rpcs3 PATH to decrypt a private copy of EBOOT.BIN")
        isolated = ROOT / ".local/rpcs3"
        isolated.mkdir(parents=True, exist_ok=True)
        for source in [rpcs3, *rpcs3.parent.glob("*.dll")]:
            target = isolated / source.name
            if source.resolve() != target.resolve() and not target.is_file():
                shutil.copy2(source, target)
        (isolated / "portable.txt").touch()
        # Stale output must not be mistaken for a successful decryption.
        elf.unlink(missing_ok=True)
        shutil.copy2(original, work / "EBOOT.BIN")
        run(isolated / "rpcs3.exe", "--decrypt", work / "EBOOT.BIN", cwd=isolated)
    if not elf.is_file() or elf.read_bytes()[:6] != b"\x7fELF\x02\x02":
        raise ValueError("Decryption did not produce a big-endian ELF64 executable")
    provenance.write_text(fingerprint + "\n")
    return elf


def audit(game: Path, elf: Path, work: Path, metadata: dict) -> dict:
    data = elf.read_bytes()
    expected = {}
    for line in (ROOT / "installer/generated.lock").read_text().splitlines():
        if line.startswith("file "):
            _, base, final, path = line.split(maxsplit=3)
            expected[path] = (base, final)
    by_hash = {base: path for path, (base, _) in expected.items()}
    loader = json.loads((work / "ppu/EBOOT.loader.json").read_text())
    images = []
    for image in sorted((work / "spu-images").glob("spu_*.elf")):
        digest = sha(image.read_bytes())
        images.append({"usa_file": image.name, "sha256": digest,
                       "identical_eu_file": by_hash.get(digest)})
    # Locate raw WWS jobs by their own headers, not EU offsets.
    jobs = []
    cursor = 0
    while (cursor := data.find(b"\xc0\xde\xc0\xde", cursor)) >= 0:
        start = cursor - 0x20
        if start >= 0 and start % 4 == 0:
            words = struct.unpack_from(">9I", data, start)
            entry, size = words[4:6]
            if (all(w >> 25 == 0x21 for w in words[:4])
                    and 0x24 <= size <= 0x40000 and start + size <= len(data) and entry < size):
                blob = data[start:start + size]
                name = f"jobmod_{hashlib.sha1(blob).hexdigest()[:12]}_e{entry:04X}"
                matched = next((p for p, (base, _) in expected.items()
                                if p.endswith(f"/jobmods/{name}.bin") and base == sha(blob)), None)
                jobs.append({"name": name, "file_offset": hex(start), "size": size,
                             "sha256": sha(blob), "identical_eu_file": matched})
        cursor += 4
    pm_path = "infamous-recomp/spu-images/pm_wwsjob.raw"
    pm_hash = expected[pm_path][0]
    pm_offset = next((offset for offset in range(0, len(data) - 0x2D80 + 1, 0x80)
                      if sha(data[offset:offset + 0x2D80]) == pm_hash), None)
    header = work / "recompiled/ppu_recomp.h"
    symbols = None
    if header.is_file():
        usa = set(re.findall(r"void func_([0-9A-Fa-f]{8})\(", header.read_text()))
        eu = set()
        for patch in (ROOT / "installer/patches/infamous-recomp/recompiled").glob("*.patch"):
            eu.update(re.findall(r"func_([0-9A-Fa-f]{8})", patch.read_bytes().decode("latin-1")))
        symbols = {"eu_patch_referenced_functions": len(eu),
                   "same_address_usa_functions": len(eu & usa),
                   "missing_at_eu_address": sorted(eu - usa),
                   "note": "An address match alone does not establish function equivalence."}
    return {"title_id": metadata["TITLE_ID"], "disc_version": metadata.get("VERSION"),
            "app_version": metadata.get("APP_VER"), "eboot_sha256": sha(data),
            "known_usa_executable": sha(data) == KNOWN_USA,
            "status": "requires-regional-patch-retargeting", "runtime_tested": False,
            "loader": loader, "spu_images": images, "wws_jobs": jobs,
            "wws_policy": {"eu_file_offset": "0x68cd80",
                           "usa_identical_file_offset": hex(pm_offset) if pm_offset is not None else None,
                           "sha256": pm_hash}, "ppu_patch_symbols": symbols}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path)
    parser.add_argument("--rpcs3", type=Path)
    parser.add_argument("--lift", action="store_true", help="Generate private USA PPU C++; never apply EU patches")
    parser.add_argument("--jobs", type=int, default=min(12, os.cpu_count() or 1))
    args = parser.parse_args()
    game = args.game
    if game is None:
        game = next((p for p in ROOT.parent.iterdir() if p.is_dir()
                     and (p / "PS3_GAME/PARAM.SFO").is_file()
                     and read_sfo(p / "PS3_GAME/PARAM.SFO").get("TITLE_ID") == "BCUS98119"), None)
    if game is None:
        raise ValueError("No BCUS98119 dump found beside the checkout; pass --game DIR")
    game = game.resolve()
    metadata = read_sfo(game / "PS3_GAME/PARAM.SFO")
    if metadata.get("TITLE_ID") != "BCUS98119":
        raise ValueError(f"Expected BCUS98119, found {metadata.get('TITLE_ID')}")
    work = ROOT / ".local/usa"
    work.mkdir(parents=True, exist_ok=True)
    elf = decrypt(game, work, args.rpcs3)
    if sha(elf.read_bytes()) != KNOWN_USA:
        raise ValueError("USA executable differs from the researched disc build; no offsets will be assumed")
    run(sys.executable, ROOT / "ps3recomp/tools/ppu_loader.py", elf, "--output", work / "ppu")
    run(sys.executable, ROOT / "ps3recomp/tools/extract_spu_images.py", elf,
        "--output", work / "spu-images")
    if args.lift:
        run(sys.executable, ROOT / "ps3recomp/tools/ppu_lifter.py", elf,
            "--functions", work / "ppu/EBOOT.functions.json",
            "--hle-stubs", work / "ppu/EBOOT.imports.json",
            "--output", work / "recompiled", "--jobs", max(1, args.jobs), log=work / "lift.log")
    report = audit(game, elf, work, metadata)
    destination = ROOT / "reports/usa-layout.json"
    destination.parent.mkdir(exist_ok=True)
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"USA report: {destination}\nStatus: {report['status']}\nNo game runtime was launched.")


if __name__ == "__main__":
    main()
