# One-command pipeline for the owner's own game files (Windows, PowerShell 5.1 or 7).
#
#   scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" [-TitleUpdate "D:\path\TU_174Q22P_0000008000000.00000000000O2"]
#                              [-Work work] [-Report work\report.txt] [-Preset windows-clang-cl]
#                              [-SkipBuild] [-Codegen] [-Rexglue path\to\rexglue.exe]
#                              [-BuildHost] [-SdkInstall <dir>] [-InstallSdk] [-ExtractGame] [-Boot] [-BootTimeout 0]
#                              [-BootRounds 3]
#
# What it does, in order (docs/OWNER_CHECKLIST.md explains each step):
#   1. builds the tools with scripts\build.ps1 (skip with -SkipBuild);
#   2. identifies the disc image (ttt2-iso identify --image-hash);
#   3. identifies the title update (ttt2-stfs identify); -TitleUpdate may be a folder, in which case
#      the package whose Default.xexp targets 0.0.3.2 is picked, as TTT2Recomp's installer does.
#      Without -TitleUpdate the pipeline runs in BASE-IMAGE MODE: steps 3 and 5 are skipped and every later
#      step works on the retail executable 0.0.0.2 from the disc, clearly labelled in the report (the base
#      game is the image TTT2 Web recompiled; Title Update 3 can be added later);
#   4. extracts default.xex (and Default.xexp when a title update is given) into the work folder;
#   5. applies the title update with ttt2-xexpatch (Clang builds only) and hashes the result, then lists
#      the kernel/XAM imports (marking which ones the pinned ReXGlue runtime implements, stubs or lacks)
#      and the save/restore helper addresses (ttt2-xexinfo --imports --helpers);
#   5c. PREPARES THE RECOMPILATION IMAGE: the retail executable is AES-encrypted and basic-compressed, which the
#      switch-table finder and rexglue codegen do not read. `ttt2-xexinfo --write-plain` writes its plain form
#      (every header kept, FILE_FORMAT_INFO set to encryption none / compression none, the decrypted and
#      decompressed PE image after the headers) to <work>\default_base_plain.xex (or default_tu3_plain.xex for
#      the patched image), reads it back and verifies it against the input. The original files are not touched.
#   6. with -Codegen, copies the prepared image to recomp\game\default.xex, runs the switch-table finder on it
#      (recomp\game\ttt2_switch_tables.toml, which the manifests include), runs `rexglue codegen` on
#      recomp\ttt2_manifest.toml through recomp\tools\resolve_functions.py and summarises it with
#      recomp\tools\codegen_report.py. The resolver turns each "target not in any function" error (functions
#      ReXGlue's analysis never registers: leaf functions entered only by a tail branch, blocks cut off their
#      function) into a [functions] entry in recomp\game\ttt2_resolved_functions.toml, but only after checking
#      the branch in the image and finding the same boundary in TTT2 Web's published table, which it uses only
#      when the image's helper fingerprint matches; then it reruns codegen. Its report is <work>\resolve-REPORT.md;
#   7. with -BuildHost, builds the native host ttt2.exe from that code against the installed ReXGlue
#      SDK package (scripts\build-host.ps1 -Game <prepared image>): -SdkInstall <dir> names the package (the
#      unzipped rexglue-sdk-install-win-amd64 artifact of the Windows workflow, whose bin\rexglue.exe is then used
#      for step 6 unless -Rexglue says otherwise), -InstallSdk builds and installs it first (long);
#   8. with -ExtractGame, extracts the whole disc into the work folder's game\ directory, the folder ttt2.exe runs
#      from (`ttt2.exe --game_data_root <work>\game`; the host decrypts the disc's default.xex itself when it
#      loads it), and the title update's files into update\ next to it (the host mounts that as update:);
#   9. with -Boot, runs the host on that game data through scripts\run-host.ps1: waits for it to end (or
#      closes its window after -BootTimeout seconds), triages its log (<work>\boot-triage.md) and copies the
#      log file to <work>\boot-logs\; the triage goes into the report too.
#   9b. when that run ended on the runtime's trap for a call to an address no recompiled function starts at
#      (a function only a pointer reaches, which ReXGlue's analysis never registers), resolve_functions.py boot
#      verifies each called address against the image and TTT2 Web's table and adds the verified ones; with
#      -Codegen and -BuildHost the pipeline then recompiles, rebuilds and boots again, up to -BootRounds runs
#      (<work>\resolve-boot-<n>.md says what each round added).
# A CRITICAL FAILURE STOPS THE PIPELINE: when a stage the later ones depend on fails (the disc or the title
# update cannot be read or extracted, the image cannot be prepared, the finder, codegen, the host build or the
# extraction fails), the report gets a "CRITICAL:" line naming the cause, is written, and the script throws
# instead of running the later steps on missing files.
# Everything the tools print is collected in the report file. The report holds hashes, versions,
# counts and addresses only; game files stay in the work folder and recomp\game (both git-ignored).
# Send the report file, never the work folder.
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [string]$TitleUpdate = "",
    [string]$Work = "work",
    [string]$Report = "",
    [string]$Preset = "windows-clang-cl",
    [switch]$SkipBuild,
    [switch]$Codegen,
    [string]$Rexglue = "",
    [switch]$BuildHost,
    [string]$SdkInstall = "",
    [switch]$InstallSdk,
    [switch]$ExtractGame,
    [switch]$Boot,
    [int]$BootTimeout = 0,
    [int]$BootRounds = 3
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$root = (Get-Location).Path
if (-not (Test-Path $Iso)) { throw "disc image not found: $Iso" }
if ($TitleUpdate -ne "" -and -not (Test-Path $TitleUpdate)) { throw "title update not found: $TitleUpdate" }
$baseMode = ($TitleUpdate -eq "")
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
if ($Report -eq "") { $Report = Join-Path $Work "report.txt" }
$lines = New-Object System.Collections.Generic.List[string]
$script:lastExit = 0

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
function Write-Report {
    $script:lines | Set-Content -Path $Report -Encoding UTF8
}
function Fail-Pipeline([string]$reason) {
    # A stage the later ones depend on failed: running them on missing or wrong files would only produce a
    # misleading report. Record the cause, write the report and stop.
    Log ""
    Log "CRITICAL: $reason"
    Log "The pipeline stopped here; the later steps did not run. Fix the cause and rerun the same command."
    Log "Report written to $Report (send this file, not the work folder)."
    Write-Report
    throw "pipeline stopped: $reason (see $Report)"
}
function ConvertTo-Line($item) {
    # Native stderr lines arrive as ErrorRecords under 2>&1; an empty one would print as its exception type.
    # PowerShell 7 colours a child script's error text with escape sequences, which the report does not need.
    $text = $(if ($item -is [System.Management.Automation.ErrorRecord]) { $item.Exception.Message } else { $item.ToString() })
    return ($text -replace "\x1b\[[0-9;]*[A-Za-z]", "")
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
    # Runs a tool, echoes its output into the report, returns the output lines and records the exit code in
    # $script:lastExit; never throws (the caller decides whether a failure is critical).
    Log ("> " + (Split-Path $exe -Leaf) + " " + ($arguments -join " "))
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) {
        Log "(command not found: $exe)"
        $script:lastExit = 127
        return ,@()
    }
    $output = Invoke-Tool { & $exe @arguments 2>&1 | ForEach-Object { ConvertTo-Line $_ } }
    $code = $LASTEXITCODE
    foreach ($o in $output) { Log $o }
    if ($code -ne 0) { Log "(exit code $code)" }
    $script:lastExit = $code
    return ,@($output)
}
function Invoke-ChildScript([string]$path, [object[]]$arguments, [int]$tail = 0) {
    # Runs one of our scripts in a child PowerShell (the one running this script). In-process, its native
    # tools (cmake, ninja, python) would write into this script's redirected error stream, where Windows
    # PowerShell 5.1 turns their first stderr line into a terminating error under the child's own
    # $ErrorActionPreference = "Stop". Output lines go to the report; $script:lastExit gets the exit code.
    $shell = (Get-Process -Id $PID).Path
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $path) + $arguments
    $output = Invoke-Tool { & $shell @argList 2>&1 | ForEach-Object { ConvertTo-Line $_ } }
    $script:lastExit = $LASTEXITCODE
    $shown = $(if ($tail -gt 0) { $output | Select-Object -Last $tail } else { $output })
    foreach ($o in $shown) { Log $o }
    return ,@($output)
}
function Find-Python {
    foreach ($name in @("python", "python3")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $major = & $cmd.Source -c "import sys; print(sys.version_info[0])" 2>$null
        if ($major -eq "3") { return $cmd.Source }   # the Store alias prints a message instead
    }
    return $null
}

