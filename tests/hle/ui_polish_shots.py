"""Picks the F1 overlay pictures of the UI polish walk (tests/hle/ui_polish_f1_script.txt) out of an NVN_FRAME_DUMP folder (BMP names f<N>_p<present>_t<seconds>.bmp)
and writes <out>/f1_<name>_<size>.png. Stop k of the script is at 27+3k seconds, its picture the dump frame nearest 28.5+3k.
   python -E -P tests/hle/ui_polish_shots.py <dump dir> <out dir> <WxH>"""
import re, sys
from pathlib import Path
from PIL import Image

NAMES = ['bindings', 'mouse', 'controller', 'accessibility', 'audio', 'graphics_top', 'graphics_a', 'graphics_b', 'graphics_bottom',
         'display_top', 'display_bottom', 'photos', 'mods', 'saves', 'save_state', 'cheats', 'benchmark', 'test_input']

src, out, size = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
out.mkdir(parents=True, exist_ok=True)
frames = []
for f in src.glob('f*_p*_t*.bmp'):
    m = re.match(r'f\d+_p\d+_t(\d+(?:\.\d+)?)', f.name)
    if m: frames.append((float(m.group(1)), f))
frames.sort()
if not frames:
    sys.exit('no frames in %s' % src)
for k, name in enumerate(NAMES):
    want = 28.5 + 3 * k
    t, f = min(frames, key=lambda p: abs(p[0] - want))
    if abs(t - want) > 1.6:
        print('no frame near %.1f s for %s (nearest %.1f)' % (want, name, t)); continue
    Image.open(f).convert('RGB').save(out / ('f1_%s_%s.png' % (name, size)))
    print(name, f.name)
