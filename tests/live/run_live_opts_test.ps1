# Live options test: rows of the options menu are changed in the middle of a ride and the picture must change with no restart.
#
#   powershell -File tests\live\run_live_opts_test.ps1 -Group atmos [-BuildDir build\w\mybuild] [-Route core_florio_a] [-Moment ride40]
#   powershell -File tests\live\run_live_opts_test.ps1 -Rows 'atmos_fog=1|atmos_fog=-' [-Set SNAP_LIGHTING=enhanced] [-Same key,key]
#
# One game run per group (host\ab_capture.ps1 -Effect opts): the route is ridden to the moment, game time is frozen, each row is set the way the
# menu sets it and one frame is dumped per step. tests\live\live_opts_check.py then checks that every row is a live row and that its frame differs
# from the frame before it by more than the noise floor (two control frames at the start). Each row is changed and then put back, so both
# directions are covered. Takes one run slot (host\locks.ps1) and never starts while build\play.lock exists.
# Exit code: 0 PASS, 1 a row failed, 2 the run was unusable.
[CmdletBinding(PositionalBinding=$false)]
param([string]$Group, [string]$Rows, [string[]]$Set = @(), [string]$Same = '', [string]$Route, [string]$Moment,
      [string]$BuildDir, [string]$Name, [int]$Settle = 16, [int]$Timeout = 590, [switch]$List)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# group -> rows (changed, then put back), launch settings, the moment. Keep a group under about 20 steps: every step holds the frozen frame for a second.
$groups = [ordered]@{
    atmos = @{ Rows = 'atmos_shafts=1|atmos_shafts=-|atmos_fog=1|atmos_fog=-|atmos_exposure=1|atmos_exposure=-|atmos_bloom=1|atmos_bloom=-|atmos_clouds=1|atmos_clouds=-|atmos_sky=1|atmos_sky=-'; Same = 'atmos_clouds' }
    post  = @{ Rows = 'gfx_pass_bloom=off|gfx_pass_bloom=-|gfx_pass_dof=off|gfx_pass_dof=-|gfx_pass_grain=off|gfx_pass_grain=-|gfx_pass_aberration=off|gfx_pass_aberration=-|gfx_sharpen=1.00|gfx_sharpen=-|gfx_smaa_quality=low|gfx_smaa_quality=-|look=vivid|look_intensity=0.30|look_intensity=-|look=-'; Same = 'gfx_pass_dof,gfx_pass_grain,gfx_pass_aberration,look,look_intensity' }   # the three passes draw nothing at this moment (log: gamemods: pass "..." switched off); the look is applied after the frame dump (log: look: SNAP_LOOK=...)
    light = @{ Rows = 'ssao_quality=ultra|ssao_quality=-|gfx_ssao_samples=256|gfx_ssao_samples=-|gfx_ssao_hbao=32,128|gfx_ssao_hbao=-|gfx_lighting=maximum|gfx_lighting=-|gfx_bounce=1|gfx_bounce=-|gfx_shadow_dist=8|gfx_shadow_dist=-'; Same = 'ssao_quality,gfx_ssao_samples,gfx_ssao_hbao,gfx_shadow_dist' }   # no visible change in this frozen frame
    tex   = @{ Rows = 'gfx_aniso=0|gfx_aniso=-|gfx_lod_bias=2|gfx_lod_bias=-|foliage_aa=1|foliage_aa=-|leaf_translucency=1|leaf_translucency=-|gfx_pom=high|gfx_pom=-' }
    view  = @{ Rows = 'gfx_lod_dist=8|gfx_lod_dist=-|gfx_lod_force0=1|gfx_lod_force0=-|gfx_draw_dist=8.00|gfx_draw_dist=-'; Same = 'gfx_draw_dist' }   # nothing beyond the default distance in this view
    # size rows (runtime/nvn/nvn_retarget.cpp): the render targets are made again mid-ride; no frame over the resize may take more than 100 ms
    size  = @{ Rows = 'gfx_render_scale=1.50|gfx_render_scale=-|gfx_render_scale=0.50|gfx_render_scale=-|gfx_aa=smaa|gfx_aa=none|gfx_aa=-|gfx_shadow_res=4096|gfx_shadow_res=-'; Same = 'gfx_shadow_res'; Resizes = 6 }
    # Ultrawide on a 21:9 window: the frame gets wider and narrower again, and the 16:9 picture is the middle of the wide one
    wide  = @{ Rows = 'gfx_ultrawide=off|gfx_ultrawide=-|gfx_ultrawide=off|gfx_ultrawide=auto'; Env = 'SNAP_WINDOW_W=3440;SNAP_WINDOW_H=1440;SNAP_WINDOW_MODE=windowed'; Resizes = 4 }
    # rows whose picture cannot move in a frozen frame: the check is the live flag plus the log line each one writes
    quiet = @{ Rows = 'gfx_motion_blur=2|gfx_motion_blur=-|gfx_bokeh_dof=1.6|gfx_bokeh_dof=-|gfx_full_rate_anim=1|gfx_full_rate_anim=-|fps_target=120|fps_target=-|gfx_hdr_expand=1.00|gfx_hdr_expand=-'; Same = 'gfx_motion_blur,gfx_bokeh_dof,gfx_full_rate_anim,fps_target,gfx_hdr_expand' }
}
if ($List) { $groups.Keys | ForEach-Object { Write-Host $_ }; exit 0 }
if ($Group) {
    if (-not $groups.Contains($Group)) { Write-Host "no group '$Group' (-List shows them)"; exit 2 }
    $g = $groups[$Group]
    if (-not $Rows) { $Rows = $g.Rows }
    if (-not $Set.Count -and $g.Set) { $Set = @($g.Set) }
    if (-not $Same -and $g.Same) { $Same = $g.Same }
    if (-not $Route -and $g.Route) { $Route = $g.Route }
    if (-not $Moment -and $g.Moment) { $Moment = $g.Moment }
}
if ($Group -and $groups[$Group].Env) { foreach ($kv in $groups[$Group].Env.Split(';')) { $n, $v = $kv.Split('=', 2); [Environment]::SetEnvironmentVariable($n, $v, 'Process') } }
if (-not $Rows) { Write-Host 'give -Group <name> or -Rows <key=value|key=value>'; exit 2 }
if (-not $Route) { $Route = 'core_florio_a' }
if (-not $Moment) { $Moment = 'ride40' }
if ((Test-Path (Join-Path $root 'build\play.lock')) -and $env:SNAP_RUN_WHILE_PLAYING -ne '1') { Write-Host 'build\play.lock is there (someone is playing): not starting'; exit 2 }