Section "Tekken Tag Tournament 2 Native Online - owner pipeline report"
Log ("Date:      " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"))
Log ("Machine:   " + [System.Environment]::OSVersion.VersionString + ", PowerShell " + $PSVersionTable.PSVersion)
$gitDescribe = (& git describe --always --dirty --tags 2>$null)
Log ("Checkout:  " + $(if ($gitDescribe) { $gitDescribe } else { "(not a git checkout)" }))
Log ("Preset:    " + $Preset)
Log ("Mode:      " + $(if ($baseMode) { "BASE IMAGE - no Title Update 3 given; the retail executable 0.0.0.2 is used throughout" } else { "Title Update 3 applied to the retail executable" }))

# --- 1. tools ---------------------------------------------------------------------------------
Section "1. Build the tools"
if (-not $SkipBuild) {
    if ($Preset -eq "windows-clang-cl" -and -not (Test-Path "third_party\xenonrecomp\src\XenonUtils\xex_patcher.cpp")) {
        Log "Fetching the pinned XenonRecomp sources for ttt2-xexpatch..."
        Invoke-ChildScript (Join-Path $PSScriptRoot "fetch-xenonrecomp.ps1") @() | Out-Null
        if ($script:lastExit -ne 0) { Log "PROBLEM: fetching XenonRecomp failed (exit $($script:lastExit)); ttt2-xexpatch will be missing" }
    }
    Invoke-ChildScript (Join-Path $PSScriptRoot "build.ps1") @("-Preset", $Preset) 15 | Out-Null
    if ($script:lastExit -ne 0) { Fail-Pipeline "BUILD FAILED (exit $($script:lastExit)): the tools could not be built with preset $Preset" }
} else {
    Log "skipped (-SkipBuild)"
}
$binDirs = @("out\build\$Preset\bin\Release", "out\build\$Preset\bin")
$bin = $binDirs | Where-Object { Test-Path (Join-Path $_ "ttt2-iso.exe") } | Select-Object -First 1
if (-not $bin) { Fail-Pipeline "built tools not found under out\build\$Preset\bin (build them first, or drop -SkipBuild)" }
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
if ($script:lastExit -ne 0) { Fail-Pipeline "ttt2-iso cannot read $Iso as an Xbox 360 disc image (exit $($script:lastExit))" }
$isoIdentify = Run $isoTool @("identify", $Iso, "--image-hash")
if ($script:lastExit -ne 0) { Fail-Pipeline "the disc image could not be identified (ttt2-iso identify exit $($script:lastExit)); see the lines above" }

# --- 3. title update --------------------------------------------------------------------------
Section "3. Title update"
$tuFile = $null
if ($baseMode) {
    Log "skipped: no -TitleUpdate given (base-image mode). The retail executable 0.0.0.2 from the disc is used for"
    Log "every later step; docs\OWNER_CHECKLIST.md explains how to obtain Title Update 3 legitimately and rerun."
} else {
    $tuFile = $TitleUpdate
    if ((Get-Item $TitleUpdate).PSIsContainer) {
        Log "Folder given; looking for the Tekken Tag Tournament 2 Title Update 3 package..."
        $tuFile = $null
        foreach ($candidate in Get-ChildItem -Path $TitleUpdate -File -Recurse | Where-Object { $_.Length -lt 256MB }) {
            $out = Invoke-Tool { & $stfsTool identify $candidate.FullName 2>&1 | ForEach-Object { ConvertTo-Line $_ } }
            if ($LASTEXITCODE -eq 0 -and ($out -match "Identified:\s+Tekken Tag Tournament 2 Title Update 3")) {
                $tuFile = $candidate.FullName
                Log "Found:     $tuFile"
                break
            }
        }
        if (-not $tuFile) { Fail-Pipeline "no package identified as Title Update 3 in $TitleUpdate (run without -TitleUpdate for the base-image path)" }
    }
    Log "File:      $tuFile ($([math]::Round((Get-Item $tuFile).Length / 1MB, 2)) MB)"
    $tuIdentify = Run $stfsTool @("identify", $tuFile)
    if ($script:lastExit -ne 0) { Fail-Pipeline "ttt2-stfs cannot read $tuFile as a title update package (exit $($script:lastExit))" }
}

# --- 4. extract -------------------------------------------------------------------------------
Section $(if ($baseMode) { "4. Extract default.xex into $Work" } else { "4. Extract default.xex and Default.xexp into $Work" })
$baseXex = Join-Path $Work "base.xex"
$patchXexp = Join-Path $Work "Default.xexp"
Run $isoTool @("extract", $Iso, "default.xex", $baseXex) | Out-Null
if ($script:lastExit -ne 0 -or -not (Test-Path $baseXex)) { Fail-Pipeline "default.xex could not be extracted from the disc image (ttt2-iso extract exit $($script:lastExit))" }
Run $xexinfo @($baseXex, "--image-hash") | Out-Null
if ($script:lastExit -ne 0) { Fail-Pipeline "the extracted default.xex does not parse as an XEX2 executable (ttt2-xexinfo exit $($script:lastExit))" }
if (-not $baseMode) {
    Run $stfsTool @("extract", $tuFile, "Default.xexp", $patchXexp) | Out-Null
    if ($script:lastExit -ne 0 -or -not (Test-Path $patchXexp)) { Fail-Pipeline "Default.xexp could not be extracted from the title update package (ttt2-stfs extract exit $($script:lastExit))" }
    Run $xexinfo @($patchXexp) | Out-Null
    if ($script:lastExit -ne 0) { Fail-Pipeline "the extracted Default.xexp does not parse as an XEX2 patch (ttt2-xexinfo exit $($script:lastExit))" }
}

# --- 5. patch ---------------------------------------------------------------------------------
Section "5. Apply the title update"
$patched = Join-Path $Work "default_tu3.xex"
$patchedOk = $false
if ($baseMode) {
    Log "skipped: base-image mode (no Title Update 3)"
} elseif (Test-Path $patchTool) {
    $patchOut = Run $patchTool @($baseXex, $patchXexp, $patched)
    if ($script:lastExit -eq 0 -and (Test-Path $patched)) {
        $patchedOk = $true
        Run $xexinfo @($patched, "--image-hash") | Out-Null
    } else {
        Log "PROBLEM: the title update could not be applied (ttt2-xexpatch exit $($script:lastExit)); there is no patched image."
    }
} else {
    Log "ttt2-xexpatch.exe not built (preset $Preset): the patch tool needs the windows-clang-cl preset"
    Log "(install the 'C++ Clang tools for Windows' component, then rerun with -Preset windows-clang-cl)."
}
# The image the later steps recompile and boot: the patched executable, or in base-image mode the retail one.
# A failed patch attempt leaves no image (recompiling the base by accident would mislead the report).
$image = $null
$imageLabel = ""
if ($patchedOk) { $image = $patched; $imageLabel = "Title Update 3 image (0.0.3.2)" }
elseif ($baseMode) { $image = $baseXex; $imageLabel = "base retail executable 0.0.0.2 (no Title Update 3)" }
if (-not $image -and ($Codegen -or $BuildHost)) {
    Fail-Pipeline "the title update could not be applied, so there is no executable for -Codegen / -BuildHost; fix step 5 or run without -TitleUpdate (base-image mode)"
}

# --- 5b. imports and helpers -------------------------------------------------------------------
Section "5b. Imports (with the ReXGlue runtime's support for each) and save/restore helpers"
$scanned = $(if ($patchedOk) { $patched } else { $baseXex })
Log "Image:     $scanned"
Run $xexinfo @($scanned, "--imports", "--helpers", "--no-hash") | Out-Null
if ($script:lastExit -ne 0) { Log "PROBLEM: the import/helper scan failed (exit $($script:lastExit)); the report lacks it" }

# --- 5c. the recompilation image -----------------------------------------------------------------
# The finder and rexglue codegen read an unencrypted, uncompressed XEX; the disc's executable is neither
# (encryption normal, compression basic). The plain form keeps every header and carries the decrypted,
# decompressed image; it is verified after writing. The owner's first real run failed here (encrypted image
# handed to the finder, no switch tables, no codegen), hence a step of its own that stops on failure.
Section "5c. Prepare the recompilation image (plain XEX: headers kept, image decrypted and decompressed)"
$plain = $null
if (-not $image) {
    Log "skipped: no image to prepare (the title update was given but could not be applied)"
} else {
    $plain = Join-Path $Work $(if ($patchedOk) { "default_tu3_plain.xex" } else { "default_base_plain.xex" })
    Log "Image:     $imageLabel ($image)"
    if (Test-Path $plain) { Remove-Item $plain -Force }
    $prep = Run $xexinfo @($image, "--write-plain", $plain)
    if ($script:lastExit -ne 0 -or -not (Test-Path $plain) -or -not ($prep -match "Plain image:\s+verified")) {
        Fail-Pipeline "the plain image could not be prepared from $image (ttt2-xexinfo --write-plain exit $($script:lastExit)); the switch-table finder and rexglue codegen need an unencrypted, uncompressed executable"
    }
    Log "Prepared:  $plain ($([math]::Round((Get-Item $plain).Length / 1MB, 2)) MB). Steps 6 and 7 read this file; $image is unchanged."
}

# --- 6. codegen -------------------------------------------------------------------------------
# Steps 6, 7 and 9 are functions: when a boot ends on calls to functions ReXGlue never registered, step 9b
# adds the verified ones and runs them again (pass 2, 3, ... up to -BootRounds).
$triageMd = Join-Path $Work "boot-triage.md"
$triageJson = Join-Path $Work "boot-triage.json"
$script:codegenLog = $null

function Invoke-Codegen([int]$pass) {
    Section ("6. Recompile with rexglue codegen" + $(if ($pass -gt 1) { " (pass ${pass}: with the entries verified from the boot log)" } else { "" }))
    Log "Image:     $imageLabel, prepared as $plain"
    if ($script:Rexglue -eq "" -and $SdkInstall -ne "" -and (Test-Path (Join-Path $SdkInstall "bin\rexglue.exe"))) {
        $script:Rexglue = (Resolve-Path (Join-Path $SdkInstall "bin\rexglue.exe")).Path
    }
    if ($script:Rexglue -eq "") {
        $found = Get-ChildItem -Path "third_party\rexglue-sdk\src\out" -Recurse -Filter rexglue.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch "CMakeFiles" } | Select-Object -First 1
        if ($found) { $script:Rexglue = $found.FullName }
    }
    if ($script:Rexglue -eq "" -or -not (Test-Path $script:Rexglue)) {
        Fail-Pipeline "rexglue.exe not found: pass -SdkInstall <unzipped rexglue-sdk-install-win-amd64> or -Rexglue <path>, build it with scripts\build-rexglue-windows.ps1, or download the artifact of the 'ReXGlue codegen tool (Windows)' workflow"
    }
    $script:python = Find-Python
    if (-not $script:python) { Fail-Pipeline "Python 3 not found on PATH (winget install Python.Python.3.12): the switch-table finder needs it" }
    New-Item -ItemType Directory -Force -Path "recomp\game" | Out-Null
    Copy-Item $plain "recomp\game\default.xex" -Force
    Log "Copied the prepared image to recomp\game\default.xex (git-ignored)."
    # Jump tables: the SDK's own detector misses or miscompiles several of the shapes TTT2's compiler
    # emits (measured), so the symbolic finder runs first and the manifests include its output.
    Section "6a. Switch jump tables (recomp\tools\find_switch_tables.py; the manifests include the result)"
    $tables = "recomp\game\ttt2_switch_tables.toml"
    if (Test-Path $tables) { Remove-Item $tables -Force }
    $tablesJson = Join-Path $Work "switch_tables.json"
    Run $script:python @("-I", "recomp\tools\find_switch_tables.py", "recomp\game\default.xex", "--toml", $tables, "--json", $tablesJson) | Out-Null
    if ($script:lastExit -ne 0 -or -not (Test-Path $tables)) {
        Fail-Pipeline "the switch-table finder failed (exit $($script:lastExit)) or wrote no $tables; rexglue codegen cannot run without it"
    }
    Section "6b. Recompile (rexglue codegen through recomp\tools\resolve_functions.py)"
    $script:codegenLog = Join-Path $Work "codegen.log"
    $resolveMd = Join-Path $Work "resolve-REPORT.md"
    $resolveJson = Join-Path $Work "resolve-report.json"
    foreach ($old in @($resolveMd, $resolveJson)) { if (Test-Path $old) { Remove-Item $old -Force } }
    Get-ChildItem -Path $Work -Filter "codegen.round*.log" -ErrorAction SilentlyContinue | Remove-Item -Force
    Run $script:python @("-I", "recomp\tools\resolve_functions.py", "codegen", "--rexglue", $script:Rexglue,
        "--manifest", "recomp\ttt2_manifest.toml", "--image", "recomp\game\default.xex",
        "--out", "recomp\game\ttt2_resolved_functions.toml", "--reference", "recomp\ttt2_functions.ttt2web.toml",
        "--log", $script:codegenLog, "--report", $resolveMd, "--json", $resolveJson, "--ignore-stamp") | Out-Null
    $codegenCode = $script:lastExit
    if (Test-Path $resolveMd) { Log ""; Get-Content $resolveMd | ForEach-Object { Log $_ } }
    Log ""
    Log "(last lines of $($script:codegenLog))"
    if (Test-Path $script:codegenLog) { Get-Content $script:codegenLog | Select-Object -Last 25 | ForEach-Object { Log $_ } }
    if ($codegenCode -ne 0 -or -not (Test-Path "recomp\generated\default\sources.cmake")) {
        Fail-Pipeline "rexglue codegen failed (resolve_functions.py exit $codegenCode) or produced no recomp\generated\default\sources.cmake; see $resolveMd and $($script:codegenLog)"
    }
    $reportMd = Join-Path $Work "codegen-REPORT.md"
    $reportJson = Join-Path $Work "codegen-report.json"
    Run $script:python @("-I", "recomp\tools\codegen_report.py", "recomp\generated\default", "--log", $script:codegenLog, "--project", "ttt2", "--md", $reportMd, "--json", $reportJson, "--quiet") | Out-Null
    if (Test-Path $reportMd) { Get-Content $reportMd | ForEach-Object { Log $_ } } else { Log "PROBLEM: no codegen report was written (codegen_report.py exit $($script:lastExit))" }
    if ($pass -gt 1) { return }

    # Second run with TTT2 Web's (CC0) explicit function boundaries, only when the image has the
    # same layout as TTT2 Web's (all eight save/restore helper addresses match); compare the reports.
    $helperScan = Invoke-Tool { & $xexinfo $plain --helpers --no-hash 2>&1 | ForEach-Object { ConvertTo-Line $_ } }
    if ($helperScan -match "Layout vs TTT2 Web: 8 of 8") {
        Log "Layout matches TTT2 Web: recompiling again with its 18k function boundaries (ttt2_manifest.ttt2web.toml)"
        $codegenLog2 = Join-Path $Work "codegen-ttt2web.log"
        Push-Location recomp
        try {
            Invoke-Tool { & $script:Rexglue --log-level debug codegen --ignore-stamp ttt2_manifest.ttt2web.toml 2>&1 | ForEach-Object { ConvertTo-Line $_ } | Set-Content $codegenLog2 }
            Log "(rexglue exit code $LASTEXITCODE for the TTT2 Web variant; informative, the host builds from the default variant)"
        } finally { Pop-Location }
        $reportMd2 = Join-Path $Work "codegen-ttt2web-REPORT.md"
        Run $script:python @("-I", "recomp\tools\codegen_report.py", "recomp\generated\ttt2web", "--log", $codegenLog2, "--project", "ttt2", "--md", $reportMd2, "--json", (Join-Path $Work "codegen-ttt2web-report.json"), "--quiet") | Out-Null
        if (Test-Path $reportMd2) { Get-Content $reportMd2 | ForEach-Object { Log $_ } }
    } else {
        Log "Layout does not match TTT2 Web's published helper addresses: its function boundaries are not used."
    }
}

