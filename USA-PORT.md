# USA (BCUS98119) port

This branch builds the port from the USA disc. It starts from `whompay/inFAMOUSreborn` commit
`f1ed9678fa410ca00b448fd81a43ce343d1e7426`, whose port targets the European BCES00220 disc. The EU build is
unchanged: everything USA-specific is selected with the CMake option `INFAMOUS_REGION=USA`.

The USA dump beside the checkout is BCUS98119, decrypted executable SHA-256
`4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4`. The original dump is never modified.
Decryption, extracted programs and generated C++ are private files under `.local/usa` and the ignored
generated folders of `infamous-recomp/`.

## Build

From this checkout, with Python 3.11+, Visual Studio (clang-cl), CMake and Ninja (the installer's `doctor`):

```powershell
python tools/prepare_usa.py --lift   # decrypt a private copy of EBOOT.BIN, lift the PPU code
python tools/build_usa.py            # generate + patch every source, then build infamous-recomp/build-usa
```

`build_usa.py` must run in a Visual Studio x64 developer environment with LLVM on `PATH`
(`.local/build-usa.cmd` is an example). `--no-build`, `--skip-gen` and `--no-assets` split the work.

## How the European patch set is reused

The two executables are the same compiled game linked slightly differently.

| Item | European port | USA disc |
|---|---|---|
| PPU code | | shifted in 26 regions, -0xF0 .. +0x1A8 (`tools/eu2usa.py`) |
| TOC / OPD table / `.data` near the TOC | TOC `0x78BFC0` | 0x48 lower (TOC `0x78BF78`); some tables 0x50 lower |
| start of `.data` (language lists, class vtables) | | same addresses |
| `.bss` (clock, player, managers) | | same addresses |
| Embedded SPU programs (8), WWS policy module, WWS job modules (21) | | byte-identical, file offsets 0x500 lower |

**PPU** (`tools/port_ppu_patches.py`). The EU patches against the lifted PPU code are 44,612 mechanical
rewrites (coherent `stvx`/`dcbz`, the `vslo`/`vsl` operand-order fix, comment encoding) -- the USA lift has
exactly the same number of sites -- plus 30 hand-written hunks (mouse look, right-stick sites, taskwait hooks,
atomic counters, movie texture pool, cell probe, animation LOD, car probe, trainer movement). Each hand hunk is
found in the USA lift by its context with every address-like token wildcarded, the candidates are ranked by
literal agreement and by agreement with the EU->USA address deltas measured at ~3,100 anchors from the
mechanical hunks, and its tokens are rewritten through the EU->USA correspondence of the matched context.
A hunk with no unique, consistent match stops the tool. `.local/usa/ppu-port-report.json` lists every hunk,
where it went and every address it rewrote.

**SPU.** The images are extracted from their USA offsets and given the EU tree's names, so the lifters
produce byte-identical sources; the EU patches are then applied under the installer's strict before/after
SHA-256 checks from `installer/generated.lock`. The WWS plugin sources differ only in comments naming the
EBOOT file offset of each blob; they are checked and patched with those comments moved to the EU offsets
and written back with the USA ones. `plug_lift.py` takes the policy module's offset from `PLUG_PM_LS0`.

**Hand-written hooks.** `infamous-recomp/infamous_region.h` provides `INF_RA(eu, usa)` and the TOC. Every
guest address in the port's own code and the runtime was reviewed; the ones that move were retargeted and each
USA value was checked against the USA executable (code: the thunk/branch words and lifted function starts;
data: the stored values, e.g. the traffic literals 25,000,000 and 1000.0 and the draw-distance bands):

- `trainer.cpp`: 17 lifted functions called directly, ModifyHealth, entity refresh, collision on/off,
  CharSet/spawn calls, the entity-update freeze hook and its thunk signature (`b 0x2747B8` = `0x4BDBA044`).
- `stubs.cpp`: pause-menu native OPD TOC and script anchor OPD, animation-LOD kick site.
- `infamous_clock_probe.h`: clock pointer chain (`[TOC-0x7F08]` = `0x769554`); the clock itself is unmoved.
- `car_v233.h`: traffic literal pool and draw-distance table.
- `cellPad.c`: third-person camera right-stick site; `ppu_sysprx.cpp`: idle pool worker wait site
  (`LWCOND_SITE_POLL` default `52493C:16`; the launch manifest's EU value must be overridden for USA).
- `main.cpp`: window title, default EBOOT search, and the SP Movie Player's two strict lwcond waits
  (`PS3_LWCOND_STRICT_LR` default `186484,1864F4`, EU `18631C,18638C`).

Unchanged on purpose: `.bss` addresses (player, characters, XP, traffic manager, Cole's position), the
language lists (`0x720868`), the class vtables used by the trainer, and diagnostics of other titles in the
shared runtime (env-gated). The pause-menu patch and the clock probe verify the guest words before
touching anything and log and skip on a mismatch.

## Debug player

Any runtime verification must use a visible windowed player with audio at 1%. `INFAMOUS_DEBUG_PLAYER=1`
forces `FULLSCREEN=0` after loading saved settings and uses native resolution; the Windows WASAPI backend
sets **only the player's audio session** to `0.01` before playback starts, and fails audio initialisation
rather than play at an unknown volume.

```powershell
pwsh -File tools/Debug-Player.ps1 -Executable infamous-recomp/build-usa/inFamousRecomp.exe -Elf .local/usa/EBOOT.elf -GameRoot '..\inFamous [BCUS98119]'
```

Logs go to `.local/debug-player`.

## Runtime verification (2026-10-22)

Debug player, visible 1280x720 window, audio session at 1% (both confirmed in the log and on screen):

- Boots to the autosave notice, then the title screen (live 3D scene, ~60 FPS).
- Start with no save begins a new game: the blast opening plays and Cole is controllable in the crater with
  the first tutorial prompt (~40-45 FPS). The clock probe, trainer (F10) and LOD table hooks report installed;
  the strict movie waits and the idle-worker poll site are active at their USA addresses.
- Not yet exercised: the pause-menu Settings/Quit patch, longer play, saving/loading, the trainer features.

## Known gaps

- The keyboard/mouse HUD prompt textures (`kbm_prompts.bin`) are built from EU texture object ids that the
  USA `core.xpp` does not have; that optional asset is skipped (controller prompts show instead).
- The installer (`infamous-installer`) still accepts only BCES00220; USA builds go through
  `tools/build_usa.py`.
