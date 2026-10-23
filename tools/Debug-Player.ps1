param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Elf,
    [Parameter(Mandatory = $true)][string]$GameRoot,
    # Windows audio session volume of the player, percent (debug runs: 1 unless asked otherwise)
    [ValidateRange(0, 100)][int]$Volume = 1
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskExe = (Resolve-Path -LiteralPath $Executable).Path
$taskElf = (Resolve-Path -LiteralPath $Elf).Path
$taskGame = (Resolve-Path -LiteralPath $GameRoot).Path
$taskLog = Join-Path $taskRoot '.local\debug-player'
New-Item -ItemType Directory -Path $taskLog -Force | Out-Null

# Use the upstream launch environment, then apply this run's debug overrides.
$taskPython = Get-Command python -ErrorAction Stop
$taskEnvJson = & $taskPython.Source -c 'import json,tomllib,sys; print(json.dumps(tomllib.load(open(sys.argv[1],"rb"))["launch"]["env"]))' (Join-Path $taskRoot 'installer\infamous.toml')
if ($LASTEXITCODE -ne 0) { throw 'Could not read upstream launch settings.' }
$taskLaunchEnv = $taskEnvJson | ConvertFrom-Json -AsHashtable
$taskLaunchEnv['INFAMOUS_DEBUG_PLAYER'] = '1'
$taskLaunchEnv['FULLSCREEN'] = '0'
$taskLaunchEnv['RES_SCALE'] = '1'
$taskLaunchEnv['PS3_VFS_ROOT'] = $taskGame
$taskLaunchEnv['INFAMOUS_DEBUG_VOLUME'] = "$Volume"
$taskLaunchEnv['PS3_TITLE'] = "inFamous USA - Debug (windowed, audio $Volume%)"
$taskLaunchEnv['INFAMOUS_HANGDUMP'] = '15,30,60'
# The manifest's poll site is the EU address of the worker-pool wait; the USA build's is 0x52493C.
$taskLaunchEnv['LWCOND_SITE_POLL'] = '52493C:16'
$taskSavedEnv = @{}
try {
    foreach ($taskKey in $taskLaunchEnv.Keys) {
        $taskSavedEnv[$taskKey] = [Environment]::GetEnvironmentVariable($taskKey, 'Process')
        [Environment]::SetEnvironmentVariable($taskKey, $taskLaunchEnv[$taskKey], 'Process')
    }
    # Normal start: a hidden or minimised start state also applies to the game window's first ShowWindow.
    $taskPlayer = Start-Process -FilePath $taskExe -ArgumentList ('"' + $taskElf + '"') `
        -WorkingDirectory (Join-Path $taskRoot 'infamous-recomp') -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $taskLog 'player.out.log') `
        -RedirectStandardError (Join-Path $taskLog 'player.err.log') -PassThru
    Write-Output "Player PID: $($taskPlayer.Id). Logs: $taskLog"
    Write-Output "Debug mode: windowed (saved fullscreen setting overridden), Windows audio session at $Volume%."
} finally {
    foreach ($taskKey in $taskSavedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($taskKey, $taskSavedEnv[$taskKey], 'Process')
    }
}