# --- 7. host build ------------------------------------------------------------------------------
function Invoke-HostBuild([int]$pass) {
    Section ("7. Build the native host (ttt2.exe) against the installed ReXGlue SDK" + $(if ($pass -gt 1) { " (pass $pass)" } else { "" }))
    Log "Image:     $imageLabel, prepared as $plain"
    $hostArgs = @("-Game", $plain)
    if ($SdkInstall -ne "") { $hostArgs += @("-SdkInstall", $SdkInstall) }
    if ($InstallSdk -and $pass -eq 1) { $hostArgs += "-InstallSdk" }
    Invoke-ChildScript (Join-Path $PSScriptRoot "build-host.ps1") $hostArgs | Out-Null
    $hostCode = $script:lastExit
    $hostExe = "recomp\out\build\win-amd64-release\ttt2.exe"
    $listing = "recomp\out\build\win-amd64-release\host-listing.txt"
    if ($hostCode -ne 0 -or -not (Test-Path $hostExe)) {
        Fail-Pipeline "the host build failed (exit $hostCode) or left no $hostExe; see the lines above (scripts\build-host.ps1)"
    }
    if (Test-Path $listing) { Log "Host binaries:"; Get-Content $listing | ForEach-Object { Log "  $_" } }
}

# --- 9. boot ------------------------------------------------------------------------------------
function Invoke-Boot([int]$pass) {
    # $true when the host ran (whatever the outcome), $false when a prerequisite is missing.
    Section ("9. Boot the host on the extracted game data (scripts\run-host.ps1) and triage its log" + $(if ($pass -gt 1) { " (pass $pass)" } else { "" }))
    $hostExe = "recomp\out\build\win-amd64-release\ttt2.exe"
    $bootRoot = Join-Path $Work "game"
    if (-not (Test-Path $hostExe)) {
        Log "skipped: $hostExe is missing (build it with -BuildHost or scripts\build-host.ps1)"
        return $false
    }
    if (-not (Test-Path (Join-Path $bootRoot "default.xex"))) {
        Log "skipped: no default.xex in $bootRoot (extract the disc with -ExtractGame)"
        return $false
    }
    foreach ($old in @($triageMd, $triageJson)) { if (Test-Path $old) { Remove-Item $old -Force } }
    $bootArgs = @("-GameRoot", $bootRoot, "-Work", $Work, "-TimeoutSeconds", $BootTimeout)
    Invoke-ChildScript (Join-Path $PSScriptRoot "run-host.ps1") $bootArgs | Out-Null
    $bootCode = $script:lastExit
    Log "(run-host exit code ${bootCode}: the host's own exit code (3 = host crash, 4 = the runtime aborted), 1 = closed by the timeout, 2 = prerequisites missing)"
    if (Test-Path $triageMd) {
        Log ""
        Log "--- $triageMd ---"
        Get-Content $triageMd | ForEach-Object { Log $_ }
    }
    return $true
}

