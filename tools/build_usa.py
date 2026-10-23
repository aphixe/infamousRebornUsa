#!/usr/bin/env python3
"""Generate the USA (BCUS98119) sources and build the USA player.

Runs the same pipeline as installer/infamous.toml, with the USA executable's offsets:

  ppu        tools/port_ppu_patches.py: the EU PPU patches relocated onto the USA lift
             (the lift itself comes from tools/prepare_usa.py --lift)
  spu        the eight embedded SPU programs, the WWS policy module and the 21 job modules
             are byte-identical to the European ones; they are extracted from their USA
             offsets (0x500 lower), named as in the EU tree, lifted, and the EU patches are
             applied with the installer's strict before/after hash checks
  assets     the optional font / prompt assets from the game data
  build      CMake + Ninja with -DINFAMOUS_REGION=USA (build-usa/)

    python tools/build_usa.py [--elf EBOOT.elf] [--game DUMP] [--work DIR] [--lift]
                              [--skip-gen] [--no-build] [--no-assets] [--jobs N]

The defaults are the development layout (.local/usa, the dump next to the checkout); the installer
passes its own. --lift runs the PPU loader and lifter into <work>/ first (needed once per machine).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "infamous-recomp"
TK = ROOT / "ps3recomp"
WORK = ROOT / ".local/usa"
ELF = WORK / "EBOOT.elf"
GAME = ROOT.parent / "inFamous [BCUS98119]"
USA_SHA = "4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4"
LOCK = ROOT / "installer/generated.lock"
PATCHES = ROOT / "installer/patches"
PY = sys.executable

# USA file offset of each embedded SPU program -> the EU tree's name for the same bytes
SPU_NAMES = {
    0x5F6000: "spu_0000_at_005F6500", 0x673E80: "spu_0001_at_00674380",
    0x68F900: "spu_0002_at_0068FE00", 0x6A3300: "spu_0003_at_006A3800",
    0x6BEF80: "spu_0004_at_006BF480", 0x6D7200: "spu_0005_at_006D7700",
    0x6F1900: "spu_0006_at_006F1E00", 0x700100: "spu_0007_at_00700600",
}
PM_OFF, PM_LEN = 0x68C880, 0x2D80      # EU 0x68CD80
PM_LS0 = 0x68BE80                       # file offset of the policy module's LS 0 (EU 0x68C380)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(*argv, cwd=ROOT, env=None):
    argv = [str(a) for a in argv]
    print(">", " ".join(argv), flush=True)
    e = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1")
    e.update(env or {})
    subprocess.run(argv, cwd=cwd, env=e, check=True)


def load_lock():
    files = {}
    for line in LOCK.read_text(encoding="utf-8").splitlines():
        if line.startswith("file "):
            _, before, after, rel = line.split(" ", 3)
            files[rel] = (before, after)
    return files


def split_lines(b: bytes):
    out, i = [], 0
    while i < len(b):
        j = b.find(b"\n", i)
        j = len(b) if j < 0 else j + 1
        out.append(b[i:j]); i = j
    return out


def apply_patch(old: bytes, patch: bytes) -> bytes:
    """installer/src/diff.rs apply_patch: exact context, hunks in order."""
    import re
    if patch.startswith(b"# eol lf\n"):
        old = old.replace(b"\r\n", b"\n")
    elif patch.startswith(b"# eol crlf\n"):
        old = old.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    src = split_lines(old)
    out, cur = [], 0
    lines = split_lines(patch)
    k = 0
    while k < len(lines):
        m = re.match(rb"@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@", lines[k])
        if not m:
            k += 1; continue
        start, cnt = int(m.group(1)), int(m.group(2) or 1)
        at = start if cnt == 0 else start - 1
        if at < cur or at > len(src):
            raise ValueError(f"hunk at line {start} out of order")
        out += src[cur:at]; cur = at
        k += 1
        while k < len(lines) and not lines[k].startswith(b"@@"):
            l = lines[k]; tag, body = l[:1], l[1:]
            if tag in (b" ", b"-"):
                if cur >= len(src) or src[cur] != body:
                    raise ValueError(f"context mismatch at line {cur + 1} (hunk at {start})")
                if tag == b" ":
                    out.append(body)
                cur += 1
            elif tag == b"+":
                out.append(body)
            elif l.startswith(b"\\"):
                # "\ No newline at end of file": strip the newline of the previous line
                if out and out[-1].endswith(b"\n"):
                    out[-1] = out[-1][:-2] if out[-1].endswith(b"\r\n") else out[-1][:-1]
            k += 1
    out += src[cur:]
    return b"".join(out)


def plug_offsets(b: bytes, delta: int) -> bytes:
    """plug_lift.py names each blob's EBOOT file offset in a comment; the USA blobs sit 0x500 lower.
    The code is byte-identical: with the comments moved to the EU offsets the file hashes as in the EU lock."""
    import re
    return re.sub(rb"(\(\w+(?:, fixed LS 0x[0-9A-F]+)?\): file )0x([0-9A-F]+)",
                  lambda m: m.group(1) + (b"0x%X" % (int(m.group(2), 16) + delta) if int(m.group(2), 16) else b"0x0"), b)


def patch_generated(prefixes):
    """Apply the EU patches to the generated (non-PPU) files, checking the lock's hashes."""
    lock = load_lock()
    bad = []
    n = 0
    for rel, (before, after) in sorted(lock.items()):
        if not rel.startswith(prefixes):
            continue
        p = ROOT / rel
        pp = PATCHES / (rel + ".patch")
        if before == "-":   # a file the patch set adds
            new = apply_patch(b"", pp.read_bytes())
            if hashlib.sha256(new).hexdigest() != after:
                bad.append(f"patch result differs  {rel}"); continue
            p.write_bytes(new); n += 1
            continue
        if not p.is_file():
            bad.append(f"missing  {rel}"); continue
        data = p.read_bytes()
        shifted = rel.startswith("infamous-recomp/spu/wwsplug/") and rel.endswith(".c")
        if shifted:
            data = plug_offsets(data, +0x500)
        h = hashlib.sha256(data).hexdigest()
        if h == after:
            continue
        if h != before:
            bad.append(f"differs  {rel}"); continue
        if before != after:
            new = apply_patch(data, pp.read_bytes())
            if hashlib.sha256(new).hexdigest() != after:
                bad.append(f"patch result differs  {rel}"); continue
            p.write_bytes(plug_offsets(new, -0x500) if shifted else new); n += 1
    print(f"patched {n} generated files")
    return bad