$name = if ($Name) { $Name } elseif ($Group) { $Group } else { 'rows' }   # -Name keeps two runs at once apart (log and verdict files)
$outDir = Join-Path $root 'analysis\live_opts'
$log = Join-Path $outDir "run_$name.out"
New-Item -ItemType Directory -Force $outDir | Out-Null
$a = @('-NoProfile', '-File', (Join-Path $root 'host\ab_capture.ps1'), '-Route', $Route, '-Moment', $Moment, '-Effect', 'opts', '-Values', "base|base|$Rows",
       '-NoControl', '-NoPhoto', '-KeepRun', '-Settle', "$Settle", '-Window', '4', '-Name', $name, '-OutDir', $outDir, '-Timeout', "$Timeout")
if ($BuildDir) { $a += @('-BuildDir', $BuildDir) }
if ($Set.Count) { $a += '-Set'; $a += $Set }
$ErrorActionPreference = 'Continue'   # a stderr line of the capture (its own compare step) is not this test's failure
& powershell @a 2>&1 | Tee-Object -FilePath $log | ForEach-Object { Write-Host "$_" }
$ErrorActionPreference = 'Stop'
# the capture's own compare step needs Pillow (not in the venv) and may stop before its "outputs" line: take the run id and the stamp from the run lines
$m = Select-String -Path $log -Pattern 'route__abcap_(\d+_\d+)_(\S+?)_\d+\.png' | Select-Object -Last 1
if (-not $m) { Write-Host "live opts: the capture run gave no frames (see $log)"; exit 2 }
$stamp = $m.Matches[0].Groups[1].Value; $runId = $m.Matches[0].Groups[2].Value
$work = Join-Path $root "build\w\abcap_run_$stamp"
$shots = Join-Path $root "analysis\boot\route__abcap_${stamp}_$runId"
$c = @('-I', (Join-Path $root 'tests\live\live_opts_check.py'), '--result', (Join-Path $work 'result.json'), '--shots', $shots)
if ($Same) { $c += @('--same', $Same) }
if ($Group -and $groups[$Group].Resizes) { $c += @('--log', (Join-Path $root "analysis\boot\runs\$runId\boot_stderr.log"), '--resizes', "$($groups[$Group].Resizes)") }
& (Join-Path $root 'tools\venv-recomp\Scripts\python.exe') @c 2>&1 | Tee-Object -FilePath (Join-Path $outDir "check_$name.txt") | ForEach-Object { Write-Host "$_" }
$code = $LASTEXITCODE
Write-Host "live opts: group $name, run $runId, frames $shots`_NN.png, verdicts $(Join-Path $outDir "check_$name.txt")"
exit $code
