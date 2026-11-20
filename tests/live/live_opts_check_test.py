# SPDX-License-Identifier: MIT
"""Unit test of tests/live/live_opts_check.py (no game run).

Usage: python -I tests/live/live_opts_check_test.py   (the venv python: needs numpy)
"""
import importlib.util
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np

spec = importlib.util.spec_from_file_location('live_opts_check', Path(__file__).with_name('live_opts_check.py'))
t = importlib.util.module_from_spec(spec)
spec.loader.exec_module(t)


def frame(v, noise_px=0):
    f = np.full((40, 60, 3), v, dtype=np.int16)
    if noise_px:
        f[0, :noise_px] += 9   # a little HUD animation
    return f


def png_bytes(img, filt):
    h, w, _ = img.shape
    rows, prev = [], np.zeros((w, 3), dtype=np.int16)
    for y in range(h):
        cur = img[y].astype(np.int16)
        left = np.vstack([np.zeros((1, 3), dtype=np.int16), cur[:-1]])
        ul = np.vstack([np.zeros((1, 3), dtype=np.int16), prev[:-1]])
        if filt == 0:
            enc = cur
        elif filt == 1:
            enc = cur - left
        elif filt == 2:
            enc = cur - prev
        elif filt == 3:
            enc = cur - ((left + prev) >> 1)
        else:
            pa, pb, pc = abs(prev - ul), abs(left - ul), abs(left + prev - 2 * ul)
            enc = cur - np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, prev, ul))
        rows.append(bytes([filt]) + (enc & 255).astype(np.uint8).tobytes())
        prev = cur

    def chunk(k, d):
        return struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d))
    return bytes.fromhex('89504e470d0a1a0a') + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')


def png_round_trip():
    rng = np.random.default_rng(7)
    img = rng.integers(0, 256, size=(9, 13, 3)).astype(np.int16)
    ok = True
    with tempfile.TemporaryDirectory() as d:
        for filt in range(5):
            p = os.path.join(d, 'f%d.png' % filt)
            with open(p, 'wb') as f:
                f.write(png_bytes(img, filt))
            ok = ok and np.array_equal(t.read_png(p), img)
    return ok


def main():
    def st(v, info=''):
        return {'value': v, 'info': info}
    states = [st('base'), st('base'), st('fog=1', 'live now=1'), st('fog=-', 'live now=default'), st('aa=smaa', 'restart now=smaa'), st('blur=1', 'live now=1')]
    frames = {0: frame(100), 1: frame(100, 2), 2: frame(120), 3: frame(100), 4: frame(140), 5: frame(140, 1)}
    noise, rows = t.verdicts(states, frames)
    by = {r[0]: r for r in rows}
    checks = [
        ('the noise floor comes from the control pair', noise is not None and 0 < noise[1] < 1),
        ('a live row that moved the picture passes', by['fog=1'][2]),
        ('going back to the default moves it again', by['fog=-'][2]),
        ('a restart row fails even when the picture moved', not by['aa=smaa'][2]),
        ('a live row that moved nothing fails', not by['blur=1'][2]),
        ('a row listed as same passes without a change', t.verdicts(states, frames, ('blur',))[1][5][2]),
        ('no control pair means no verdict', t.verdicts([st('base'), st('fog=1', 'live')], {0: frame(1), 1: frame(2)})[0] is None),
        ('the PNG reader decodes every filter type', png_round_trip()),
        ('a missing frame fails the row', not t.verdicts(states, {**frames, 2: None})[1][2][2]),
    ]
    fails = 0
    for label, ok in checks:
        print('%s %s' % ('ok  ' if ok else 'FAIL', label))
        fails += not ok
    print('live opts check test:', 'PASS' if not fails else '%d FAILED' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
