# Runs the native host once on the player's game data and triages the run: the owner's boot step as one
# command (docs\OWNER_CHECKLIST.md step 6). Windows PowerShell 5.1 or PowerShell 7; runs under pwsh on
# Linux too (CI).
#
#   scripts\run-host.ps1 [-GameRoot work\game] [-Exe recomp\out\build\win-amd64-release\ttt2.exe] [-Work work]
#                        [-TimeoutSeconds 0] [-NoGpu] [-HostArgs "--log_level","debug"]
#
# What it does:
#   1. checks the host executable (default: what scripts\build-host.ps1 built), the game data root (it must
#      hold default.xex: ttt2-iso extract-all, or the pipeline's -ExtractGame) and Python 3;
#   2. starts the host from its own folder with --game_data_root <GameRoot> (plus -HostArgs; -NoGpu adds
#      --gpu_plugin=none, which runs without GPU emulation: log gathering only, the game cannot draw) and
#      waits for it to end. The host ends by itself when the game's main thread returns, and after a crash
#      (HOST CRASH report in the log, exit code 3). With -TimeoutSeconds N a host still running after N
#      seconds gets the close request a player's X button sends (WM_CLOSE through taskkill; SIGTERM on
#      Linux), on which it writes its kernel stub report and exits; one that ignores it for 20 s is killed.
#      Without a timeout, close the window yourself once nothing happens for a while (the stall dump needs
#      15 s of no change) and the triage follows.
#   3. runs recomp\tools\triage_boot_log.py on the log session the run wrote (the logs\ folder next to the
#      executable; the window shows nothing useful before the game draws) and writes <Work>\boot-triage.md
#      and .json, copies the log file(s) to <Work>\boot-logs\, and writes <Work>\boot-report.txt (how the
#      host ended, exit code, duration, the triage outcome line).
# Exit code: the host's (0, or 3 after a crash), 1 when the host had to be killed, 2 when a prerequisite is
# missing. Nothing here reads game data; the log, the triage and the report hold names, addresses and counts.
param(
    [string]$GameRoot = "",
    [string]$Exe = "",
    [string]$Work = "work",
    [int]$TimeoutSeconds = 0,
    [switch]$NoGpu,
    [string[]]$HostArgs = @()
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$root = (Get-Location).Path
$windows = ($env:OS -eq "Windows_NT")

function Fail([string]$text) { Write-Host "run-host: $text"; exit 2 }

function Find-Python {
    foreach ($name in @("python", "python3")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $major = & $cmd.Source -c "import sys; print(sys.version_info[0])" 2>$null
        if ($major -eq "3") { return $cmd.Source }   # the Store alias prints a message instead
    }
    return $null
}

function Invoke-Tool([scriptblock]$command) {
    # Under Windows PowerShell 5.1 with $ErrorActionPreference = "Stop", a 2>&1 redirection turns every
    # stderr line of a native tool into a terminating error; run them with "Continue" (PowerShell 7 is unaffected).
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $command } finally { $ErrorActionPreference = $saved }
}

# --- 1. prerequisites -----------------------------------------------------------------------------
if ($GameRoot -eq "") { $GameRoot = [IO.Path]::Combine("work", "game") }
if ($Exe -eq "") {
    if (-not $windows) { Fail "pass -Exe <path to the ttt2 executable> (no default location outside Windows)" }
    $Exe = [IO.Path]::Combine($root, "recomp", "out", "build", "win-amd64-release", "ttt2.exe")
}
if (-not (Test-Path $Exe -PathType Leaf)) { Fail "host executable not found: $Exe (scripts\build-host.ps1 builds it; docs\OWNER_CHECKLIST.md step 6)" }
$Exe = (Resolve-Path $Exe).Path
$exeDir = Split-Path $Exe
if (-not (Test-Path (Join-Path $GameRoot "default.xex") -PathType Leaf)) {
    Fail "no default.xex in the game data root $GameRoot (ttt2-iso extract-all <iso> $GameRoot, or the pipeline's -ExtractGame)"
}
$GameRoot = (Resolve-Path $GameRoot).Path
$python = Find-Python
if (-not $python) { Fail "Python 3 not found on PATH (winget install Python.Python.3.12): the triage tool needs it" }
$triageTool = [IO.Path]::Combine($root, "recomp", "tools", "triage_boot_log.py")
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$logDir = Join-Path $exeDir "logs"
$before = @{}
if (Test-Path $logDir) { Get-ChildItem $logDir -File | ForEach-Object { $before[$_.Name] = $true } }

