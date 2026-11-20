# SPDX-License-Identifier: MIT
"""Checks one `ab_capture.ps1 -Effect opts` run: every options row that was changed mid-ride changed the picture with no restart.

The run freezes game time, then walks a list of states. A state is `<key>=<value>` (the options row is set the way the menu sets it;
`-` = the row's default) or a bare word (nothing is changed: a control frame). One full frame is dumped per state.

For every `<key>=<value>` state:
  * the row must be a live row (abcap records "live" / "restart" from the options table) and the set must have worked;
  * its frame must differ from the frame of the state before it by more than the noise floor. The noise floor is the largest difference
    between a control frame and the frame before it (the HUD animates a little even with game time frozen).

Usage: python -I tests/live/live_opts_check.py --result <result.json> --shots <analysis\\boot\\route_<name>_<runId>> [--same key,key]
  --same   rows whose picture is not expected to change at this moment (checked for "live" only; reported, not failed)
Exit code 0 = every row passed, 1 = a row failed, 2 = the run is unusable (no frames, no control).
"""
import argparse
import json
import os
import re
import struct
import sys
import zlib

import numpy as np


def read_png(path):
    """8-bit RGB / RGBA PNG (what run_boot.ps1 writes from a frame dump) as an int16 H x W x 3 array. No Pillow needed."""
    b = open(path, 'rb').read()
    if b[:8] != bytes.fromhex('89504e470d0a1a0a'):
        raise ValueError('not a PNG: ' + path)
    pos, idat, w = 8, [], 0
    while pos < len(b):
        n, kind = struct.unpack('>I4s', b[pos:pos + 8])
        body = b[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, lace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or lace:
                raise ValueError('unsupported PNG (depth %d, color type %d, interlace %d): %s' % (depth, ctype, lace, path))
            ch = 3 if ctype == 2 else 4
        elif kind == b'IDAT':
            idat.append(body)
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), dtype=np.uint8).reshape(h, 1 + w * ch)
    out = np.zeros((h, w * ch), dtype=np.uint8)
    prev = np.zeros(w * ch, dtype=np.uint8)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:]
        if f == 0:
            cur = line.copy()
        elif f == 2:
            cur = line + prev
        elif f == 1:   # Sub: a running sum per channel
            cur = np.cumsum(line.reshape(w, ch), axis=0, dtype=np.uint8).reshape(-1)
        else:          # Average / Paeth: byte by byte (rare in these files)
            cur = np.zeros(w * ch, dtype=np.uint8)
            pl, cl = prev.tolist(), [0] * (w * ch)
            for i, x in enumerate(line.tolist()):
                a = cl[i - ch] if i >= ch else 0
                up = pl[i]
                if f == 3:
                    p = (a + up) >> 1
                else:
                    c = pl[i - ch] if i >= ch else 0
                    pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                    p = a if pa <= pb and pa <= pc else up if pb <= pc else c
                cl[i] = (x + p) & 255
            cur[:] = cl
        out[y] = cur
        prev = cur
    return out.reshape(h, w, ch)[:, :, :3].astype(np.int16)


def match_dumps(result, shots):
    """Frame file per state: requests and files are matched by order and present number (the file name carries its present)."""
    names = []
    listing = shots + '_dumps.txt'
    if os.path.exists(listing):
        for i, ln in enumerate(l.strip() for l in open(listing) if l.strip()):
            m = re.search(r'_p(\d+)_', ln)
            names.append((i, int(m.group(1)) if m else -1))
    used, files = set(), {}
    for d in result['dumps']:
        if d['kind'] != 'scene':
            continue
        for i, p in names:
            if i in used or p < d['request_present'] - 1:
                continue
            used.add(i)
            files[d['state']] = '%s_%02d.png' % (shots, i)
            break
    return files


def diff(a, b):
    """(mean absolute difference, percent of pixels that moved by more than 2 of 255), or None when the frames cannot be compared."""
    if a is None or b is None or a.shape != b.shape:
        return None
    d = np.abs(a - b)
    return float(d.mean()), float((d.max(axis=2) > 2).mean() * 100)


