# Tests the owner pipeline's boot-repair loop (scripts/owner-pipeline.ps1, Invoke-BootRepair) without a host,
# a game or a build. The loop and the helpers it uses are loaded from the pipeline's own source; the steps it
# drives (Invoke-Boot, Invoke-Codegen, Invoke-HostBuild, the resolver through Run, Get-FreeGb, the clock) are
# replaced by scripted fakes. Each case checks the runs, the rebuilds, the timeouts and why the loop stops:
# repair until a run ends some other way, no progress, a repeated trap, the run limit and its cap, the time
# limit, low disk space, a failed rebuild, a missing host, no -Codegen/-BuildHost.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\powershell\test_boot_repair.ps1   (Windows PowerShell 5.1)
#   pwsh -NoProfile -File tests/powershell/test_boot_repair.ps1                                (PowerShell 7)
#
# Exit code 0 when every case passes, 1 otherwise.
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot)
$source = Join-Path (Join-Path $root "scripts") "owner-pipeline.ps1"
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors) { throw "owner-pipeline.ps1 does not parse: $($parseErrors[0].Message)" }
$wanted = @("Format-Address", "Get-ResolvedCounts", "Read-Triage", "Invoke-BootRepair")
$loaded = @()
foreach ($f in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    if ($wanted -contains $f.Name) { . ([scriptblock]::Create($f.Extent.Text)); $loaded += $f.Name }
}
foreach ($name in $wanted) { if ($loaded -notcontains $name) { throw "owner-pipeline.ps1 has no function $name" } }

# --- fakes ------------------------------------------------------------------------------------------------
function Log([string]$text) { $script:lines.Add($text) }
function Section([string]$title) { $script:lines.Add("== $title") }
function Write-Report { $script:reports++ }
function Find-Python { return "python" }
function Get-FreeGb { return $script:freeGb }
function Get-Minutes($watch) {
    # Each call is the next scripted reading (the loop reads the clock once before each rebuild, once after it).
    if ($script:clock.Count -eq 0) { return 0.0 }
    $m = $script:clock[0]
    $script:clock = @($script:clock | Select-Object -Skip 1)
    return $m
}
function Invoke-Boot([int]$pass, [int]$timeout) {
    $script:timeouts += $timeout
    if ($script:noHost) { Log "skipped: recomp\out\build\win-amd64-release\ttt2.exe is missing"; return $false }
    $r = $script:runs[$pass - 1]
    $md = @("# Boot log triage", "", "## Outcome", "", "**$($r.Outcome)**", "")
    if ($r.Calls.Count -gt 0) {
        $md += @("## Timeline", "", "* FATAL: indirect call to $($r.Calls[0]), where no recompiled function starts", "")
        $md += @("## Calls to unregistered functions (runtime trap)", "")
        foreach ($c in $r.Calls) { $md += "* ``2026-10-20 10:00:0$pass.000`` indirect call to $c" }
    }
    $md | Set-Content -Path $triageMd -Encoding UTF8
    New-Item -ItemType Directory -Force -Path (Join-Path $Work "boot-logs") | Out-Null
    "log of run $pass" | Set-Content -Path (Join-Path (Join-Path $Work "boot-logs") "ttt2_$pass.log")
    return $true
}
function Run([string]$exe, [string[]]$arguments) {
    # The resolver's boot command: the scripted additions, in its summary-line form.
    $script:resolves++
    $r = $script:runs[$script:resolves - 1]
    $out = @("resolve_functions: all called addresses verified",
             "resolve_functions: boot summary: $($r.Calls.Count) called, $($r.Added.Count) added, 0 already present, 0 unverified")
    if ($r.Added.Count -gt 0) { $out += "resolve_functions: added " + ($r.Added -join " ") }
    if ($r.Present) { $out += "resolve_functions: already present " + ($r.Present -join " ") }
    return ,$out
}
function Invoke-Codegen([int]$pass) { $script:codegens += $pass }
function Invoke-HostBuild([int]$pass) {
    if ($script:buildFails) { throw "pipeline stopped: the host build failed (exit 1)" }
    $script:builds += $pass
}

