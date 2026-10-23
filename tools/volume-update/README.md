# One-click volume update (no player compilation)

`inFamous-v2.1-Quick-Update.exe` is a handwritten Windows updater and volume companion. It embeds the current launcher, not a compiled game or game assets. It works with the existing USA or European player and needs no Python, compiler, or build cache on the user's PC.

Running it finds installations beside the updater, in the working folder, at `C:\Games\inFamous`, or through inFamous desktop/Start-menu shortcuts (including redirected known folders). If there is no unique match, it opens a folder picker. Close the game and launcher before updating. Protected install folders may require running the updater as administrator.

The updater backs up the old launcher under `.installer/volume-exe-backups`, installs a new `inFamous.exe`, and copies itself to `installer/infamous-volume.exe`. Saves, INIs, launch configuration, game files, and the compiled player are unchanged. A failed replacement restores the old launcher. To revert, close the game, restore the backed-up launcher, and remove the volume helper.

Start the game through its usual **inFamous shortcut** or **inFamous.exe**. The launcher starts the helper for that game process only. Directly launching the game EXE or an old CMD script bypasses the helper. The helper exits with the game and prevents duplicate instances. Players with the native v2.1 controls already compiled get no second handler or overlay.

The helper polls `=`, `+`, `-`, numpad `+` and numpad `-` while the game has focus, adjusts only that process's Windows audio session, unmutes positive volume, and saves/restores `infamous-recomp/audio.ini`. It draws a click-through, nonactivating dark/purple bottom-left overlay, with 24-pixel virtual padding and a 2.2-second timeout. It neither injects code nor modifies the game executable.

Maintainer build, after building the current Rust launcher:

```powershell
tools/volume-update/Build-VolumeUpdater.ps1
```

The companion is built with the static MSVC runtime. `volume-tests.cpp` exercises the real WASAPI session routing, volume changes, persistence, clamping, unmuting, native-player detection and the overlay paint path. Audio-session enumeration requires a normal desktop process; sandboxed tests may be denied access to Windows' session manager.
