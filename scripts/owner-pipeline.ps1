# One-command pipeline for the owner's own game files (Windows, PowerShell 5.1 or 7).
#
#   scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_174Q22P_0000008000000.00000000000O2"
#                              [-Work work] [-Report work\report.txt] [-Preset windows-clang-cl]
#                              [-SkipBuild] [-Codegen] [-Rexglue path\to\rexglue.exe]
#                              [-BuildHost] [-InstallSdk] [-ExtractGame]
#
# What it does, in order (docs/OWNER_CHECKLIST.md explains each step):
#   1. builds the tools with scripts\build.ps1 (skip with -SkipBuild);
#   2. identifies the disc image (ttt2-iso identify --image-hash);
#   3. identifies the title update (ttt2-stfs identify); -TitleUpdate may be a folder, in which case
#      the package whose Default.xexp targets 0.0.3.2 is picked, as TTT2Recomp's installer does;
#   4. extracts default.xex and Default.xexp into the work folder;
#   5. applies the title update with ttt2-xexpatch (Clang builds only) and hashes the result, then lists
#      the kernel/XAM imports (marking which ones the pinned ReXGlue runtime implements, stubs or lacks)
#      and the save/restore helper addresses (ttt2-xexinfo --imports --helpers);
#   6. with -Codegen, copies the patched executable to recomp\game\default.xex, runs the switch-table
#      finder on it (recomp\game\ttt2_switch_tables.toml, which the manifests include), runs
#      `rexglue codegen` on recomp\ttt2_manifest.toml and summarises it with recomp\tools\codegen_report.py;
#   7. with -BuildHost, builds the native host ttt2.exe from that code against the installed ReXGlue
#      SDK package (scripts\build-host.ps1; -InstallSdk builds and installs the SDK first, which takes
#      a long time); with -ExtractGame, extracts the whole disc into the work folder's game\ directory,
#      the folder ttt2.exe runs from (`ttt2.exe --game_data_root <work>\game`), and the title update's
#      files into update\ next to it (the host mounts that as update:).
# Everything the tools print is collected in the report file. The report holds hashes, versions,
# counts and addresses only; game files stay in the work folder and recomp\game (both git-ignored).
# Send the report file, never the work folder.
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$TitleUpdate,
    [string]$Work = "work",
    [string]$Report = "",
    [string]$Preset = "windows-clang-cl",
    [switch]$SkipBuild,
    [switch]$Codegen,
    [string]$Rexglue = "",
    [switch]$BuildHost,
    [switch]$InstallSdk,
    [switch]$ExtractGame
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$root = (Get-Location).Path
if (-not (Test-Path $Iso)) { throw "disc image not found: $Iso" }
if (-not (Test-Path $TitleUpdate)) { throw "title update not found: $TitleUpdate" }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
if ($Report -eq "") { $Report = Join-Path $Work "report.txt" }
$lines = New-Object System.Collections.Generic.List[string]

function Log([string]$text) {
    Write-Host $text
    $script:lines.Add($text)
}
function Section([string]$title) {
    Log ""
    Log ("=" * 78)
    Log $title
    Log ("=" * 78)
}
function Invoke-Tool([scriptblock]$command) {
    # Native tools write progress and warnings to stderr. Under Windows PowerShell 5.1 with
    # $ErrorActionPreference = "Stop", a 2>&1 redirection turns every such line into a terminating
    # NativeCommandError; run them with "Continue" and let the exit code speak (PowerShell 7 is unaffected).
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $command } finally { $ErrorActionPreference = $saved }
}
function Run([string]$exe, [string[]]$arguments) {
    # Runs a tool, echoes its output into the report, returns the output lines; never throws on
    # a non-zero exit code (the report records it).
    Log ("> " + (Split-Path $exe -Leaf) + " " + ($arguments -join " "))
    $output = Invoke-Tool { & $exe @arguments 2>&1 | ForEach-Object { $_.ToString() } }
    $code = $LASTEXITCODE
    foreach ($o in $output) { Log $o }
    if ($code -ne 0) { Log "(exit code $code)" }
    return ,@($output)
}

