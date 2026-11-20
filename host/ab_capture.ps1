# One-run A/B capture for a graphics effect (docs/ab-capture.md). Rides a route to a named moment, freezes game time (the renderer keeps
# drawing the same frame state), sets the effect through each value, captures a frame and the per-pass GPU ms per value, checks the photo
# frame, unfreezes and exits. One run replaces the 2 to 4 Off/On runs.
#
#   powershell -File host\ab_capture.ps1 -Route core_florio_a -Moment ride40 -Effect lighting [-Values off,on] [-Set SNAP_LIGHTING=enhanced]
#
#   -Route    host\routes\<name>.txt, ridden normally until the moment (its A taps stop when the moment is met)
#   -Moment   ride<N> (rail progress N), a name from host\routes\ab_moments.txt, or a literal route `wait` condition (ride>=40, ui=photo_submit, film<=60)
#   -Effect   lighting (alias all): every effect that honors SNAP_LIGHT_AB / snaplight::ab_on (Enhanced lighting, SSR, atmosphere, lens, bounce light);
#             SNAP_* / NVN_*: a live environment variable for effects that read getenv every frame. Until runtime\nvn\nvn_dev.h lands.
#             opts: rows of the options menu, set the way the menu sets them: -Values 'atmos_fog=1|atmos_fog=-' (key=value, '-' = default, '|' between).
#   -Values   list set one after the other (default off,on). The first value is repeated at the end as a control (noise floor); -NoControl skips it.
#   -Set      launch-time configuration, NAME=value (e.g. SNAP_LIGHTING=enhanced SNAP_RENDER_SCALE=2)
#   -Settle   presents to wait after each change (default 12; raise it for temporal effects)   -Window  presents timed per value (default 24)
#   -Exclusive take all run slots (clean GPU timing for a cost run; one at a time on this PC)
#   -PerfCsv  time with SNAP_PERF_SHARED rows instead of the dev loop's named passes
#   -NoPhoto  skip the photo-frame check   -Name  output base name (default the moment)   -OutDir  default analysis\<effect>   -BuildDir  a worker build
# Takes one normal run slot through host\run_boot.ps1 (host\locks.ps1 fair queue, not exclusive). Exit code: 0 PASS, 10 photo FAIL, 11 photo INCONCLUSIVE,
# 20 the run failed.
[CmdletBinding(PositionalBinding=$false)]
param([Parameter(Mandatory)][string]$Route, [Parameter(Mandatory)][string]$Moment, [Parameter(Mandatory)][string]$Effect,
      [string[]]$Values = @('off', 'on'), [string[]]$Set = @(), [int]$Settle = 12, [int]$Window = 24, [switch]$NoPhoto, [switch]$NoControl,
      [string]$Name, [string]$OutDir, [string]$BuildDir, [int]$Timeout = 590, [switch]$KeepRun, [switch]$PerfCsv, [switch]$Exclusive)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
# -Effect opts: option rows, each value <key>=<value> ('-' = the row's default), separated by '|' (option values hold commas)
$sep = if ($Effect -eq 'opts') { '|' } else { ',' }
$Values = @($Values | ForEach-Object { $_.Split($sep) } | Where-Object { $_ })
if ($Values.Count -lt 2) { Write-Host 'need at least two values (for example off,on)'; exit 2 }

# ---- moment -> a route wait condition
$cond = $Moment
if ($Moment -match '^ride(\d+(\.\d+)?)$') { $cond = "ride>=$($Matches[1])" }
else {
    $mf = Join-Path $root 'host\routes\ab_moments.txt'
    if (Test-Path $mf) { foreach ($l in Get-Content $mf) { if ($l -match '^\s*([^#=\s]+)\s*=\s*(\S.*?)\s*$' -and $Matches[1] -eq $Moment) { $cond = $Matches[2] } } }
}
$rf = Join-Path $root "host\routes\$Route.txt"
if (-not (Test-Path $rf)) { Write-Host "no route file $rf"; exit 2 }
$base = if ($Name) { $Name } else { ($Moment -replace '[^A-Za-z0-9_.-]', '_') }
$effDir = ($Effect -replace '^(SNAP|NVN)_', '').ToLower() -replace '[^a-z0-9_-]', '_'
if (-not $OutDir) { $OutDir = Join-Path $root "analysis\$effDir" } elseif (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $root $OutDir }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$work = Join-Path $root "build\w\abcap_run_$stamp"; New-Item -ItemType Directory -Force $work | Out-Null
$tmpRoute = "_abcap_$stamp"
# generated route: the original, then a wait for the moment (the original's segment stops when the wait is met, so its shutter taps end there)
$lines = @(Get-Content $rf)
$lines += "wait $cond timeout=$($Timeout - 70)"
$lines += 'end 150'
Set-Content -Path (Join-Path $root "host\routes\$tmpRoute.txt") -Value $lines -Encoding ascii

