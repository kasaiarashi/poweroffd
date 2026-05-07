# Build helper for poweroffd-gui.
# Sets up MSVC env (auto-discovers VS install via vswhere) and runs cargo.
# Usage: .\build.ps1 [check|build|run]   (default: build --release)

param(
    [string]$Action = "release"
)

$ErrorActionPreference = "Stop"

function Find-VsInstallPath {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    foreach ($vsw in $candidates) {
        if (Test-Path $vsw) {
            $p = & $vsw -latest -property installationPath 2>$null
            if ($p) { return $p }
        }
    }
    # Fallback: probe common paths
    foreach ($root in @("${env:ProgramFiles}\Microsoft Visual Studio",
                        "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
                        "D:\Softwares\Microsoft Visual Studio")) {
        if (Test-Path $root) {
            $found = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
                Where-Object { Test-Path "$($_.FullName)\VC\Tools\MSVC" } |
                Select-Object -First 1
            if ($found) { return $found.FullName }
        }
    }
    throw "Visual Studio install not found. Install Build Tools or VS 2022."
}

$vsPath = Find-VsInstallPath
Write-Host "VS install: $vsPath"

# Pick newest MSVC toolset
$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
$toolset = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolset) { throw "No MSVC toolset under $msvcRoot" }

# Pick newest Windows SDK
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkLibRoot = Join-Path $sdkRoot "lib"
$sdkVer = Get-ChildItem $sdkLibRoot -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "ucrt\x64") } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVer) { throw "No Windows SDK found under $sdkLibRoot" }

$msvcLib = Join-Path $toolset.FullName "lib\x64"
$msvcInc = Join-Path $toolset.FullName "include"
$msvcBin = Join-Path $toolset.FullName "bin\Hostx64\x64"
$ucrtLib = Join-Path $sdkVer.FullName "ucrt\x64"
$umLib   = Join-Path $sdkVer.FullName "um\x64"
$incRoot = Join-Path $sdkRoot "Include\$($sdkVer.Name)"

$env:LIB     = "$msvcLib;$ucrtLib;$umLib"
$env:INCLUDE = "$msvcInc;$incRoot\ucrt;$incRoot\um;$incRoot\shared"
if ($env:Path -notlike "*$msvcBin*") { $env:Path = "$msvcBin;$env:Path" }

Write-Host "MSVC: $($toolset.Name)   SDK: $($sdkVer.Name)"

Push-Location $PSScriptRoot
try {
    switch ($Action.ToLower()) {
        "check"   { cargo check }
        "debug"   { cargo build }
        "release" { cargo build --release }
        "run"     { cargo run --release }
        default   { cargo build --release }
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    Pop-Location
}