if ($Codegen) { Invoke-Codegen 1 }
if ($BuildHost) { Invoke-HostBuild 1 }

# --- 8. game data -------------------------------------------------------------------------------
if ($ExtractGame) {
    Section "8. Extract the disc into $Work\game (the host's game data root)"
    $gameRoot = Join-Path $Work "game"
    Run $isoTool @("extract-all", $Iso, $gameRoot) | Out-Null
    if ($script:lastExit -ne 0 -or -not (Test-Path (Join-Path $gameRoot "default.xex"))) {
        Fail-Pipeline "the disc could not be extracted into $gameRoot (ttt2-iso extract-all exit $($script:lastExit))"
    }
    Log "The disc's default.xex is extracted as it is (retail form); the host decrypts and decompresses it itself when it loads it."
    # The title update's own files (next to game\, where the host mounts them as update:).
    if ($baseMode) {
        Log "No title update: nothing to extract into $Work\update (the host runs the base game without update: data)."
    } else {
        $updateRoot = Join-Path $Work "update"
        Run $stfsTool @("extract-all", $tuFile, $updateRoot) | Out-Null
        if ($script:lastExit -ne 0) { Fail-Pipeline "the title update's files could not be extracted into $updateRoot (ttt2-stfs extract-all exit $($script:lastExit))" }
        Log "Title update files extracted to $updateRoot; the host picks that folder up as update: when it sits next to game\."
    }
    Log "Run the host with: ttt2.exe --game_data_root `"$gameRoot`"  (keep the game folder private)"
}

# --- 9, 9b. boot, and the functions the boot shows ReXGlue missed -------------------------------
if ($Boot) {
    $rounds = [Math]::Max(1, $BootRounds)
    for ($pass = 1; $pass -le $rounds; $pass++) {
        $passStart = Get-Date
        if (-not (Invoke-Boot $pass)) { break }
        # A run that ended on the runtime's trap for a call to an unregistered function (a function only a
        # pointer reaches, such as a vtable slot without RTTI): verify each called address against the image and
        # TTT2 Web's table, add the verified ones, rebuild and boot again.
        $invalid = @()
        if (Test-Path $triageJson) {
            try {
                $tj = Get-Content $triageJson -Raw | ConvertFrom-Json
                if ($tj.invalid_calls) { $invalid = @($tj.invalid_calls) }
            } catch { $invalid = @() }
        }
        if ($invalid.Count -eq 0) { break }
        Section "9b. Calls to functions ReXGlue never registered: verify them from the boot log (recomp\tools\resolve_functions.py boot)"
        $newLogs = @(Get-ChildItem -Path (Join-Path $Work "boot-logs") -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $passStart.AddSeconds(-2) })
        if ($newLogs.Count -eq 0) { Log "PROBLEM: the triage reports calls to unregistered functions but this run left no log copy in $Work\boot-logs"; break }
        $cgLog = $(if ($script:codegenLog -and (Test-Path $script:codegenLog)) { $script:codegenLog }
                   elseif (Test-Path (Join-Path $Work "codegen.log")) { Join-Path $Work "codegen.log" }
                   else { "recomp\game\codegen.log" })
        $bootMd = Join-Path $Work "resolve-boot-$pass.md"
        $bootJson = Join-Path $Work "resolve-boot-$pass.json"
        foreach ($old in @($bootMd, $bootJson)) { if (Test-Path $old) { Remove-Item $old -Force } }
        $resolveArgs = @("-I", "recomp\tools\resolve_functions.py", "boot", "--image", "recomp\game\default.xex")
        foreach ($l in $newLogs) { $resolveArgs += @("--boot-log", $l.FullName) }
        $resolveArgs += @("--codegen-log", $cgLog, "--reference", "recomp\ttt2_functions.ttt2web.toml",
                          "--out", "recomp\game\ttt2_resolved_functions.toml", "--report", $bootMd, "--json", $bootJson)
        $py = $(if ($script:python) { $script:python } else { Find-Python })
        if (-not $py) { Log "PROBLEM: Python 3 not found on PATH; the boot log cannot be resolved"; break }
        Run $py $resolveArgs | Out-Null
        if (Test-Path $bootMd) { Log ""; Get-Content $bootMd | ForEach-Object { Log $_ } }
        $added = 0
        if (Test-Path $bootJson) {
            try {
                $bj = Get-Content $bootJson -Raw | ConvertFrom-Json
                if ($bj.added) { $added = @($bj.added).Count }
            } catch { $added = 0 }
        }
        if ($added -eq 0) { Log "No new verified entries: the boot cannot get further this way (the report above says why)."; break }
        if (-not ($Codegen -and $BuildHost)) { Log "$added verified entries were added to recomp\game\ttt2_resolved_functions.toml; rerun with -Codegen -BuildHost -Boot to rebuild with them."; break }
        if ($pass -ge $rounds) { Log "$added verified entries were added; the -BootRounds limit ($rounds) is reached: rerun the same command to recompile, rebuild and boot again."; break }
        Log "$added verified entries added: recompiling, rebuilding and booting again (pass $($pass + 1) of at most $rounds)."
        Invoke-Codegen ($pass + 1)
        Invoke-HostBuild ($pass + 1)
    }
}

Section "Done"
Log "Report written to $Report. Send this file (text only). Do NOT send anything from $Work, recomp\game or recomp\out."
if ($Codegen -and (Test-Path (Join-Path $Work "resolve-REPORT.md"))) { Log "Also send $(Join-Path $Work 'resolve-REPORT.md') (addresses only; it is in this report too)." }
if ($plain) { Log "The prepared image $plain is the game executable in decrypted form: keep it private like the disc image." }
if ($Boot -and (Test-Path $triageMd)) { Log "Also send $triageMd and the log copy in $(Join-Path $Work 'boot-logs') (names, addresses and counts only)." }
$bootResolve = @(Get-ChildItem -Path $Work -Filter "resolve-boot-*.md" -ErrorAction SilentlyContinue)
if ($Boot -and $bootResolve.Count -gt 0) { Log "Also send $(($bootResolve | ForEach-Object { $_.FullName }) -join ', ') (addresses only; in this report too)." }
Write-Report
Write-Host "`nReport: $Report"