def extract_jobmods(elf: Path, out: Path) -> int:
    """installer/src/game.rs extract_jobmods: every C0DEC0DE module image in the EBOOT."""
    import struct
    d = elf.read_bytes()
    out.mkdir(parents=True, exist_ok=True)
    w = lambda o: struct.unpack_from(">I", d, o)[0] if o + 4 <= len(d) else None
    names, i = [], 0x20
    while i + 4 <= len(d):
        if d[i:i + 4] == bytes.fromhex("C0DEC0DE") and (i - 0x20) % 4 == 0:
            st = i - 0x20
            entry, size = w(st + 0x10) or 0, w(st + 0x14) or 0
            ila = all((w(st + 4 * k) or 0) >> 25 == 0x21 for k in range(4))
            if ila and 0x24 <= size <= 0x40000 and st + size <= len(d) and entry < size:
                m = d[st:st + size]
                name = f"jobmod_{hashlib.sha1(m).hexdigest()[:12]}_e{entry:04X}"
                if name not in names:
                    (out / f"{name}.bin").write_bytes(m)
                    names.append(name)
                i = st + max(size, 0x24)
                continue
        i += 4
    if not names:
        raise SystemExit("no WWS job modules found")
    return len(names)


def gen_spu():
    imgs = PORT / "spu-images"
    if imgs.exists():
        shutil.rmtree(imgs)
    imgs.mkdir(parents=True)
    tmp = WORK / "spu-images"
    run(PY, TK / "tools/extract_spu_images.py", ELF, "--output", tmp)
    for off, name in SPU_NAMES.items():
        shutil.copyfile(tmp / f"spu_{list(SPU_NAMES).index(off):04d}_at_{off:08X}.elf", imgs / f"{name}.elf")
    raw = ELF.read_bytes()[PM_OFF:PM_OFF + PM_LEN]
    (imgs / "pm_wwsjob.raw").write_bytes(raw)
    run(PY, TK / "tools/wrap_spu_elf.py", imgs / "pm_wwsjob.raw", "--base", "0xA00", "--entry", "0x2BF0",
        "--out", imgs / "pm_wwsjob.elf")
    run(PY, TK / "tools/find_spu_functions.py", imgs / "pm_wwsjob.elf", "--out", imgs / "pm_wwsjob.functions.json")
    spu = PORT / "spu"
    keep = {"jobmods_extra_map.json"}
    if spu.exists():
        for c in spu.iterdir():
            if c.name in keep or c.name == "jobmod_50f371dc382d_e3D90":
                continue
            shutil.rmtree(c) if c.is_dir() else c.unlink()
    run(PY, TK / "tools/build_spu_workloads.py", "--images", imgs, "--lifted", spu,
        "--out", spu / "spu_workloads.c", "--register-fn", "infamous_spu_register_all", "--title", "inFamous",
        "--relift")
    print(f"{extract_jobmods(ELF, spu / 'jobmods')} WWS job modules")
    run(PY, TK / "tools/lift_jobmods.py", "--jobmods", spu / "jobmods", "--out", spu,
        "--register", spu / "jobmods_register.c", "--symbol-prefix", "infamous",
        env={"JOBMOD_EXTRA_MAP": str(spu / "jobmods_extra_map.json")})
    run(PY, TK / "tools/spu_regcache.py", spu)
    run(PY, TK / "tools/plug_lift.py", ELF, spu / "wwsplug", TK / "tools/plug_obs.json",
        env={"PLUG_PM_LS0": hex(PM_LS0)})
    run(PY, TK / "tools/spu_ibfast.py", spu)
    for f in sorted(spu.glob("jobmod_*/spu_recomp_rc.c")):
        run(PY, TK / "tools/spu_v200.py", f)
    run(PY, TK / "tools/spu_v200.py", spu / "wwsplug/spu_recomp_rc.c")


