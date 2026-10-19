# Windows equivalent of scripts/fetch-xenonrecomp.sh: clones XenonRecomp at the pinned commit
# (third_party\xenonrecomp\PIN) with only the submodules ttt2-xexpatch needs. Requires git.
#   scripts\fetch-xenonrecomp.ps1 [-Destination <dir>]
param([string]$Destination = "")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$pin = Get-Content (Join-Path $root "third_party\xenonrecomp\PIN")
$url = ($pin | Where-Object { $_ -like "url=*" }) -replace "^url=", ""
$commit = ($pin | Where-Object { $_ -like "commit=*" }) -replace "^commit=", ""
$submodules = (($pin | Where-Object { $_ -like "submodules=*" }) -replace "^submodules=", "") -split " "
if (-not $Destination) { $Destination = Join-Path $root "third_party\xenonrecomp\src" }
if (Test-Path (Join-Path $Destination ".git")) {
    $current = (git -C $Destination rev-parse HEAD).Trim()
    if ($current -ne $commit) { throw "$Destination is at $current, expected $commit" }
    Write-Host "xenonrecomp already at $commit"
} else {
    git clone --no-checkout $url $Destination
    if ($LASTEXITCODE -ne 0) { throw "clone failed" }
    git -C $Destination checkout --quiet $commit
    if ($LASTEXITCODE -ne 0) { throw "checkout failed" }
}
foreach ($sm in $submodules) {
    git -C $Destination submodule update --init --depth 1 $sm
    if ($LASTEXITCODE -ne 0) { throw "submodule $sm failed" }
}
Write-Host "xenonrecomp ready at $Destination ($commit)"
