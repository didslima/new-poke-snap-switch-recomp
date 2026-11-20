# Pictures of every launcher page at 1920x1080 and 3840x2160, from the standalone preview exe (host/build_launcher_ui_test.py -> build\launcher_ui).
# The window is hidden; the picture is the finished frame, saved by the launcher itself (SNAP_LAUNCHER_UI_SHOT), so a stale screen grab cannot happen.
#   powershell -File scripts\ui\capture_launcher.ps1 -Out analysis\ui_polish\launcher_before
# Then a contact sheet per size: python -E -P scripts\ui\contact_sheet.py <dir> <size> <out.png>
param([string]$Out = 'analysis\ui_polish\launcher_before', [string[]]$Sizes = @('1920x1080', '3840x2160'))
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$exe = Join-Path $root 'build\launcher_ui\launcher_ui_test.exe'
if (-not (Test-Path $exe)) { throw "build it first: tools\venv-recomp\Scripts\python.exe -I host\build_launcher_ui_test.py" }
New-Item -ItemType Directory -Force $Out | Out-Null
$data = Join-Path $root 'build\ui_polish_data'
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $data | Out-Null
$pages = 'Display', 'Picture', 'Detail', 'Upscaling', 'Frame_rate', 'Sound', 'Controls', 'Gameplay', 'Accessibility'
$views = @(, @('home', 'home'))
for ($i = 0; $i -lt $pages.Count; $i++) { $views += , @("settings_$($pages[$i])", "settings:$i") }
$views += , @('pulldown_Picture_Look', 'pulldown:1:0')
$views += , @('pulldown_Display_Window', 'pulldown:0:1')
foreach ($x in @('saves', 'mods:0', 'steam', 'album')) { $views += , @(($x -replace ':0', ''), $x) }
$env:SNAP_LAUNCHER_NO_SHELL = '1'
foreach ($size in $Sizes) {
  $dpi = if ($size -eq '3840x2160') { '192' } else { '144' }   # a 4K screen is usually 200%, a 1080p one 150%
  foreach ($v in $views) {
    $bmp = Join-Path $Out "$($v[0])_$size.bmp"
    Remove-Item $bmp -ErrorAction SilentlyContinue
    $env:SNAP_LAUNCHER_UI_SHOT = $bmp
    $env:SNAP_LAUNCHER_UI_VIEW = $v[1]
    $env:SNAP_LAUNCHER_UI_SIZE = $size
    $env:SNAP_LAUNCHER_DPI = $dpi
    $p = Start-Process -FilePath $exe -ArgumentList "`"$data`"" -WindowStyle Hidden -PassThru
    $p.PriorityClass = 'BelowNormal'
    if (-not $p.WaitForExit(40000)) { taskkill /PID $p.Id /T /F | Out-Null; Write-Host "TIMEOUT $($v[0]) $size" }
  }
}
Remove-Item Env:\SNAP_LAUNCHER_UI_SHOT, Env:\SNAP_LAUNCHER_UI_VIEW, Env:\SNAP_LAUNCHER_UI_SIZE, Env:\SNAP_LAUNCHER_DPI
python -E -P -c "
import sys, glob, os
from PIL import Image
for f in glob.glob(os.path.join(sys.argv[1], '*.bmp')):
    Image.open(f).convert('RGB').save(f[:-4] + '.png'); os.remove(f)
" $Out
Get-ChildItem $Out -Filter *.png | Measure-Object | ForEach-Object { Write-Host "$($_.Count) pictures in $Out" }