def gen_assets():
    usrdir = GAME / "PS3_GAME/USRDIR"
    steps = [
        (PORT, [PY, "tools/extract_ui_font.py", "english"]),
        (PORT, [PY, "tools/dump_icon_tex.py"]),
        (PORT / "tools", [PY, "make_kbm_icons.py"]),
        (ROOT, [PY, PORT / "tools/psarc.py", usrdir / "cache/psarc/install1/infamous1.psarc_s", "extract",
                "core.xpp", WORK / "xpp"]),
        (PORT / "tools", [PY, "make_kbm_prompts.py", WORK / "xpp/core.xpp", PORT / "assets/kbm_prompts.bin"]),
    ]
    (PORT / "assets").mkdir(exist_ok=True)
    for cwd, argv in steps:
        try:
            run(*argv, cwd=cwd, env={"INFAMOUS_GAME_DIR": str(usrdir.parents[1])})
        except subprocess.CalledProcessError as e:
            print(f"(optional asset step failed: {e})")


def build(jobs):
    bdir = PORT / "build-usa"
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    cc = os.environ.get("CC") or shutil.which("clang-cl")
    if not (cmake and ninja and cc):
        raise SystemExit("CMake, Ninja and clang-cl are required. Run from an x64 Visual Studio developer environment or use Setup's Install / repair.")
    run(cmake, "-S", PORT, "-B", bdir, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
        f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cc}", "-DCMAKE_BUILD_TYPE=Release",
        "-DPORT_PPU_REGCACHE=ON", "-DPORT_PPU_MEMLEAN=ALL", "-DPORT_PGO=OFF", "-DINFAMOUS_REGION=USA",
        f"-DPORT_PPU_MEMLEAN_LIST={PORT / 'tools/v243A_mem_hot_usa.txt'}", f"-DPORT_PPU_VR_LIST={PORT / 'tools/v241B_vr_hot_usa.txt'}",
        f"-DPS3RECOMP_DIR={TK}", f"-DPython3_EXECUTABLE={PY}")
    run(cmake, "--build", bdir, "--parallel", str(jobs))


def lift(jobs):
    """The PPU loader + lifter (installer/infamous.toml steps ppu-loader / ppu-lift) into WORK."""
    stamp = WORK / "recompiled/.lifted"
    if stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == USA_SHA:
        print("PPU lift is current")
        return
    run(PY, TK / "tools/ppu_loader.py", ELF, "--output", WORK / "ppu")
    run(PY, TK / "tools/ppu_lifter.py", ELF, "--functions", WORK / "ppu/EBOOT.functions.json",
        "--hle-stubs", WORK / "ppu/EBOOT.imports.json", "--output", WORK / "recompiled", "--jobs", jobs)
    stamp.write_text(USA_SHA, encoding="utf-8")


def main():
    global ELF, WORK, GAME
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, help="the decrypted BCUS98119 EBOOT.elf")
    ap.add_argument("--game", type=Path, help="the USA game dump (the folder that contains PS3_GAME)")
    ap.add_argument("--work", type=Path, help="scratch folder for the lift and extracted programs")
    ap.add_argument("--lift", action="store_true", help="lift the PPU code into <work> first")
    ap.add_argument("--skip-gen", action="store_true", help="keep the generated sources, only build")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-assets", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    WORK = a.work.resolve() if a.work else WORK
    ELF = a.elf.resolve() if a.elf else (WORK / "EBOOT.elf")
    GAME = a.game.resolve() if a.game else GAME
    WORK.mkdir(parents=True, exist_ok=True)
    if sha(ELF) != USA_SHA:
        sys.exit(f"{ELF} is not the BCUS98119 executable this port was mapped for (run tools/prepare_usa.py)")
    if a.lift and not a.skip_gen:
        lift(a.jobs)
    if not a.skip_gen:
        run(PY, ROOT / "tools/port_ppu_patches.py", "--src", WORK / "recompiled", "--out", PORT / "recompiled",
            "--report", WORK / "ppu-port-report.json")
        gen_spu()
        bad = patch_generated(("infamous-recomp/spu/", "infamous-recomp/spu-images/"))
        if not a.no_assets:
            gen_assets()
        if bad:
            print("\n".join(bad))
            sys.exit(f"{len(bad)} generated SPU files do not match the European lock")
    if not a.no_build:
        build(a.jobs)


if __name__ == "__main__":
    main()