def resized(a, b):
    """Two frames of different sizes (a size row made the render targets again): mean absolute difference with the larger one sampled down to the
    smaller one's grid, or None when either frame is missing or the sizes are equal. A small value means the same picture at a new size."""
    if a is None or b is None or a.shape == b.shape:
        return None
    if a.shape[0] * a.shape[1] < b.shape[0] * b.shape[1]:
        a, b = b, a
    # the Ultrawide row changes the shape: the 16:9 picture is the middle of the wider one (same height of view), so compare that middle
    wa, wb = a.shape[1] * b.shape[0], b.shape[1] * a.shape[0]
    if wa > wb * 1.02:
        w = a.shape[0] * b.shape[1] // b.shape[0]; x0 = (a.shape[1] - w) // 2; a = a[:, x0:x0 + w]
    elif wb > wa * 1.02:
        w = b.shape[0] * a.shape[1] // a.shape[0]; x0 = (b.shape[1] - w) // 2; b = b[:, x0:x0 + w]
    ys =(np.arange(b.shape[0]) * a.shape[0] // b.shape[0]).astype(np.int64)
    xs = (np.arange(b.shape[1]) * a.shape[1] // b.shape[1]).astype(np.int64)
    return float(np.abs(a[ys][:, xs] - b).mean())


def hitches(log_path):
    """The longest frame the game measured over each live resize (runtime/nvn/nvn_retarget.cpp log lines), in ms."""
    out = []
    try:
        for line in open(log_path, errors='replace'):
            if '[live]' in line and 'longest frame over the resize' in line:
                out.append(float(line.rsplit(':', 1)[1].split()[0]))
    except OSError:
        pass
    return out


def verdicts(states, frames, same=()):
    """One row per state: (value, kind, ok, text). `frames` maps state index -> image array (or None)."""
    pairs = {i: diff(frames.get(i), frames.get(i - 1)) for i in range(1, len(states))}
    sized = {i: resized(frames.get(i), frames.get(i - 1)) for i in range(1, len(states))}
    controls = [pairs[i] for i in range(1, len(states)) if '=' not in states[i]['value'] and pairs[i]]
    if not controls:
        return None, []
    noise = (max(c[0] for c in controls), max(c[1] for c in controls))
    need = (noise[0] * 3 + 0.01, noise[1] * 3 + 0.05)
    rows = []
    for i, s in enumerate(states):
        v = s['value']
        if '=' not in v:
            rows.append((v, 'control', True, 'control' + ('' if i == 0 or not pairs[i] else ' (moved %.4f, %.3f%% of pixels)' % pairs[i])))
            continue
        key = v.split('=', 1)[0]
        info = s.get('info', '')
        if not info.startswith('live') or 'set-failed' in info:
            rows.append((v, 'row', False, 'the row is not live or the set failed (%s)' % info))
            continue
        p = pairs.get(i)
        if p is None and sized.get(i) is not None:   # the frame has a new size: the row applied; the picture must still be the same scene
            a, b = frames[i - 1], frames[i]
            text = 'frame size %dx%d -> %dx%d, same picture within %.2f of 255 (limit 12)' % (a.shape[1], a.shape[0], b.shape[1], b.shape[0], sized[i])
            rows.append((v, 'row', sized[i] < 12.0 and float(b.mean()) > 2.0, text))
            continue
        if p is None:
            rows.append((v, 'row', False, 'no frame to compare'))
            continue
        moved = p[0] > need[0] or p[1] > need[1]
        text = 'moved %.4f, %.3f%% of pixels (needs more than %.4f or %.3f%%)' % (p[0], p[1], need[0], need[1])
        if key in same:
            rows.append((v, 'same', True, 'live; picture not expected to change here: ' + text))
        else:
            rows.append((v, 'row', moved, text))
    return noise, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--result', required=True)
    ap.add_argument('--shots', required=True)
    ap.add_argument('--same', default='')
    ap.add_argument('--log', default='', help="the run's stderr log: every live resize must stay under --hitch-ms")
    ap.add_argument('--hitch-ms', type=float, default=100.0)
    ap.add_argument('--resizes', type=int, default=0, help='least number of live resizes the log must show')
    a = ap.parse_args()
    result = json.load(open(a.result))
    if result.get('status') != 'ok':
        print('the run did not finish (status %s)' % result.get('status'))
        return 2
    files = match_dumps(result, a.shots)
    frames = {i: (read_png(f) if os.path.exists(f) else None) for i, f in files.items()}
    noise, rows = verdicts(result['states'], frames, tuple(k for k in a.same.split(',') if k))
    if noise is None:
        print('no control frame pair: the noise floor is unknown')
        return 2
    print('noise floor: %.4f mean, %.3f%% of pixels' % noise)
    fails = 0
    for value, kind, ok, text in rows:
        if kind == 'control':
            print('     %-34s %s' % (value, text))
            continue
        print('%s %-34s %s' % ('ok  ' if ok else 'FAIL', value, text))
        fails += not ok
    if a.log:
        hs = hitches(a.log)
        bad = [h for h in hs if h > a.hitch_ms]
        ok = not bad and len(hs) >= a.resizes
        print('%s live resizes: %d (needs %d), longest frame over one %.1f ms (limit %.0f ms)' % ('ok  ' if ok else 'FAIL', len(hs), a.resizes, max(hs) if hs else 0.0, a.hitch_ms))
        fails += not ok
    print('live options check:', 'PASS' if not fails else '%d FAILED' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
