"""Package only the handwritten runtime volume changes, never generated code.

Uses only Python's standard library; no graphical interface is needed.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = [
    'ps3recomp/libs/audio/cellAudio.c',
    'ps3recomp/libs/audio/audio_volume.h',
    'ps3recomp/libs/video/rsx_d3d12_backend.c',
    'ps3recomp/libs/video/rsx_ui_overlay.c',
]


def digest(data):
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()


def main():
    out = ROOT / 'dist/release/v2.1/inFamous-v2.1-Volume-Quick-Update.zip'
    out.parent.mkdir(parents=True, exist_ok=True)
    entries = []
    for name in FILES:
        old = subprocess.run(['git', '-C', str(ROOT), 'show', f'2af34ea:{name}'], capture_output=True)
        if old.returncode and name != 'ps3recomp/libs/audio/audio_volume.h':
            raise SystemExit(f'Cannot find v2 base for {name}')
        base = [digest(old.stdout) if old.returncode == 0 else '']
        # Also accept the first volume-hotkey update (before INI persistence).
        if name == 'ps3recomp/libs/audio/cellAudio.c':
            base.append('6234d6bd7ce0d3aa7bcb41ce1c71ffef0e7a9c8787568b25a2a1002bc47d4de4')
        entries.append(dict(path=name, sha256=digest((ROOT / name).read_bytes()), base_sha256=base))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in FILES:
            z.write(ROOT / name, f'volume-update/files/{name}')
        z.writestr('volume-update/manifest.json', json.dumps(dict(files=entries), indent=2))
        z.write(ROOT / 'tools/Apply-VolumeUpdate.ps1', 'tools/Apply-VolumeUpdate.ps1')
        z.writestr('Apply Volume Update.cmd', '@echo off\r\ncd /d "%~dp0"\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\\Apply-VolumeUpdate.ps1"\r\nif errorlevel 1 (echo Update failed. See the message above.)\r\npause\r\n')
        z.writestr('Volume Update - Read Me.txt', '''inFamous v2.1 - Volume Quick Update for existing v2 installations

1. Close the game.
2. Extract this ZIP into your EXISTING installation folder (beside inFamous.exe).
3. Double-click Apply Volume Update.cmd and wait for completion.
4. Start the game normally.

Volume up: =, Shift+= (+), or numpad +.
Volume down: -, Shift+- (_), or numpad -.
Each step is 5 percentage points, limited to 0-100%. Hold to repeat.
The bottom-left indicator shows the actual game-session volume for 2.2 seconds.
Raising volume also unmutes the game. Only this game's Windows mixer slider changes.
The game saves each adjustment in infamous-recomp/audio.ini, under [audio] volume=0-100,
and restores it on the next normal launch. Debug launches explicitly start at their
requested debug volume without overwriting the saved value at startup.

Requires an already-built v2 installation with its build cache and C++ build tools.
Normally only 3 runtime C files compile, then the game relinks. No PPU/SPU lift,
disc extraction, Python packages, or full game rebuild. Linking can still take a few
minutes. A stale/incompatible cache is refused instead of starting a large rebuild.
The update validates source versions and backs up the executable and changed sources
under .installer/volume-update-backups. A failed build restores those files.
Saves, settings, launch configuration and game data are not edited.
No game executable, generated game code, or disc assets are included in this ZIP.
''')
    print(f'{out} ({out.stat().st_size:,} bytes)')


if __name__ == '__main__':
    main()
