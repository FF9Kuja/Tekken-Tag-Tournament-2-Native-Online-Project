# Builds the native host, ttt2.exe: the recompiled code under recomp\ plus the ReXGlue SDK runtime,
# consumed as an installed CMake package (the SDK's own distribution model).
#
#   scripts\build-host.ps1 [-Game <patched default.xex>] [-SdkInstall <dir>] [-Preset win-amd64-release]
#                          [-InstallSdk]
#
# What it does:
#   1. -Game copies the player's own, Title-Update-3-patched executable to recomp\game\default.xex
#      (git-ignored; codegen reads it during the build). Without -Game the file must already exist.
#   2. Checks the SDK package under -SdkInstall (default: the SDK checkout's
#      out\install\win-amd64, which scripts\build-rexglue-windows.ps1 -Install produces); with
#      -InstallSdk it builds and installs the SDK first (long: runtime, GPU plugin, SDL3).
#   3. Configures recomp\ with the given preset (recomp\CMakePresets.json: clang + Ninja) and
#      CMAKE_PREFIX_PATH pointing at the package, then builds. `rexglue codegen` runs inside the
#      build (recomp\generated\rexglue.cmake), so the generated C++ never has to be run by hand.
#   4. Prints the executable and the DLLs copied next to it, and writes host-listing.txt into the
#      build directory (names and sizes only).
# Requirements: LLVM clang 20+ on PATH (winget install LLVM.LLVM), CMake 3.25+, Ninja, Visual Studio
# 2022 C++ workload (Windows SDK headers). Output: recomp\out\build\<preset>\ttt2.exe (git-ignored).
# No game content leaves the machine: the generated code and the game image stay in ignored folders.
param(
    [string]$Game = "",
    [string]$SdkInstall = "",
    [string]$Preset = "win-amd64-release",
    [switch]$InstallSdk
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$root = (Get-Location).Path
if ($SdkInstall -eq "") { $SdkInstall = Join-Path $root "third_party\rexglue-sdk\src\out\install\win-amd64" }

function Find-Clang {
    $c = Get-Command clang -ErrorAction SilentlyContinue
    if ($c) { return (Split-Path $c.Source) }
    $candidates = @("$env:ProgramFiles\LLVM\bin")
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vs) { $candidates += (Join-Path $vs "VC\Tools\Llvm\x64\bin") }
    }
    foreach ($dir in $candidates) { if (Test-Path (Join-Path $dir "clang.exe")) { return $dir } }
    return $null
}

# 1. the game image
$gameDir = Join-Path $root "recomp\game"
$gameXex = Join-Path $gameDir "default.xex"
if ($Game -ne "") {
    if (-not (Test-Path $Game)) { throw "game executable not found: $Game" }
    New-Item -ItemType Directory -Force -Path $gameDir | Out-Null
    Copy-Item $Game $gameXex -Force
    Write-Host "Copied $Game -> recomp\game\default.xex (git-ignored)"
}
if (-not (Test-Path $gameXex)) {
    throw "recomp\game\default.xex is missing: pass -Game <your patched default.xex> (docs\OWNER_CHECKLIST.md step 4)"
}

# 2. the SDK package
$config = Join-Path $SdkInstall "lib\cmake\rexglue\rexglueConfig.cmake"
if (-not (Test-Path $config)) {
    if ($InstallSdk) {
        Write-Host "SDK package not found at $SdkInstall; building and installing the SDK (this takes a long time)..."
        & (Join-Path $PSScriptRoot "build-rexglue-windows.ps1") -Install
        if ($LASTEXITCODE -ne 0) { throw "SDK build/install failed ($LASTEXITCODE)" }
    }
    if (-not (Test-Path $config)) {
        throw "ReXGlue SDK package not found at $SdkInstall (run scripts\build-rexglue-windows.ps1 -Install, or pass -SdkInstall / -InstallSdk)"
    }
}
Write-Host "SDK package: $SdkInstall"

# 3. configure and build the host
$clangDir = Find-Clang
if (-not $clangDir) { throw "clang.exe not found: install LLVM 20+ (winget install LLVM.LLVM)" }
$env:PATH = "$clangDir;$env:PATH"
foreach ($tool in @("cmake", "ninja")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH" }
}
Push-Location (Join-Path $root "recomp")
try {
    cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$SdkInstall"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "host build failed ($LASTEXITCODE)" }
    $buildDir = Join-Path (Get-Location).Path "out\build\$Preset"
} finally {
    Pop-Location
}

# 4. report
$exe = Join-Path $buildDir "ttt2.exe"
if (-not (Test-Path $exe)) { throw "build finished but $exe is missing" }
$listing = Get-ChildItem -Path $buildDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } |
    Sort-Object Name | ForEach-Object { "{0,12} {1}" -f $_.Length, $_.Name }
$listing | Set-Content (Join-Path $buildDir "host-listing.txt") -Encoding UTF8
Write-Host "Host built: $exe"
$listing | ForEach-Object { Write-Host "  $_" }
Write-Host "Run it from a folder that holds the extracted game data (docs\OWNER_CHECKLIST.md step 6)."
