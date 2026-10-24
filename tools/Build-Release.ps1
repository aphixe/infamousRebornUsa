param(
    [ValidatePattern('^[0-9][0-9A-Za-z.-]*$')][string]$Version = '2.1',
    [string]$OutputDirectory
)
# Maintainer entry point: fix formatting before checks, then build both EXEs.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "dist\release\v$Version" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$oldEncodedFlags = $env:CARGO_ENCODED_RUSTFLAGS
Push-Location -LiteralPath (Join-Path $root 'installer')
try {
    Write-Host 'Formatting installer sources and checking the same rule as GitHub Actions...'
    cargo fmt --all
    if ($LASTEXITCODE -ne 0) { throw 'cargo fmt failed. Install rustfmt with rustup component add rustfmt.' }
    cargo fmt --all -- --check
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed; release files were not created.' }

    $env:TEMP = Join-Path $root '.local\release-test-temp'
    $env:TMP = $env:TEMP
    New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
    $env:CARGO_ENCODED_RUSTFLAGS = if ($oldEncodedFlags) { $oldEncodedFlags + [char]31 + '-Dwarnings' } else { '-Dwarnings' }
    Write-Host 'Running installer tests with Cargo.lock enforced and warnings treated as errors...'
    cargo test --locked
    if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed; release files were not created.' }

    # Keep personal workspace/profile paths out of published Rust binaries.
    # Encoded flags keep paths containing spaces intact.
    $cargoRoot = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
    $rustupRoot = if ($env:RUSTUP_HOME) { $env:RUSTUP_HOME } else { Join-Path $env:USERPROFILE '.rustup' }
    $env:CARGO_ENCODED_RUSTFLAGS = @(
        '-Dwarnings',
        "--remap-path-prefix=$cargoRoot\registry\src=/cargo/registry",
        "--remap-path-prefix=$cargoRoot\git=/cargo/git",
        "--remap-path-prefix=$rustupRoot=/rustup",
        "--remap-path-prefix=$root=/src"
    ) -join [char]31
    cargo build --release --locked
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $setup = Join-Path $OutputDirectory "inFamous-v$Version-Setup.exe"
    $quick = Join-Path $OutputDirectory "inFamous-v$Version-Quick-Update.exe"
    Push-Location -LiteralPath $root
    try {
        & '.\installer\target\release\infamous-installer.exe' package --out $setup
        if ($LASTEXITCODE -ne 0) { throw 'Setup packaging failed.' }
        & (Join-Path $PSScriptRoot 'volume-update\Build-VolumeUpdater.ps1') -Output $quick
    } finally { Pop-Location }

    $checksums = foreach ($asset in @($setup, $quick)) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $asset)
    }
    [IO.File]::WriteAllLines((Join-Path $OutputDirectory 'SHA256SUMS.txt'), $checksums)
    Write-Host "Release EXEs and checksums are ready in $OutputDirectory"
    Write-Host 'Review and commit the formatted sources before pushing or tagging the release.'
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    $env:CARGO_ENCODED_RUSTFLAGS = $oldEncodedFlags
    Pop-Location
}