Section "Tekken Tag Tournament 2 Native Online - owner pipeline report"
Log ("Date:      " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"))
Log ("Machine:   " + [System.Environment]::OSVersion.VersionString + ", PowerShell " + $PSVersionTable.PSVersion)
$gitDescribe = (& git describe --always --dirty --tags 2>$null)
Log ("Checkout:  " + $(if ($gitDescribe) { $gitDescribe } else { "(not a git checkout)" }))
Log ("Preset:    " + $Preset)

# --- 1. tools ---------------------------------------------------------------------------------
Section "1. Build the tools"
if (-not $SkipBuild) {
    if ($Preset -eq "windows-clang-cl" -and -not (Test-Path "third_party\xenonrecomp\src\XenonUtils\xex_patcher.cpp")) {
        Log "Fetching the pinned XenonRecomp sources for ttt2-xexpatch..."
        Invoke-Tool { & scripts\fetch-xenonrecomp.ps1 2>&1 | ForEach-Object { Log $_.ToString() } }
    }
    Invoke-Tool { & scripts\build.ps1 -Preset $Preset 2>&1 | Select-Object -Last 15 | ForEach-Object { Log $_.ToString() } }
    if ($LASTEXITCODE -ne 0) { Log "BUILD FAILED (exit $LASTEXITCODE)"; $lines | Set-Content $Report; throw "build failed; see $Report" }
} else {
    Log "skipped (-SkipBuild)"
}
$binDirs = @("out\build\$Preset\bin\Release", "out\build\$Preset\bin")
$bin = $binDirs | Where-Object { Test-Path (Join-Path $_ "ttt2-iso.exe") } | Select-Object -First 1
if (-not $bin) { throw "built tools not found under out\build\$Preset\bin" }
$bin = (Resolve-Path $bin).Path
Log "Tools:     $bin"
$xexinfo = Join-Path $bin "ttt2-xexinfo.exe"
$isoTool = Join-Path $bin "ttt2-iso.exe"
$stfsTool = Join-Path $bin "ttt2-stfs.exe"
$patchTool = Join-Path $bin "ttt2-xexpatch.exe"
Run $xexinfo @("--version") | Out-Null

# --- 2. disc image ----------------------------------------------------------------------------
Section "2. Disc image"
Log "File:      $Iso ($([math]::Round((Get-Item $Iso).Length / 1GB, 2)) GB)"
Run $isoTool @("info", $Iso) | Out-Null
$isoIdentify = Run $isoTool @("identify", $Iso, "--image-hash")

# --- 3. title update --------------------------------------------------------------------------
Section "3. Title update"
$tuFile = $TitleUpdate
if ((Get-Item $TitleUpdate).PSIsContainer) {
    Log "Folder given; looking for the Tekken Tag Tournament 2 Title Update 3 package..."
    $tuFile = $null
    foreach ($candidate in Get-ChildItem -Path $TitleUpdate -File -Recurse | Where-Object { $_.Length -lt 256MB }) {
        $out = Invoke-Tool { & $stfsTool identify $candidate.FullName 2>&1 | ForEach-Object { $_.ToString() } }
        if ($LASTEXITCODE -eq 0 -and ($out -match "Identified:\s+Tekken Tag Tournament 2 Title Update 3")) {
            $tuFile = $candidate.FullName
            Log "Found:     $tuFile"
            break
        }
    }
    if (-not $tuFile) { Log "No package identified as Title Update 3 in $TitleUpdate"; $lines | Set-Content $Report; throw "title update not found; see $Report" }
}
Log "File:      $tuFile ($([math]::Round((Get-Item $tuFile).Length / 1MB, 2)) MB)"
$tuIdentify = Run $stfsTool @("identify", $tuFile)

# --- 4. extract -------------------------------------------------------------------------------
Section "4. Extract default.xex and Default.xexp into $Work"
$baseXex = Join-Path $Work "base.xex"
$patchXexp = Join-Path $Work "Default.xexp"
Run $isoTool @("extract", $Iso, "default.xex", $baseXex) | Out-Null
Run $stfsTool @("extract", $tuFile, "Default.xexp", $patchXexp) | Out-Null
Run $xexinfo @($baseXex, "--image-hash") | Out-Null
Run $xexinfo @($patchXexp) | Out-Null

# --- 5. patch ---------------------------------------------------------------------------------
Section "5. Apply the title update"
$patched = Join-Path $Work "default_tu3.xex"
$patchedOk = $false
if (Test-Path $patchTool) {
    $patchOut = Run $patchTool @($baseXex, $patchXexp, $patched)
    if ($LASTEXITCODE -eq 0 -and (Test-Path $patched)) {
        $patchedOk = $true
        Run $xexinfo @($patched, "--image-hash") | Out-Null
    }
} else {
    Log "ttt2-xexpatch.exe not built (preset $Preset): the patch tool needs the windows-clang-cl preset"
    Log "(install the 'C++ Clang tools for Windows' component, then rerun with -Preset windows-clang-cl)."
}

# --- 5b. imports and helpers -------------------------------------------------------------------
Section "5b. Imports (with the ReXGlue runtime's support for each) and save/restore helpers"
$scanned = $(if ($patchedOk) { $patched } else { $baseXex })
Log "Image:     $scanned"
Run $xexinfo @($scanned, "--imports", "--helpers", "--no-hash") | Out-Null

# --- 6. codegen -------------------------------------------------------------------------------
if ($Codegen) {
    Section "6. Recompile with rexglue codegen"
    if (-not $patchedOk) {
        Log "skipped: no patched executable"
    } else {
        if ($Rexglue -eq "") {
            $found = Get-ChildItem -Path "third_party\rexglue-sdk\src\out" -Recurse -Filter rexglue.exe -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -notmatch "CMakeFiles" } | Select-Object -First 1
            if ($found) { $Rexglue = $found.FullName }
        }
        if ($Rexglue -eq "" -or -not (Test-Path $Rexglue)) {
            Log "rexglue.exe not found: build it with scripts\build-rexglue-windows.ps1 or pass -Rexglue, or download"
            Log "the artifact of the 'ReXGlue codegen tool (Windows)' workflow."
        } else {
            New-Item -ItemType Directory -Force -Path "recomp\game" | Out-Null
            Copy-Item $patched "recomp\game\default.xex" -Force
            Log "Copied the patched executable to recomp\game\default.xex (git-ignored)."
            # Jump tables: the SDK's own detector misses or miscompiles several of the shapes TTT2's compiler
            # emits (measured), so the symbolic finder runs first and the manifests include its output.
            Section "6a. Switch jump tables (recomp\tools\find_switch_tables.py; the manifests include the result)"
            $tablesJson = Join-Path $Work "switch_tables.json"
            Run python @("-I", "recomp\tools\find_switch_tables.py", "recomp\game\default.xex", "--toml", "recomp\game\ttt2_switch_tables.toml", "--json", $tablesJson) | Out-Null
            if (-not (Test-Path "recomp\game\ttt2_switch_tables.toml")) { Log "PROBLEM: the finder wrote no recomp\game\ttt2_switch_tables.toml; codegen will fail (is Python 3 on PATH as 'python'?)" }
            Section "6b. Recompile"
            Push-Location recomp
            try {
                $codegenLog = Join-Path $Work "codegen.log"
                Log "> rexglue --log-level debug codegen --ignore-stamp ttt2_manifest.toml  (log: $codegenLog)"
                Invoke-Tool { & $Rexglue --log-level debug codegen --ignore-stamp ttt2_manifest.toml 2>&1 | ForEach-Object { $_.ToString() } | Set-Content $codegenLog }
                Log "(rexglue exit code $LASTEXITCODE; last lines:)"
                Get-Content $codegenLog | Select-Object -Last 25 | ForEach-Object { Log $_ }
            } finally { Pop-Location }
            $reportMd = Join-Path $Work "codegen-REPORT.md"
            $reportJson = Join-Path $Work "codegen-report.json"
            Run python @("-I", "recomp\tools\codegen_report.py", "recomp\generated\default", "--log", $codegenLog, "--project", "ttt2", "--md", $reportMd, "--json", $reportJson, "--quiet") | Out-Null
            if (Test-Path $reportMd) { Get-Content $reportMd | ForEach-Object { Log $_ } }

            # Second run with TTT2 Web's (CC0) explicit function boundaries, only when the image has the
            # same layout as TTT2 Web's (all eight save/restore helper addresses match); compare the reports.
            $helperScan = Invoke-Tool { & $xexinfo $patched --helpers --no-hash 2>&1 | ForEach-Object { $_.ToString() } }
            if ($helperScan -match "Layout vs TTT2 Web: 8 of 8") {
                Log "Layout matches TTT2 Web: recompiling again with its 18k function boundaries (ttt2_manifest.ttt2web.toml)"
                Push-Location recomp
                try {
                    $codegenLog2 = Join-Path $Work "codegen-ttt2web.log"
                    Invoke-Tool { & $Rexglue --log-level debug codegen --ignore-stamp ttt2_manifest.ttt2web.toml 2>&1 | ForEach-Object { $_.ToString() } | Set-Content $codegenLog2 }
                    Log "(rexglue exit code $LASTEXITCODE for the TTT2 Web variant)"
                } finally { Pop-Location }
                $reportMd2 = Join-Path $Work "codegen-ttt2web-REPORT.md"
                Run python @("-I", "recomp\tools\codegen_report.py", "recomp\generated\ttt2web", "--log", $codegenLog2, "--project", "ttt2", "--md", $reportMd2, "--json", (Join-Path $Work "codegen-ttt2web-report.json"), "--quiet") | Out-Null
                if (Test-Path $reportMd2) { Get-Content $reportMd2 | ForEach-Object { Log $_ } }
            } else {
                Log "Layout does not match TTT2 Web's published helper addresses: its function boundaries are not used."
            }
        }
    }
}

