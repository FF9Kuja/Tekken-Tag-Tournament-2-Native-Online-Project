# Builds the ReXGlue SDK codegen tool (rexglue.exe) on Windows, the way the SDK's own CI does
# (.github/workflows/_build-platform.yaml in the SDK: LLVM clang + Ninja + the win-amd64 preset).
#
#   scripts\build-rexglue-windows.ps1 [-SdkDir third_party\rexglue-sdk\src] [-Config Release]
#                                     [-Target rexglue] [-Install]
#
# Requirements (verified by the GitHub workflow .github/workflows/rexglue-windows.yml):
#   * Visual Studio 2022 with "Desktop development with C++" (MSVC libraries + Windows SDK headers
#     that clang targets; the D3D12 headers come from the Windows SDK),
#   * LLVM 20 or newer with clang, clang++ and lld on PATH: `winget install LLVM.LLVM` or
#     `choco install llvm`, or the VS component "C++ Clang Compiler for Windows" (20.x),
#   * CMake 3.25+ and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`).
# Run it from a "Developer PowerShell for VS 2022" if clang cannot find the MSVC headers.
# The SDK checkout comes from scripts\fetch-rexglue-sdk.ps1. Output: out\win-amd64\...\rexglue.exe
# inside the SDK directory (the script prints the exact path). No game content is involved.
param(
    [string]$SdkDir = "third_party\rexglue-sdk\src",
    [string]$Config = "Release",
    [string]$Target = "rexglue",
    [switch]$Install
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
if (-not (Test-Path (Join-Path $SdkDir "CMakePresets.json"))) {
    throw "SDK checkout not found at $SdkDir; run scripts\fetch-rexglue-sdk.ps1 first"
}

function Find-Clang {
    $c = Get-Command clang -ErrorAction SilentlyContinue
    if ($c) { return (Split-Path $c.Source) }
    $candidates = @("$env:ProgramFiles\LLVM\bin")
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vs) { $candidates += (Join-Path $vs "VC\Tools\Llvm\x64\bin") }
    }
    foreach ($dir in $candidates) {
        if (Test-Path (Join-Path $dir "clang.exe")) { return $dir }
    }
    return $null
}

$clangDir = Find-Clang
if (-not $clangDir) { throw "clang.exe not found: install LLVM 20+ (winget install LLVM.LLVM) or the VS Clang component" }
$env:PATH = "$clangDir;$env:PATH"
$versionLine = (& clang --version | Select-Object -First 1)
if ($versionLine -match "clang version (\d+)\.") {
    if ([int]$Matches[1] -lt 20) { Write-Warning "ReXGlue's wiki asks for Clang 20+ on Windows; found: $versionLine" }
} 
Write-Host "Using $versionLine from $clangDir"
foreach ($tool in @("cmake", "ninja")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH" }
}

Push-Location $SdkDir
try {
    cmake --preset win-amd64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    $buildTarget = if ($Install) { "install" } else { $Target }
    cmake --build out\build\win-amd64 --config $Config --target $buildTarget --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    $exe = Get-ChildItem -Recurse -Filter rexglue.exe -Path out -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch "CMakeFiles" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($exe) {
        Write-Host "rexglue: $($exe.FullName)"
        & $exe.FullName --version
    } else {
        Write-Warning "build finished but rexglue.exe was not found under $SdkDir\out"
    }
} finally {
    Pop-Location
}
