param([string]$Output = 'dist/release/v2.1/inFamous-v2.1-Quick-Update.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required on the maintainer machine.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$work = Join-Path $root '.local\volume-exe-build'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$launcher = Join-Path $root 'installer\target\release\infamous-launch.exe'
if (!(Test-Path -LiteralPath $launcher)) { throw 'Build the current Rust launcher with cargo build --release first.' }
$icon = (Join-Path $root 'installer\assets\infamous.ico').Replace('\','/')
$launcherRc = $launcher.Replace('\','/')
$rc = Join-Path $work 'volume.rc'
[IO.File]::WriteAllText($rc, "1 ICON `"$icon`"`r`n201 RCDATA `"$launcherRc`"`r`n")
if (![IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $root $Output }
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null
$cpp = Join-Path $PSScriptRoot 'volume-update.cpp'
$res = Join-Path $work 'volume.res'
$obj = Join-Path $work 'volume.obj'
$driver = Join-Path $work 'build.cmd'
$commands = @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b %errorlevel%',
    "rc.exe /nologo /fo `"$res`" `"$rc`"", 'if errorlevel 1 exit /b %errorlevel%',
    "cl.exe /nologo /std:c++17 /EHsc /O2 /MT /W4 /Fo`"$obj`" /Fe`"$Output`" `"$cpp`" `"$res`" /link /SUBSYSTEM:WINDOWS /OPT:REF ole32.lib uuid.lib shell32.lib user32.lib gdi32.lib")
[IO.File]::WriteAllLines($driver,$commands,[Text.Encoding]::Default)
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $env:ComSpec
$start.Arguments = '/d /s /c ""' + $driver + '""'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$process = [Diagnostics.Process]::Start($start)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Volume updater build failed ($($process.ExitCode))" }
$process.Dispose()
Write-Output "Built: $Output"
