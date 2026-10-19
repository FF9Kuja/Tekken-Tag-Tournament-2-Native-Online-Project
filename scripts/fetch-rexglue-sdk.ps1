# Windows equivalent of scripts/fetch-rexglue-sdk.sh: clones the ReXGlue SDK at the pinned commit
# (third_party\rexglue-sdk\PIN) with its submodules and applies third_party\rexglue-patches\*.patch.
#   scripts\fetch-rexglue-sdk.ps1 [-Destination <dir>]
param([string]$Destination = "")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$pin = Get-Content (Join-Path $root "third_party\rexglue-sdk\PIN")
$url = ($pin | Where-Object { $_ -like "url=*" }) -replace "^url=", ""
$commit = ($pin | Where-Object { $_ -like "commit=*" }) -replace "^commit=", ""
if (-not $Destination) { $Destination = Join-Path $root "third_party\rexglue-sdk\src" }
if (Test-Path (Join-Path $Destination ".git")) {
    $current = (git -C $Destination rev-parse HEAD).Trim()
    if ($current -ne $commit) { throw "$Destination is at $current, expected $commit (remove it to refetch)" }
    Write-Host "rexglue-sdk already at $commit"
} else {
    # core.longpaths: the SDK's vulkan-loader submodule carries fuzzer test files whose paths exceed
    # Windows' 260-character limit (seen on the GitHub runner: "Filename too long").
    git -c core.longpaths=true clone --no-checkout $url $Destination
    if ($LASTEXITCODE -ne 0) { throw "clone failed" }
    git -c core.longpaths=true -C $Destination checkout --quiet $commit
    if ($LASTEXITCODE -ne 0) { throw "checkout failed" }
}
git -c core.longpaths=true -C $Destination submodule update --init --recursive --depth 1 --jobs 4
if ($LASTEXITCODE -ne 0) { throw "submodules failed" }
$patches = Join-Path $root "third_party\rexglue-patches"
if (Test-Path $patches) {
    Get-ChildItem $patches -Filter *.patch | Sort-Object Name | ForEach-Object {
        git -C $Destination apply --check $_.FullName 2>$null
        if ($LASTEXITCODE -eq 0) { git -C $Destination apply $_.FullName; Write-Host "applied $($_.Name)" }
        else {
            git -C $Destination apply --reverse --check $_.FullName 2>$null
            if ($LASTEXITCODE -eq 0) { Write-Host "already applied: $($_.Name)" } else { throw "patch does not apply: $($_.FullName)" }
        }
    }
}
Write-Host "rexglue-sdk ready at $Destination ($commit)"
