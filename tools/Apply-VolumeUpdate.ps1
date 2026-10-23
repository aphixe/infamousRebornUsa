param([string]$InstallRoot = (Split-Path -Parent $PSScriptRoot))
# Narrow update for an already-built v2 install. Never regenerate disc code.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $InstallRoot).Path
$payload = Join-Path $root 'volume-update'
$manifest = Get-Content -LiteralPath (Join-Path $payload 'manifest.json') -Raw | ConvertFrom-Json
$launch = Join-Path $root 'installer\launch.txt'
if (!(Test-Path -LiteralPath $launch)) { throw 'Extract this update into the installed folder containing inFamous.exe, then run Apply Volume Update.cmd.' }
$exeLine = Get-Content -LiteralPath $launch | Where-Object { $_.StartsWith('exe=') } | Select-Object -First 1
if (!$exeLine) { throw 'The installed launcher has no game executable path.' }
$exePath = $exeLine.Substring(4)
if (![IO.Path]::IsPathRooted($exePath)) { $exePath = Join-Path $root $exePath }
$exePath = [IO.Path]::GetFullPath($exePath)
if (!$exePath.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'The game executable must be inside this installation.' }
if (!(Test-Path -LiteralPath $exePath)) { throw 'This update needs an existing compiled game. Complete the original installation first.' }
if (Get-Process -Name inFamousRecomp -ErrorAction SilentlyContinue) { throw 'Close inFamous before applying the volume update.' }
$buildDir = Split-Path -Parent $exePath
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache) -or !(Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { throw 'The existing CMake/Ninja build cache is missing. Use the full Setup update instead.' }
$ninjaLine = Get-Content -LiteralPath $cache | Where-Object { $_ -match '^CMAKE_MAKE_PROGRAM:[^=]+=' } | Select-Object -First 1
if (!$ninjaLine) { throw 'No Ninja path was found in the existing build cache.' }
$ninja = $ninjaLine.Substring($ninjaLine.IndexOf('=') + 1)
if (!(Test-Path -LiteralPath $ninja)) { throw 'The Ninja build tool used by this installation is missing. Repair the build tools with Setup.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools are required for this incremental update.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path ($vs | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'Cannot find the installed x64 C++ build environment.' }
foreach ($p in @($root, $exePath, $ninja, $vcvars)) {
    if ($p -match '[&|<>^%!"\r\n]') { throw 'This quick updater cannot use paths containing command-shell special characters. Use Setup instead.' }
}
function SourceHash([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return '' }
    # Git/Setup copies may have either CRLF or LF line endings.
    $bytes = [Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText($Path)).Replace("`r`n", "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
# Check every input before changing any installed file. Also prevents overwriting
# a customized runtime or applying a patch to a different upstream version.
foreach ($entry in $manifest.files) {
    $dest = Join-Path $root $entry.path
    $incoming = Join-Path $payload ('files\' + $entry.path)
    if ((SourceHash $incoming) -ne $entry.sha256) { throw "Update payload verification failed: $($entry.path)" }
    $current = SourceHash $dest
    if ($current -ne $entry.sha256 -and $current -notin $entry.base_sha256) {
        throw "This is a different or customized source version: $($entry.path). No runtime files were changed. Use the full Setup update instead."
    }
}
$backup = Join-Path $root ('.installer\volume-update-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Copy-Item -LiteralPath $exePath -Destination (Join-Path $backup 'inFamousRecomp.exe')
$changed = @()
try {
    foreach ($entry in $manifest.files) {
        $dest = Join-Path $root $entry.path
        if ((SourceHash $dest) -eq $entry.sha256) { continue }
        $old = Join-Path $backup $entry.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $old) -Force | Out-Null
        $existed = Test-Path -LiteralPath $dest
        if ($existed) { Copy-Item -LiteralPath $dest -Destination $old }
        $changed += [pscustomobject]@{ path = $dest; old = $old; existed = $existed }
        Copy-Item -LiteralPath (Join-Path $payload ('files\' + $entry.path)) -Destination $dest
    }
    $target = Split-Path -Leaf $exePath
    $driver = Join-Path $backup 'incremental-build.cmd'
    $lines = @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b %errorlevel%', "`"$ninja`" -C `"$buildDir`" -j 4 `"$target`" %*")
    [IO.File]::WriteAllLines($driver, $lines, [Text.Encoding]::Default)
    function Invoke-CachedBuild([string]$ExtraArgs, [string]$LogPath) {
        # Raw ProcessStartInfo arguments avoid PowerShell 5.1's extra native
        # quoting when the installation path contains spaces. cmd consumes the
        # outer quote pair; the inner pair belongs to the driver path.
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $env:ComSpec
        $start.Arguments = '/d /s /c ""' + $driver + '" ' + $ExtraArgs + ' > "' + $LogPath + '" 2>&1"'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.WorkingDirectory = $root
        $process = [Diagnostics.Process]::Start($start)
        try { $process.WaitForExit(); return $process.ExitCode }
        finally { $process.Dispose() }
    }
    Write-Host 'Checking the existing build: no disc-code generation or clean rebuild.'
    $planLog = Join-Path $backup 'build-plan.log'
    if ((Invoke-CachedBuild '-n' $planLog) -ne 0) { throw 'The cached build could not be planned. See build-plan.log in the backup folder.' }
    $plan = Get-Content -LiteralPath $planLog
    $compileLines = @($plan | Where-Object { "$_" -match 'Building .+ object' })
    if ($compileLines.Count -gt 12 -or ($compileLines -match '[\\/]recompiled[\\/]|[\\/]spu[\\/]|[\\/]spu-images[\\/]') -or ($plan -match 'Re-running CMake')) {
        throw 'This cache needs more than the small runtime update. The quick update was cancelled; use Setup to update this installation.'
    }
    Write-Host "Compiling $($compileLines.Count) changed runtime file(s), then linking the game. Please wait..."
    $buildLog = Join-Path $backup 'build.log'
    if ((Invoke-CachedBuild '' $buildLog) -ne 0) { throw 'The incremental build failed. See build.log in the backup folder.' }
    Write-Host 'Volume update installed. Use = / + or numpad + to raise volume; - or numpad - to lower it.'
    Write-Host "Backups and build log: $backup"
} catch {
    foreach ($item in $changed) {
        if ($item.existed) { Copy-Item -LiteralPath $item.old -Destination $item.path -Force }
        else { Remove-Item -LiteralPath $item.path -Force }
    }
    Copy-Item -LiteralPath (Join-Path $backup 'inFamousRecomp.exe') -Destination $exePath -Force
    throw
}