# --- cases ------------------------------------------------------------------------------------------------
$failures = 0
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("ttt2-boot-repair-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work | Out-Null
$triageMd = Join-Path $Work "boot-triage.md"
$Reference = "recomp\ttt2_functions.ttt2web.toml"
$script:codegenLog = $null
$script:python = "python"
Push-Location $Work
New-Item -ItemType Directory -Force -Path (Join-Path "recomp" "game") | Out-Null
@('[functions]', '"0x82D6FC78" = {}  # entry: called through a pointer at run time (ttt2_1.log line 9)',
  '"0x82D6FC98" = {}  # entry: ahead of time, pointer at 0x820F0010 (.rdata)') |
    Set-Content -Path (Join-Path (Join-Path "recomp" "game") "ttt2_resolved_functions.toml")

function New-TrapRun([string]$address, [string[]]$added) {
    return @{ Outcome = "fatal: the game called $address through a pointer, where no recompiled function starts (the runtime aborts on such a call)"
              Calls = @($address); Added = @($added) }
}
function New-CleanRun([string]$outcome) { return @{ Outcome = $outcome; Calls = @(); Added = @() } }

function Invoke-Case([string]$name, [object[]]$runs, [hashtable]$opts, [scriptblock]$check) {
    $script:lines = New-Object System.Collections.Generic.List[string]
    $script:runs = $runs
    $script:timeouts = @(); $script:codegens = @(); $script:builds = @(); $script:resolves = 0; $script:reports = 0
    $script:freeGb = $(if ($opts.ContainsKey("FreeGb")) { $opts.FreeGb } else { 120.0 })
    $script:clock = $(if ($opts.ContainsKey("Clock")) { @($opts.Clock) } else { @() })
    $script:noHost = [bool]$opts.NoHost
    $script:buildFails = [bool]$opts.BuildFails
    $script:BootRounds = $(if ($opts.ContainsKey("BootRounds")) { $opts.BootRounds } else { 10 })
    $script:BootTimeout = $(if ($opts.ContainsKey("BootTimeout")) { $opts.BootTimeout } else { 300 })
    $script:BootMinutes = $(if ($opts.ContainsKey("BootMinutes")) { $opts.BootMinutes } else { 480 })
    $script:MinFreeGb = $(if ($opts.ContainsKey("MinFreeGb")) { $opts.MinFreeGb } else { 5 })
    $script:Codegen = -not $opts.NoRepair
    $script:BuildHost = -not $opts.NoRepair
    if (Test-Path (Join-Path $Work "boot-logs")) { Remove-Item (Join-Path $Work "boot-logs") -Recurse -Force }
    $thrown = $null
    try { Invoke-BootRepair } catch { $thrown = $_.Exception.Message }
    $text = ($script:lines -join "`n")
    $stopLine = @($script:lines | Where-Object { $_ -like "Stopped: *" }) -join ""
    try {
        & $check $text $stopLine $thrown
        Write-Host "ok   $name"
    } catch {
        $script:failures++
        Write-Host "FAIL $name`: $($_.Exception.Message)"
        Write-Host "     stop line: $stopLine"
        Write-Host "     runs: $($script:timeouts.Count), codegen passes: $($script:codegens -join ','), builds: $($script:builds -join ','), thrown: $thrown"
    }
}
function Expect([bool]$condition, [string]$what) { if (-not $condition) { throw $what } }

$A = "0x82D6FC78"; $B = "0x82D6FCB8"; $C = "0x8212F5E0"
Invoke-Case "repairs until a run ends some other way" @((New-TrapRun $A @($A)), (New-TrapRun $B @($B)), (New-CleanRun "stall: the thread census and stub calls stopped changing while the main thread ran; the window was closed afterwards")) @{} {
    param($text, $stop, $thrown)
    Expect ($script:timeouts.Count -eq 3) "3 runs expected, got $($script:timeouts.Count)"
    Expect (($script:codegens -join ",") -eq "2,3" -and ($script:builds -join ",") -eq "2,3") "rebuilds for passes 2 and 3 expected"
    Expect ($stop -like "Stopped: run 3 did not end on a call to an unregistered function*") "wrong stop: $stop"
    Expect ($text -match [regex]::Escape("run 1: fatal: the game called $A through a pointer") -and $text -match [regex]::Escape("added $A")) "run 1 line"
    Expect ($text -match "run 3: stall: ") "run 3 line"
    Expect ($text -match "3 runs in [0-9.,]+ minutes:") "summary header"
    Expect ($text -match "2 verified entries \(1 of them registered ahead of time\), kept for the next run") "resolved-file counts"
    Expect ($null -eq $thrown) "nothing thrown"
}
Invoke-Case "the first run keeps -BootTimeout 0, the automatic ones close after 300 s" @((New-TrapRun $A @($A)), (New-CleanRun "clean exit: the program returned from its entry point and the host ended")) @{ BootTimeout = 0 } {
    param($text, $stop, $thrown)
    Expect (($script:timeouts -join ",") -eq "0,300") "timeouts 0,300 expected, got $($script:timeouts -join ',')"
    Expect ($text -match "the runs after a rebuild close the game after 300 s") "loop banner"
}
Invoke-Case "a run whose log adds nothing stops the loop (no progress)" @((New-TrapRun $C @())) @{} {
    param($text, $stop, $thrown)
    Expect ($script:timeouts.Count -eq 1 -and $script:builds.Count -eq 0) "1 run, no rebuild"
    Expect ($stop -like "Stopped: NO PROGRESS: the log of run 1 gave no new verified entry*") "wrong stop: $stop"
    Expect ($text -match [regex]::Escape("run 1: fatal: the game called $C") -and $text -match "added nothing") "run line"
}
Invoke-Case "a trap at an address the loop already added stops it (repeated failure)" @((New-TrapRun $A @($A)), (New-TrapRun $A @($A))) @{} {
    param($text, $stop, $thrown)
    Expect ($script:timeouts.Count -eq 2 -and ($script:builds -join ",") -eq "2") "2 runs, 1 rebuild"
    Expect ($stop -like "Stopped: REPEATED FAILURE: the rebuilt host called $A again, although run 1 added the entry*") "wrong stop: $stop"
    Expect ($script:resolves -eq 1) "the repeated trap must not be resolved again"
}
Invoke-Case "a trap at an entry the host was built with is a repeated failure" @(@{ Outcome = "fatal: the game called $A through a pointer"; Calls = @($A); Added = @(); Present = @($A) }) @{} {
    param($text, $stop, $thrown)
    Expect ($script:builds.Count -eq 0) "no rebuild"
    Expect ($stop -like "Stopped: REPEATED FAILURE: run 1 called $A, which the resolved entries already list and this host was built with*") "wrong stop: $stop"
}
Invoke-Case "a trap at an entry an older host lacks asks for a rebuild" @(@{ Outcome = "fatal: the game called $A through a pointer"; Calls = @($A); Added = @(); Present = @($A) }) @{ NoRepair = $true } {
    param($text, $stop, $thrown)
    Expect ($stop -like "Stopped: run 1 called $A, which the resolved entries already list: this host was built before they were added; rerun with -Codegen -BuildHost -Boot") "wrong stop: $stop"
}
Invoke-Case "-BootRounds limits the runs" @((New-TrapRun $A @($A)), (New-TrapRun $B @($B)), (New-TrapRun $C @($C))) @{ BootRounds = 2 } {
    param($text, $stop, $thrown)
    Expect ($script:timeouts.Count -eq 2 -and ($script:builds -join ",") -eq "2") "2 runs, 1 rebuild"
    Expect ($stop -like "Stopped: the limit of 2 runs (-BootRounds) is reached; the verified entries are kept: rerun the same command to continue") "wrong stop: $stop"
}
Invoke-Case "-BootRounds is capped at 50" @((New-CleanRun "clean exit: the program returned from its entry point and the host ended")) @{ BootRounds = 1000 } {
    param($text, $stop, $thrown)
    Expect ($text -match "Boot repair: up to 50 runs \(-BootRounds 1000 is capped at 50\)") "cap message"
}
Invoke-Case "the time limit stops the loop before a rebuild that would exceed it" @((New-TrapRun $A @($A)), (New-TrapRun $B @($B)), (New-TrapRun $C @($C))) @{ BootMinutes = 100; Clock = @(10.0, 60.0, 70.0) } {
    param($text, $stop, $thrown)
    # before rebuild 1: 10 min used, no rebuild yet -> go; rebuild 1 took 60 min; before rebuild 2: 70 + 60 > 100 -> stop
    Expect ($script:timeouts.Count -eq 2 -and ($script:builds -join ",") -eq "2") "2 runs, 1 rebuild"
    Expect ($stop -like "Stopped: TIME LIMIT: 70 of the 100 minutes of -BootMinutes are used and the last rebuild took 60*") "wrong stop: $stop"
}
Invoke-Case "-BootMinutes 0 means no time limit" @((New-TrapRun $A @($A)), (New-CleanRun "clean exit: the program returned from its entry point and the host ended")) @{ BootMinutes = 0; Clock = @(100000.0, 50000.0) } {
    param($text, $stop, $thrown)
    Expect (($script:builds -join ",") -eq "2") "the rebuild must happen"
    Expect ($text -match "no time limit \(-BootMinutes 0\)") "banner"
}
Invoke-Case "low disk space stops the loop before a rebuild" @((New-TrapRun $A @($A))) @{ FreeGb = 3.2 } {
    param($text, $stop, $thrown)
    Expect ($script:builds.Count -eq 0 -and $script:codegens.Count -eq 0) "no rebuild"
    Expect ($stop -like "Stopped: LOW DISK SPACE: 3.2 GB free, less than the 5 GB a rebuild needs*") "wrong stop: $stop"
}
Invoke-Case "-MinFreeGb lowers the floor" @((New-TrapRun $A @($A)), (New-CleanRun "clean exit: the program returned from its entry point and the host ended")) @{ FreeGb = 3.2; MinFreeGb = 1 } {
    param($text, $stop, $thrown)
    Expect (($script:builds -join ",") -eq "2") "the rebuild must happen with 3.2 GB free and a 1 GB floor"
    Expect ($text -match "no rebuild with less than 1 GB free") "banner"
}
Invoke-Case "a failed rebuild ends the loop with its summary, then the error" @((New-TrapRun $A @($A))) @{ BuildFails = $true } {
    param($text, $stop, $thrown)
    Expect ($stop -like "Stopped: REBUILD FAILED before run 2: pipeline stopped: the host build failed (exit 1)") "wrong stop: $stop"
    Expect ($thrown -like "pipeline stopped: the host build failed*") "the error must be rethrown"
    Expect ($script:reports -eq 1) "the report must be written before the error"
}
Invoke-Case "without a host the loop reports it and stops" @() @{ NoHost = $true } {
    param($text, $stop, $thrown)
    Expect ($stop -like "Stopped: the host or the extracted game data is missing*") "wrong stop: $stop"
    Expect ($text -match "0 runs in") "summary header"
}
Invoke-Case "without -Codegen and -BuildHost the entries are added and the loop stops" @((New-TrapRun $A @($A))) @{ NoRepair = $true } {
    param($text, $stop, $thrown)
    Expect ($script:builds.Count -eq 0) "no rebuild"
    Expect ($stop -like "Stopped: 1 verified entries were added to recomp\game\ttt2_resolved_functions.toml; rerun with -Codegen -BuildHost -Boot*") "wrong stop: $stop"
    Expect ($text -notmatch "Boot repair: up to") "no loop banner without a rebuild"
}

# The helpers on their own.
function Check([string]$name, [bool]$condition) {
    if ($condition) { Write-Host "ok   $name" } else { $script:failures++; Write-Host "FAIL $name" }
}
Check "Format-Address pads and upper-cases" ((Format-Address "0x82d6fc78") -eq "0x82D6FC78" -and (Format-Address "0x2D6FC78") -eq "0x02D6FC78" -and (Format-Address "junk") -eq "junk")
@("# Boot log triage", "", "## Outcome", "", "**fatal: the game called 0x82D6FC78 through a pointer**", "",
  "* FATAL: indirect call to 0x82D6FC78, where no recompiled function starts",
  "* ``t`` indirect call to 0x82D6FC78", "* ``t`` indirect call to 0x82D6FCB8") | Set-Content -Path $triageMd -Encoding UTF8
$t = Read-Triage
Check "Read-Triage reads the outcome and each called address once" ($t.Outcome -eq "fatal: the game called 0x82D6FC78 through a pointer" -and ($t.Calls -join ",") -eq "0x82D6FC78,0x82D6FCB8")
Remove-Item $triageMd -Force
$t = Read-Triage
Check "Read-Triage without a triage" ($t.Calls.Count -eq 0 -and $t.Outcome -like "no triage was written*")
$counts = Get-ResolvedCounts
Check "Get-ResolvedCounts counts the entries and the ahead-of-time ones" ($counts[0] -eq 2 -and $counts[1] -eq 1)

Pop-Location
Remove-Item $Work -Recurse -Force
if ($failures -gt 0) { Write-Host "$failures case(s) failed"; exit 1 }
Write-Host "all boot-repair cases passed"
exit 0
