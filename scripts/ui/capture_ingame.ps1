# Pictures of the game's own option screens at one window size, from hidden runs (frame dumps, so a stale screen grab cannot happen):
#   - the F1 overlay, every page (tests\hle\ui_polish_f1_script.txt) -> <Out>\f1_<page>_<size>.png
#   - every PC tab of the game's Settings screen (host\routes\ui_polish_native_tabs.txt) -> <Out>\native_NN_<size>.png
#   powershell -File scripts\ui\capture_ingame.ps1 -Out analysis\ui_polish\ingame_before -Size 1920x1080 [-BuildDir build\w\uipolish\exe] [-Stages f1,native]
# Each run takes a run slot through host\run_boot.ps1 (BelowNormal, hidden, muted); nothing starts while build\play.lock is live.
param([string]$Out = 'analysis\ui_polish\ingame_before', [string]$Size = '1920x1080', [string]$BuildDir = 'build\w\uipolish\exe', [string[]]$Stages = @('f1', 'native'))
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
if (Test-Path 'build\play.lock') { $pl = (Get-Content 'build\play.lock') -join ' '; if ($pl -match 'pid=(\d+)' -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)) { Write-Host "play.lock held: $pl"; exit 3 } }
New-Item -ItemType Directory -Force $Out | Out-Null
$w, $h = $Size -split 'x'
$cfg = Join-Path $root 'build\testconfig_uipolish'
Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg | Out-Null
'{"version":2,"mouse":{"lock_cursor":false}}' | Set-Content (Join-Path $cfg 'input.json')
$pdir = Join-Path $root 'build\testprofiles_uipolish'
Remove-Item -Recurse -Force $pdir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pdir 'p1\save') | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s))
@"
{"version": 1, "next": 2, "last_used": "p1", "profiles": [
 {"id": "p1", "name": "Ash", "created": $($now - 864000), "last_played": $($now - 86400)}], "pending": []}
"@ | Set-Content (Join-Path $pdir 'profiles.json') -Encoding ascii
$env:SNAP_PROFILES_DIR = $pdir; $env:SNAP_PROFILE = 'Ash'
$env:SNAP_SAVE_DIR = (Join-Path $root 'build\testsave_uipolish')
$env:SNAP_WINDOW_SAVE = '1'; $env:SNAP_CONFIG_DIR = $cfg
$env:SNAP_INPUT_BACKGROUND = '1'; $env:SNAP_PROFILE_PICKER = '0'; $env:SNAP_SHADER_PRECOMPILE = 'playing'
$env:SNAP_WINDOW_W = $w; $env:SNAP_WINDOW_H = $h

if ($Stages -contains 'f1') {
  $modsF1 = Join-Path $cfg 'mods'; New-Item -ItemType Directory -Force $modsF1 | Out-Null
  foreach ($m in 'alpha', 'beta') { $d = Join-Path $modsF1 $m; New-Item -ItemType Directory -Force $d | Out-Null; "{ `"name`": `"Test pack $m`", `"version`": `"1.0`", `"author`": `"test`", `"description`": `"Empty pack ($m).`" }" | Set-Content (Join-Path $d 'pack.json') }
  $env:SNAP_MODS_DIR = $modsF1
  $dump = Join-Path $root "build\uipolish_dump_f1_$Size"
  Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $dump | Out-Null
  $env:SNAP_UI_TEST = (Join-Path $root 'tests\hle\ui_polish_f1_script.txt')
  $env:NVN_FRAME_DUMP = $dump; $env:NVN_FRAME_DUMP_POST = '1'; $env:NVN_FRAME_DUMP_FULL = '1'
  $env:NVN_FRAME_DUMP_FROM = '27'; $env:NVN_FRAME_DUMP_EVERY = '1.5'; $env:NVN_FRAME_DUMP_MAX = '45'
  powershell -NoProfile -File host\run_boot.ps1 -Timeout 240 -BuildDir $BuildDir run | Select-Object -Last 6
  python -E -P tests\hle\ui_polish_shots.py $dump $Out $Size
  foreach ($n in 'SNAP_UI_TEST', 'NVN_FRAME_DUMP', 'NVN_FRAME_DUMP_POST', 'NVN_FRAME_DUMP_FULL', 'NVN_FRAME_DUMP_FROM', 'NVN_FRAME_DUMP_EVERY', 'NVN_FRAME_DUMP_MAX', 'SNAP_MODS_DIR') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
}
if ($Stages -contains 'native') {
  $env:SNAP_MODS_DIR = (Join-Path $root 'build\uipolish_mods')
  if (-not (Test-Path (Join-Path $env:SNAP_MODS_DIR 'native-options'))) { throw "no generated pack in $($env:SNAP_MODS_DIR): run python -I scripts\ui\make_native_options.py --out <dir> first" }
  $sf = Join-Path $cfg 'native_settings.cfg'; '' | Set-Content $sf
  $env:SNAP_NATIVE_OPT_SETTINGS = $sf; $env:SNAP_NATIVE_OPT_TRACE = '1'
  $env:NVN_FRAME_DUMP_FULL = '1'
  $res = powershell -NoProfile -File host\run_boot.ps1 -Timeout 290 -BuildDir $BuildDir -Route ui_polish_native_tabs
  $res | Select-Object -Last 8 | Write-Host
  $rid = ($res | Select-String -Pattern '^run (\S+) on slot' | Select-Object -First 1).Matches[0].Groups[1].Value
  $i = 0
  Get-ChildItem (Join-Path $root 'analysis\boot') -Filter "route_ui_polish_native_tabs_${rid}_??.png" | Sort-Object Name | ForEach-Object { Copy-Item $_.FullName (Join-Path $Out ('native_{0:00}_{1}.png' -f $i, $Size)) -Force; $i++ }
  Write-Host "$i native tab pictures"
  foreach ($n in 'SNAP_MODS_DIR', 'SNAP_NATIVE_OPT_SETTINGS', 'SNAP_NATIVE_OPT_TRACE', 'NVN_FRAME_DUMP_FULL') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
}