$vals = @($Values); if (-not $NoControl) { $vals += $Values[0] }
$plan = Join-Path $work 'plan.txt'; $result = Join-Path $work 'result.json'; $perf = Join-Path $work 'perf'
@("moment=$cond", "effect=$Effect", "values=$($vals -join $sep)", "settle=$Settle", "window=$Window", "photo=$(if ($NoPhoto) { 0 } else { 1 })",
  "out=$result", "timeout=$($Timeout - 80)") | Set-Content $plan -Encoding ascii

# environment for the run (restored afterwards)
# timing: the dev loop (SNAP_DEV=1: live params and named GPU pass times, docs/devloop.md); -PerfCsv uses the older SNAP_PERF_SHARED / SNAP_GPU_PASSES rows instead
$envSet = @{ SNAP_ABCAP = $plan; NVN_FRAME_DUMP_FULL = '1' }
if ($PerfCsv) { $envSet.SNAP_PERF_SHARED = $perf; $envSet.SNAP_GPU_PASSES = "$perf`_passes.csv" }
else { $envSet.SNAP_DEV = '1'; $envSet.SNAP_DEV_TIMING = '1'; $envSet.SNAP_DEV_RENDERER_MARKS = '1' }
foreach ($s in $Set) { if ($s -match '^([^=]+)=(.*)$') { $envSet[$Matches[1]] = $Matches[2] } else { Write-Host "-Set expects NAME=value, got '$s'"; exit 2 } }
$old = @{}; foreach ($k in $envSet.Keys) { $old[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envSet[$k]) }
$runId = $null; $bootExit = 0
# One normal run slot from the fair queue, taken here; run_boot then runs with -NoLock in its own scratch dir (build\runs\slotnl<pid>), so the exe copy
# never collides with another run that shares slot 0's folder when the slot count is capped.
. (Join-Path $root 'host\locks.ps1')
$slots = Enter-RunSlots 'ab_capture' -Exclusive:$Exclusive
if ($null -eq $slots) { Write-Host 'ab_capture: could not get a run slot'; exit 20 }
$nlName = $null
try {
    $args2 = @('-NoProfile', '-File', (Join-Path $root 'host\run_boot.ps1'), '-NoLock', '-Route', $tmpRoute, '-Timeout', "$Timeout", '-ShotAt', '1000000', '-ShotEvery', '1000000', '-ShotMax', '400')
    if ($BuildDir) { $args2 += @('-BuildDir', $BuildDir) }
    Write-Host "ab_capture: route $Route, moment '$cond', effect $Effect, values $($vals -join ','), settle $Settle, window $Window"
    $log = Join-Path $work 'run_boot.out'
    & powershell @args2 2>&1 | Tee-Object -FilePath $log | ForEach-Object { if ("$_" -match '^run (\S+) on |exit code|killed|abcap|TIMEOUT|waiting for run slot') { Write-Host "$_" } }
    $bootExit = $LASTEXITCODE
    $m = Select-String -Path $log -Pattern '^run (\w+-\d{8}_\d{6}) on (slotnl\d+)' | Select-Object -First 1
    if ($m) { $runId = $m.Matches[0].Groups[1].Value; $nlName = $m.Matches[0].Groups[2].Value }
} finally {
    Exit-RunSlots
    foreach ($k in $old.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
    Remove-Item (Join-Path $root "host\routes\$tmpRoute.txt") -Force -ErrorAction SilentlyContinue
}
if ($nlName) { Remove-TreeSafe (Join-Path $root "build\runs\$nlName") }   # the private scratch copy of the exe
if (-not $runId -or -not (Test-Path $result)) {
    Write-Host "ab_capture: the run produced no result file (run $runId, exit $bootExit); see $log and analysis\boot\runs\$runId\boot_stderr.log"
    exit 20
}
$shots = Join-Path $root "analysis\boot\route_${tmpRoute}_$runId"
$cmp = @('-I', (Join-Path $root 'host\ab_compare.py'), '--result', $result, '--shots', $shots, '--passes', "$perf`_passes.csv", '--out-prefix', (Join-Path $OutDir "ab_$base"), '--route', $Route, '--run', $runId,
         '--notes', ("launch: " + (($Set + @()) -join ' ') + "; values " + ($vals -join ',')))
& (Join-Path $root 'tools\venv-recomp\Scripts\python.exe') @cmp
$cmpExit = $LASTEXITCODE
if (-not $KeepRun) {
    Get-ChildItem (Join-Path $root 'analysis\boot') -Filter "route_${tmpRoute}_${runId}*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $root 'analysis\boot') -Filter "route_${tmpRoute}_??.png" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem $work -Filter 'perf_*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}
Write-Host "ab_capture: outputs $(Join-Path $OutDir "ab_$base").png / .json (run $runId, work files in $work)"
exit $cmpExit
