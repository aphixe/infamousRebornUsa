# inFamous PC (Europe + USA)

An unofficial native Windows port of **inFamous** (PlayStation 3, 2009). The game's PowerPC (PPU) and
SPU code is statically recompiled to x86-64 C/C++ with [ps3recomp](https://github.com/sp00nznet/ps3recomp),
so it runs as a normal Windows program rather than under an emulator.

This is a fork of [whompay/inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn) that adds the
**USA release (`BCUS98119`)** next to the European one (`BCES00220`). See [USA-PORT.md](USA-PORT.md)
for how the European port was carried over to the USA disc.

**This repository contains no game code or data.** You build the game yourself, on your own PC, from
your own copy of the game: a disc image (`.iso`) or a game folder.

## Supported discs

| Release | Title ID | Status |
|---|---|---|
| Europe | `BCES00220` | The upstream port |
| USA | `BCUS98119` (executable SHA-256 `4f2db3fb…f9befb4`) | Boots, title screen, new game into gameplay |

Other revisions of the USA executable need their own address map; the installer checks the hash and
stops rather than build something broken.

## Easy install

Download **inFamous Setup.exe** from the latest release, run it, choose your `.iso` (an encrypted 1:1
image works with its `.dkey` next to it) or your game folder (the one with `PS3_GAME` in it), pick an
install location and press **Install**.

Setup checks for Python and the C++ build tools and offers to install them, copies the game out of the
disc image, regenerates the recompiled code from your copy, builds it and adds an inFamous shortcut.
The first build takes roughly 20 to 60 minutes.

For the USA disc the installer never writes into your game folder: it decrypts a private copy of the
executable under `.installer/usa` and points the game at your files.

## Command-line install

The repository ships the command-line installer as `installer/bin/infamous-installer.exe`:

```bat
installer\bin\infamous-installer.exe install --game "D:\Dumps\inFamous [BCUS98119]" --rpcs3 "C:\RPCS3\rpcs3.exe"
installer\bin\infamous-installer.exe install --game "D:\Dumps\inFamous.iso"
```

`--game` takes a game folder or an `.iso`; `--dkey FILE` or `--key HEX` give the key of an encrypted
image. Run it without arguments (or drop a folder or image on it) to be asked for everything.
`infamous-installer doctor` checks the requirements only. Finished steps are skipped when you run it
again, so an interrupted install continues where it stopped.

When it finishes it writes `Play inFamous.cmd` and `inFamous.exe` (the launcher) in the checkout.

## Updating

**Update now** in Setup, the **Update inFamous** Start-menu shortcut, or `Update inFamous.cmd` in the
install folder compare the install with this repository (`aphixe/infamousRebornUsa`, branch `main`),
download only what changed and rebuild what that needs. Your saves and settings are kept.

## Requirements

- Your own inFamous disc or dump: European (`BCES00220`) or USA (`BCUS98119`).
- Windows 10 or 11 (64-bit) and a GPU with Direct3D 12 support.
- A CPU with SSE4.1 (2008 or newer). Without AVX2 the installer builds a slower compatibility version.
- [Python 3.10+](https://www.python.org/downloads/) with "Add python.exe to PATH" ticked.
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the
  **Desktop development with C++** workload and the **C++ Clang Compiler for Windows** component,
  plus CMake and Ninja (both come with that workload).
- [RPCS3](https://rpcs3.net) (with the PS3 firmware installed), to decrypt `EBOOT.BIN`. Retail discs
  ship it encrypted; the installer runs RPCS3 only to decrypt it.
- About 20 GB of free disk space (more if you install from an `.iso`, which is copied out) and 16 GB of RAM.

## Building from source

```bat
cd installer
cargo build --release
target\release\infamous-installer install --game "D:\Dumps\inFamous"
```

Maintainers: `python tools/build_usa.py` runs the USA generation and build by hand (USA-PORT.md), and
`infamous-installer export --out DIR` / `package --out "inFamous Setup.exe"` produce the publishable tree
and the one-file setup. See [installer/README.md](installer/README.md) for every step.

## Playing

Run **inFamous** (the shortcut Setup made, or `inFamous.exe` in the install folder). It opens a small
options window first (vsync, frame cap, resolution, language and more, saved to `settings.ini`); **Play**
starts the game. Controllers work as on the console; keyboard and mouse are supported (WASD to move,
mouse to look, Space = Cross, Esc = Start / pause).

**F10** opens the in-game trainer (God Mode, Infinite Energy, Super Speed, Player Model and more).
`TRAINER=0` turns it off.

**= / +** or **numpad +** raises game volume; **-** or **numpad -** lowers it, in 5% steps.
A padded bottom-left indicator shows the percentage. Adjustments are saved to
`infamous-recomp/audio.ini` (`[audio]`, `volume=0` through `100`) and restored on launch.
Explicit debug launches still start at the requested debug volume.

Your saves live in the install folder (`infamous-recomp/gamedata`); they are never part of an update or
a published tree.

## Known limits of the USA build

- Tested from boot through the opening of a new game. Longer play, saving and loading, the trainer and
  the pause menu's added Settings / Quit Game rows have not been checked on the USA disc yet.
- The keyboard/mouse HUD prompt textures are built from European texture IDs and are skipped on the
  USA disc; controller prompts show instead.

## How it works

| Path | Contents |
|---|---|
| `ps3recomp` | The recompiler toolkit and runtime: PPU and SPU lifters, HLE system libraries, RSX on Direct3D 12, audio and input |
| `infamous-recomp` | The inFamous-specific parts: program entry, game hooks and fixes (`infamous_region.h` holds the per-release addresses), asset tools |
| `installer` | Setup and the command-line installer: reads the disc, regenerates the code from your game, applies `installer/patches`, builds |
| `installer/infamous.toml` | Everything about the game and the port: pipeline steps, generated files, build and launch settings |
| `installer/generated.lock` | Hashes of each generated file before and after the port's patches |
| `tools/build_usa.py`, `tools/port_ppu_patches.py`, `tools/eu2usa.py` | The USA pipeline: relocates the European patches onto the USA executable |

## Credits

- [whompay/inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn), the European port this builds on.
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and contributors
  (see [ps3recomp/CONTRIBUTORS.md](ps3recomp/CONTRIBUTORS.md)).
- [RPCS3](https://rpcs3.net), used to decrypt the game executable.
- Sucker Punch Productions, for the game.
