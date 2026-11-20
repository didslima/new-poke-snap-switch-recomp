# SPDX-License-Identifier: MIT
# Post-processing for host\ab_capture.ps1 (docs/ab-capture.md). Run with tools\venv-recomp\Scripts\python -I (needs Pillow and numpy).
#   ab_compare.py --result <abcap result.json> --shots <analysis\boot\route_<name>_<runId>> --passes <passes.csv> --out-prefix <analysis\<effect>\ab_<moment>>
# Writes <out-prefix>.png (the states side by side, then the most-changed region magnified), <out-prefix>.json (per-pass GPU ms per state,
# image metrics, photo-frame hashes, verdicts) and prints one `PHOTO-FRAME CHECK: PASS|FAIL|INCONCLUSIVE ...` line.
import argparse, csv, hashlib, json, os, re, sys
from collections import defaultdict
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('--result', required=True)
ap.add_argument('--shots', required=True, help='prefix of the converted dumps: <prefix>_NN.png and <prefix>_dumps.txt')
ap.add_argument('--passes', default='')
ap.add_argument('--out-prefix', required=True)
ap.add_argument('--route', default='')
ap.add_argument('--run', default='')
ap.add_argument('--notes', default='')
a = ap.parse_args()

R = json.load(open(a.result))
states = R['states']; dumps = R['dumps']

# ---- match requests to files by order and present number (the file name carries the present it was taken at)
names = []
dl = a.shots + '_dumps.txt'
if os.path.exists(dl):
    for i, ln in enumerate(l.strip() for l in open(dl) if l.strip()):
        m = re.search(r'_p(\d+)_', ln)
        names.append((i, int(m.group(1)) if m else -1))
used = set()
for d in dumps:
    d['file'] = None
    for i, p in names:
        if i in used or p < d['request_present'] - 1: continue
        used.add(i); d['file'] = '%s_%02d.png' % (a.shots, i); d['present'] = p; break

def load(d):
    return np.asarray(Image.open(d['file']).convert('RGB'), dtype=np.int16) if d.get('file') and os.path.exists(d['file']) else None

scene = {}
for d in dumps:
    if d['kind'] == 'scene' and d['file']: scene[d['state']] = d
photo = defaultdict(list)
for d in dumps:
    if d['kind'] == 'photo' and d['file']: photo[d['state']].append(d)

out = {'effect': R['effect'], 'moment': R['moment'], 'route': a.route, 'run': a.run, 'status': R['status'], 'notes': a.notes,
       'freeze_present': R['freeze_present'], 'settle_presents': R['settle'], 'window_presents': R['window'], 'states': []}

# ---- per-pass GPU ms per state (rows of SNAP_GPU_PASSES: present,slot,idx,ms,draws,label)
def norm(lb):
    lb = re.sub(r' t[0-9a-f]{1,3}$', '', lb)   # the render-target pointer hash differs per run
    return lb
rows = defaultdict(lambda: defaultdict(float))   # present -> label -> ms
if a.passes and os.path.exists(a.passes):
    with open(a.passes, newline='') as f:
        for r in csv.DictReader(f):
            try: rows[int(r['present'])][norm(r['label'])] += float(r['ms'])
            except Exception: pass
pass_unit = 'ms per frame, mean over the timing window'
for i, s in enumerate(states):
    dp = s.get('dev_passes') or {}
    if dp:   # named pass times from the dev loop (nvn_dev.h): 1 s averages of the held frame
        e = {'i': i, 'value': s['value'], 'source': 'nvn_dev', 'gpu_ms_frame_mean': round(s['dev_gpu_busy_ms'], 4) if s.get('dev_gpu_busy_ms', -1) >= 0 else None,
             'passes': {k: round(v, 4) for k, v in sorted(dp.items(), key=lambda kv: -kv[1])}}
        e['gpu_ms_frame_median'] = e['gpu_ms_frame_mean']
        out['states'].append(e); continue
    ps = [p for p in range(s['win_from'], s['win_to']) if p in rows]
    agg = defaultdict(float); tot = []
    for p in ps:
        for k, v in rows[p].items(): agg[k] += v / max(1, len(ps))
        tot.append(sum(rows[p].values()))
    e = {'i': i, 'value': s['value'], 'source': 'SNAP_GPU_PASSES', 'frames_timed': len(ps), 'gpu_ms_frame_mean': round(float(np.mean(tot)), 4) if tot else None,
         'gpu_ms_frame_median': round(float(np.median(tot)), 4) if tot else None, 'passes': {k: round(v, 4) for k, v in sorted(agg.items(), key=lambda kv: -kv[1])}}
    out['states'].append(e)

