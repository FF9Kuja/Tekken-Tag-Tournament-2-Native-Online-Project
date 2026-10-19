# Configure + build + test on Windows with a CMake preset.
#   scripts\build.ps1 [-Preset windows-msvc|windows-clang-cl] [-Config Release|Debug]
# Requires Visual Studio 2022 (Desktop development with C++), CMake 3.25+ and,
# for windows-clang-cl, the "C++ Clang tools for Windows" component plus Ninja.
param(
    [string]$Preset = "windows-msvc",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Preset -eq "windows-msvc") {
    cmake --build --preset ($Preset + $(if ($Config -eq "Debug") { "-debug" } else { "" }))
} else {
    cmake --build --preset $Preset
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset $Preset -C $Config
exit $LASTEXITCODE
