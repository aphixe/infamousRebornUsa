param(
    [string]$Executable = 'infamous-recomp/build-usa/inFamousRecomp.exe',
    [string]$Elf = '.local/usa/EBOOT.elf',
    [string]$GameRoot = '..\inFamous [BCUS98119]'
)
# local-4k branch: play the USA build at 3840x2160 (RES_SCALE 3) in a window.
# Alt+Enter or F11 = borderless fullscreen, F8 = release / recapture the mouse.
# The in-game Settings menu (settings.ini) still overrides the resolution once you change it there.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = (Resolve-Path -LiteralPath (Join-Path $root $Executable)).Path
$elf = (Resolve-Path -LiteralPath (Join-Path $root $Elf)).Path
$game = (Resolve-Path -LiteralPath (Join-Path $root $GameRoot)).Path
$python = Get-Command python -ErrorAction Stop
$json = & $python.Source -c 'import json,tomllib,sys; print(json.dumps(tomllib.load(open(sys.argv[1],"rb"))["launch"]["env"]))' (Join-Path $root 'installer\infamous.toml')
if ($LASTEXITCODE -ne 0) { throw 'Could not read the launch settings.' }
$env4k = $json | ConvertFrom-Json -AsHashtable
$env4k['RES_SCALE'] = '3'
$env4k['PS3_VFS_ROOT'] = $game
$env4k['LWCOND_SITE_POLL'] = '52493C:16'   # the USA address of the manifest's EU poll site
$env4k['PS3_TITLE'] = 'inFamous (USA) 4K'
foreach ($k in $env4k.Keys) { [Environment]::SetEnvironmentVariable($k, $env4k[$k], 'Process') }
$cwd = Join-Path $root 'infamous-recomp'
Start-Process -FilePath $exe -ArgumentList ('"' + $elf + '"') -WorkingDirectory $cwd -WindowStyle Normal `
    -RedirectStandardOutput (Join-Path $cwd 'play4k.out.log') -RedirectStandardError (Join-Path $cwd 'play4k.err.log')