# ---- images: freeze check (control vs first), effect size, most-changed region
imgs = {i: load(d) for i, d in scene.items()}
vals = [s['value'] for s in states]
nctl = len(vals) - 1 if len(vals) >= 3 and vals[-1] == vals[0] else None   # the plan's last state repeats the first: noise floor
def stat(x, y):
    if x is None or y is None or x.shape != y.shape: return None
    dif = np.abs(x - y).max(axis=2)
    return {'mean_abs': round(float(np.abs(x - y).mean()), 4), 'changed_px_pct_gt2': round(float((dif > 2).mean() * 100), 4), 'max': int(dif.max())}
if nctl is not None and 0 in imgs and nctl in imgs:
    out['control_vs_first'] = stat(imgs[0], imgs[nctl])
    cf = out['control_vs_first']
    # the HUD's lock-on ring and similar UI animate by present count, so a frozen scene still moves about 0.5% of the pixels; a running scene moves far more
    out['time_frozen'] = bool(cf and cf['changed_px_pct_gt2'] < 2.0)
if 0 in imgs and 1 in imgs:
    out['first_vs_second'] = stat(imgs[0], imgs[1])
    # most-changed region: box sum of the diff over a (W/6 x H/6) window via an integral image
    H, W, _ = imgs[0].shape
    dif = np.clip(np.abs(imgs[0] - imgs[1]).max(axis=2) - 2, 0, None).astype(np.float64)
    bw, bh = max(64, W // 6), max(36, H // 6)
    ii = np.pad(dif.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    box = ii[bh:, bw:] - ii[:-bh, bw:] - ii[bh:, :-bw] + ii[:-bh, :-bw]
    yy, xx = np.unravel_index(int(np.argmax(box)), box.shape)
    out['zoom_region'] = {'x': int(xx), 'y': int(yy), 'w': bw, 'h': bh, 'changed_mean_in_region': round(float(dif[yy:yy + bh, xx:xx + bw].mean()), 3)}
    # image: row 1 the states (not the control), row 2 the magnified region
    show = [i for i in sorted(imgs) if i != nctl][:3]
    cw = 1280; ch = int(round(cw * H / W)); zw = cw; zh = int(round(zw * bh / bw))
    try: fnt = ImageFont.truetype('arial.ttf', 28)
    except Exception: fnt = ImageFont.load_default()
    sheet = Image.new('RGB', (cw * len(show), ch + zh), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    for c, i in enumerate(show):
        full = Image.fromarray(imgs[i].astype(np.uint8))
        sheet.paste(full.resize((cw, ch), Image.LANCZOS), (c * cw, 0))
        crop = full.crop((xx, yy, xx + bw, yy + bh)).resize((zw, zh), Image.NEAREST if bw * 4 < zw else Image.LANCZOS)
        sheet.paste(crop, (c * cw, ch))
        lab = '%s = %s' % (R['effect'], vals[i])
        dr.rectangle((c * cw + 8, 8, c * cw + 24 + 16 * len(lab), 46), fill=(0, 0, 0)); dr.text((c * cw + 16, 10), lab, fill=(255, 255, 255), font=fnt)
        dr.rectangle((c * cw, ch - 2, c * cw + cw, ch + 1), fill=(24, 24, 24))
    # the zoom box on the top-row frames
    for c, i in enumerate(show):
        sx, sy = cw / W, ch / H
        dr.rectangle((c * cw + xx * sx, yy * sy, c * cw + (xx + bw) * sx, (yy + bh) * sy), outline=(255, 220, 0), width=3)
    os.makedirs(os.path.dirname(a.out_prefix) or '.', exist_ok=True)
    sheet.save(a.out_prefix + '.png')
    out['image'] = os.path.basename(a.out_prefix) + '.png'
else:
    out['image'] = None

# ---- GPU ms deltas (second state minus first)
if len(out['states']) >= 2 and out['states'][0]['passes'] and out['states'][1]['passes']:
    A, B = out['states'][0]['passes'], out['states'][1]['passes']
    d = {k: round(B.get(k, 0) - A.get(k, 0), 4) for k in set(A) | set(B)}
    top = sorted(d.items(), key=lambda kv: -abs(kv[1]))[:8]
    out['gpu_delta_second_minus_first'] = {'frame_ms': round((out['states'][1]['gpu_ms_frame_mean'] or 0) - (out['states'][0]['gpu_ms_frame_mean'] or 0), 4), 'top_passes': dict(top)}

# ---- photo-frame check: hash of the window texture on the presents the Fx plumbing marks as photo frames
def sha(d):
    x = Image.open(d['file']).convert('RGB')
    return hashlib.sha256(x.tobytes()).hexdigest()
ph = {}
for i in range(len(vals)):
    first = [d for d in photo.get(i, []) if d['photo_flag'] == 1]
    ph[i] = {'value': vals[i], 'photo_frames': len(first), 'present': first[0]['present'] if first else None, 'sha256': sha(first[0]) if first else None,
             'all_hashes': [sha(d) for d in first]}
out['photo_frames'] = [ph[i] for i in sorted(ph)]
hs = [ph[i]['sha256'] for i in sorted(ph)]
pruns = R.get('photo_runs', [])
out['photo_runs'] = pruns
if any(h is None for h in hs):
    # No identical-scene photo frames (the game's photo flow needs game time: the shutter did nothing while time was frozen). Fall back to the gating
    # counters of the shutter run with time running: effect passes that drew on presents marked as photo frames must be 0, while the effect did draw
    # on ordinary frames (otherwise the effect was not on and the check proves nothing).
    seen = [p for p in pruns if p['photo_presents'] > 0 or p.get('photo_passes', 0) > 0]   # held presents, or the photo pass itself (a ride photo holds nothing)
    bad = [p for p in seen if p['effect_draws_on_photo_frames'] > 0]
    active = [p for p in pruns if p['effect_draws'] > 0]
    if not seen and any(p.get('film_empty') for p in pruns): verdict = 'INCONCLUSIVE'; why = 'no film left at this moment (the route used it up): pick an earlier -Moment'
    elif not seen: verdict = 'INCONCLUSIVE'; why = 'no photo pass appeared, with time frozen or running (shutter not accepted at this moment)'
    elif bad: verdict = 'FAIL'; why = 'effect passes drew on photo frames for ' + ','.join('%s (%d draws)' % (p['value'], p['effect_draws_on_photo_frames']) for p in bad)
    elif not active: verdict = 'INCONCLUSIVE'; why = 'photo passes seen (%d) with no effect draw: this effect has no FxNoteRan counter, or it was off for every value' % sum(max(p['photo_presents'], p.get('photo_passes', 0)) for p in seen)
    else: verdict = 'PASS'; why = 'gating: %d photo passes, 0 effect draws on the %d held presents (effect drew %d times on the other frames; a ride photo holds none, the Evaluation passes do); scenes differ with time running, so the image hash was not compared' % (sum(p.get('photo_passes', 0) for p in seen), sum(p['photo_presents'] for p in seen), sum(p['effect_draws'] for p in active))
elif len(set(hs)) == 1:
    verdict = 'PASS'; why = 'identical photo frame for %s (sha256 %s)' % ('/'.join(vals), hs[0][:12])
else:
    # same value twice differing means noise (not the effect): inconclusive; otherwise a real difference
    same_val_diff = any(vals[i] == vals[j] and hs[i] != hs[j] for i in range(len(vals)) for j in range(i + 1, len(vals)))
    verdict = 'INCONCLUSIVE' if same_val_diff else 'FAIL'
    why = ('the photo frame differs even for the same value (' if same_val_diff else 'the photo frame differs between values (') + ', '.join('%s %s' % (ph[i]['value'], (hs[i] or 'none')[:12]) for i in sorted(ph)) + ')'
    if not same_val_diff and 0 in photo and 1 in photo and photo[0] and photo[1]:
        x, y = load(photo[0][0]), load(photo[1][0])
        out['photo_diff'] = stat(x, y)
out['photo_check'] = {'verdict': verdict, 'detail': why}
json.dump(out, open(a.out_prefix + '.json', 'w'), indent=1)
print('PHOTO-FRAME CHECK: %s (%s)' % (verdict, why))
if 'time_frozen' in out: print('FREEZE CHECK: %s (control vs first: %s)' % ('PASS' if out['time_frozen'] else 'FAIL', out['control_vs_first']))
if 'first_vs_second' in out: print('A/B: %s vs %s: %s; zoom region %s' % (vals[0], vals[1], out['first_vs_second'], out.get('zoom_region')))
if 'gpu_delta_second_minus_first' in out: print('GPU: %s' % out['gpu_delta_second_minus_first'])
sys.exit({'PASS': 0, 'FAIL': 10, 'INCONCLUSIVE': 11}[verdict])