# --- 2. run ---------------------------------------------------------------------------------------
$hostArgList = @("--game_data_root", $GameRoot) + $HostArgs
if ($NoGpu) { $hostArgList += "--gpu_plugin=none" }
# Start-Process joins the list with spaces; quote the elements that hold one.
$quoted = $hostArgList | ForEach-Object { if ($_ -match "\s") { '"' + $_ + '"' } else { $_ } }
$stdoutLog = Join-Path $Work "boot-stdout.log"
$stderrLog = Join-Path $Work "boot-stderr.log"
Write-Host "run-host: $Exe $($quoted -join ' ')"
Write-Host "run-host: the host writes its log to $logDir"
if ($TimeoutSeconds -gt 0) { Write-Host "run-host: a host still running after $TimeoutSeconds s gets a close request" }
else { Write-Host "run-host: close the window yourself once nothing happens for a while (a stall dump needs 15 s); the triage follows" }
$watch = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory $exeDir -PassThru -NoNewWindow `
    -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
$null = $p.Handle   # Windows PowerShell 5.1 reports no ExitCode without this
$ended = "by itself"
if ($TimeoutSeconds -gt 0) {
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        Write-Host "run-host: still running after $TimeoutSeconds s; sending the close request"
        if ($windows) { & taskkill /PID $p.Id | Out-Null } else { & /bin/kill -TERM $p.Id }
        if ($p.WaitForExit(20000)) {
            $ended = "after the close request ($TimeoutSeconds s timeout)"
        } else {
            Write-Host "run-host: no exit within 20 s of the close request; killing the host"
            $p.Kill()
            $p.WaitForExit()
            $ended = "killed (no exit within 20 s of the close request)"
        }
    }
} else {
    $p.WaitForExit()
}
$watch.Stop()
$code = $p.ExitCode
if ($null -eq $code) { $code = -1 }
if ($ended.StartsWith("killed")) { $code = 1 }
$seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)

# --- 3. the log session and its triage -------------------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("Tekken Tag Tournament 2 Native Online - host run report")
$lines.Add("Date:       " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"))
$lines.Add("Host:       $Exe")
$lines.Add("Game root:  $GameRoot")
$lines.Add("Arguments:  " + ($quoted -join " "))
$lines.Add("Ended:      $ended; exit code $code; $seconds s")
$newLogs = @()
if (Test-Path $logDir) {
    $newLogs = @(Get-ChildItem $logDir -File | Where-Object { -not $before.ContainsKey($_.Name) } | Sort-Object LastWriteTime)
}
$send = @()
if ($newLogs.Count -eq 0) {
    $lines.Add("Log:        none written to $logDir (the host did not get as far as opening its log)")
    foreach ($f in @($stdoutLog, $stderrLog)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
            $lines.Add("--- $(Split-Path $f -Leaf) (last 20 lines) ---")
            Get-Content $f | Select-Object -Last 20 | ForEach-Object { $lines.Add($_) }
            $send += $f
        }
    }
} else {
    $copyDir = Join-Path $Work "boot-logs"
    New-Item -ItemType Directory -Force -Path $copyDir | Out-Null
    foreach ($f in $newLogs) { Copy-Item $f.FullName (Join-Path $copyDir $f.Name) -Force; $send += (Join-Path $copyDir $f.Name) }
    $lines.Add("Log:        " + (($newLogs | ForEach-Object { $_.Name }) -join ", ") + " (copied to $copyDir)")
    $triageMd = Join-Path $Work "boot-triage.md"
    $triageJson = Join-Path $Work "boot-triage.json"
    $triageOut = Invoke-Tool { & $python -I $triageTool $logDir --md $triageMd --json $triageJson --quiet 2>&1 | ForEach-Object { $_.ToString() } }
    $triageCode = $LASTEXITCODE
    foreach ($o in $triageOut) { $lines.Add($o) }
    if ($triageCode -ne 0) { $lines.Add("(triage tool exit code $triageCode)") }
    if (Test-Path $triageMd) { $lines.Add("Triage:     $triageMd (and $triageJson)"); $send += $triageMd }
}
$report = Join-Path $Work "boot-report.txt"
$send += $report
$lines.Add("Send:       " + ($send -join ", "))
$lines.Add("Never send the game folder, recomp\game, recomp\generated or recomp\out.")
$lines | Set-Content -Path $report -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }
exit $code