# --- 7. host build and game data ----------------------------------------------------------------
if ($BuildHost) {
    Section "7. Build the native host (ttt2.exe) against the installed ReXGlue SDK"
    if (-not $patchedOk) {
        Log "skipped: no patched executable"
    } else {
        $hostArgs = @("-Game", $patched)
        if ($InstallSdk) { $hostArgs += "-InstallSdk" }
        $hostOut = Invoke-Tool { & (Join-Path $PSScriptRoot "build-host.ps1") @hostArgs 2>&1 | ForEach-Object { $_.ToString() } }
        $hostCode = $LASTEXITCODE
        foreach ($o in $hostOut) { Log $o }
        if ($hostCode -ne 0) { Log "(host build exit code $hostCode)" }
        $listing = "recomp\out\build\win-amd64-release\host-listing.txt"
        if (Test-Path $listing) { Log "Host binaries:"; Get-Content $listing | ForEach-Object { Log "  $_" } }
    }
}
if ($ExtractGame) {
    Section "8. Extract the disc into $Work\game (the host's game data root)"
    $gameRoot = Join-Path $Work "game"
    Run $isoTool @("extract-all", $Iso, $gameRoot) | Out-Null
    # The title update's own files (next to game\, where the host mounts them as update:).
    $updateRoot = Join-Path $Work "update"
    Run $stfsTool @("extract-all", $tuFile, $updateRoot) | Out-Null
    Log "Title update files extracted to $updateRoot; the host picks that folder up as update: when it sits next to game\."
    Log "Run the host with: ttt2.exe --game_data_root `"$gameRoot`"  (keep the game folder private)"
}

Section "Done"
Log "Report written to $Report. Send this file (text only). Do NOT send anything from $Work, recomp\game or recomp\out."
$lines | Set-Content -Path $Report -Encoding UTF8
Write-Host "`nReport: $Report"
