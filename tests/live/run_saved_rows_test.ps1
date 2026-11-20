# Saved rows test: a row that exists only in the profile's settings.cfg (no environment variable) must reach the game at the next start.
# Catches code that reads a SNAP_* variable before hle/settings has loaded the profile (a static initializer runs before main()).
#
#   powershell -File tests\live\run_saved_rows_test.ps1 -Case fps [-BuildDir build\w\mybuild]
#   powershell -File tests\live\run_saved_rows_test.ps1 -Rows fps_target=120 -Expect '\[highfps\] pacing target 120 Hz' [-Reject <regex>] [-Seconds 60]
#
# One hidden game start per case (host\run_boot.ps1, one run slot) on a fresh test profile whose settings.cfg holds only the rows; the run is ended
# after -Seconds and its stderr log must match every -Expect pattern and no -Reject pattern. Never starts while build\play.lock exists.
# Own folders: build\testprofiles_saved, build\testsave_saved, build\testconfig_saved. Exit code: 0 PASS, 1 FAIL, 2 the run was unusable.
[CmdletBinding(PositionalBinding=$false)]
param([string]$Case, [string[]]$Rows = @(), [string[]]$Expect = @(), [string[]]$Reject = @(), [string[]]$Unset = @(), [int]$Seconds = 60, [string]$BuildDir, [switch]$List)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# case -> rows written to settings.cfg, the variables that must not be set by hand, what the log must (not) say
$cases = [ordered]@{
    fps = @{ Rows = @('fps_target=120'); Unset = @('SNAP_FPS'); Expect = @('\[highfps\] SNAP_FPS=120', '\[highfps\] pacing target 120 Hz at present 1\b') }
}
if ($List) { $cases.Keys | ForEach-Object { Write-Host $_ }; exit 0 }
if ($Case) {
    if (-not $cases.Contains($Case)) { Write-Host "no case '$Case' (-List shows them)"; exit 2 }
    $c = $cases[$Case]
    $Rows = @($c.Rows); $Expect = @($c.Expect)
    if ($c.Reject) { $Reject = @($c.Reject) }
    if ($c.Unset) { $Unset = @($c.Unset) }
}
if (-not $Rows.Count -or -not $Expect.Count) { Write-Host 'give -Case <name>, or -Rows <key=value> and -Expect <regex>'; exit 2 }
if ((Test-Path (Join-Path $root 'build\play.lock')) -and $env:SNAP_RUN_WHILE_PLAYING -ne '1') { Write-Host 'build\play.lock is there (someone is playing): not starting'; exit 2 }

$name = if ($Case) { $Case } else { 'rows' }
$pdir = Join-Path $root 'build\testprofiles_saved'
$cfg = Join-Path $root 'build\testconfig_saved'
Remove-Item -Recurse -Force $pdir, $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pdir 'p1\save'), $cfg, (Join-Path $root 'analysis\live_opts') | Out-Null
'{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
$now = [int][double]::Parse((Get-Date -UFormat %s))
@"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "Saved rows test", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
(@('# per-profile settings (saved rows test)') + $Rows) | Set-Content (Join-Path $pdir 'p1\settings.cfg') -Encoding ascii

$env:SNAP_PROFILES_DIR = $pdir
$env:SNAP_PROFILE = 'Saved rows test'
$env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_saved')
$env:SNAP_CONFIG_DIR = $cfg
$env:SNAP_INPUT_BACKGROUND = '1'
$env:SNAP_PROFILE_PICKER = '0'
$env:SNAP_SHADER_PRECOMPILE = 'playing'
foreach ($u in $Unset) { Remove-Item "Env:\$u" -ErrorAction SilentlyContinue }   # the point of the test: only the file names the value

$ra = @('-Timeout', "$Seconds"); if ($BuildDir) { $ra += @('-BuildDir', $BuildDir) }
$out = powershell -NoProfile -File host\run_boot.ps1 @ra run 2>&1
$m = $out | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1
if (-not $m) { Write-Host 'saved rows: no run id from run_boot'; $out | Select-Object -Last 12 | ForEach-Object { Write-Host "$_" }; exit 2 }
$rid = $m.Matches[0].Groups[1].Value
$stderr = Join-Path $root "analysis\boot\runs\$rid\boot_stderr.log"
if (-not (Test-Path $stderr)) { Write-Host "saved rows: no log $stderr"; exit 2 }
$fail = 0
$report = @("saved rows: case $name, rows $($Rows -join ', '), run $rid, log $stderr")
foreach ($e in $Expect) {
    $hit = Select-String -Path $stderr -Pattern $e | Select-Object -First 1
    if ($hit) { $report += "ok   expected /$e/: $($hit.Line.Trim())" } else { $report += "FAIL expected /$e/: not in the log"; $fail++ }
}
foreach ($r in $Reject) {
    $hit = Select-String -Path $stderr -Pattern $r | Select-Object -First 1
    if ($hit) { $report += "FAIL rejected /$r/: $($hit.Line.Trim())"; $fail++ } else { $report += "ok   rejected /$r/: not in the log" }
}
$report += "saved rows test: $(if ($fail) { "$fail FAILED" } else { 'PASS' })"
$report | Tee-Object -FilePath (Join-Path $root "analysis\live_opts\saved_$name.txt") | ForEach-Object { Write-Host $_ }
exit ([int]($fail -gt 0))
