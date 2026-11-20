"""Contact sheet: every <name>_<size>.png of a folder in one picture, scaled to a fixed cell width, names under each cell.
   python -E -P scripts/ui/contact_sheet.py <dir> <size, e.g. 1920x1080> <out.png> [cols=3] [cell_w=640]
   Add a second folder with --pair <dir2> to put before (left) and after (right) of the same name side by side."""
import sys, glob, os
from PIL import Image, ImageDraw, ImageFont


def load(d, size):
    out = {}
    for f in sorted(glob.glob(os.path.join(d, '*_%s.png' % size))):
        out[os.path.basename(f)[:-len('_%s.png' % size)]] = Image.open(f).convert('RGB')
    return out


def main():
    a = sys.argv[1:]
    pair = None
    if '--pair' in a:
        i = a.index('--pair'); pair = a[i + 1]; del a[i:i + 2]
    d, size, out = a[0], a[1], a[2]
    cols = int(a[3]) if len(a) > 3 else (1 if pair else 3)
    cw = int(a[4]) if len(a) > 4 else (640 if not pair else 820)
    first = load(d, size)
    second = load(pair, size) if pair else None
    names = list(first.keys())
    if second:
        names = [n for n in names if n in second]
    try:
        font = ImageFont.truetype('arial.ttf', 22)
    except Exception:
        font = ImageFont.load_default()
    sample = next(iter(first.values()))
    ch = int(cw * sample.height / sample.width)
    lab = 34
    per = 2 if second else 1
    rows = (len(names) + cols - 1) // cols
    cell_w = cw * per + 8 * (per - 1)
    sheet = Image.new('RGB', (cols * (cell_w + 12) + 12, rows * (ch + lab + 12) + 12), (30, 30, 34))
    dr = ImageDraw.Draw(sheet)
    for i, n in enumerate(names):
        x = 12 + (i % cols) * (cell_w + 12)
        y = 12 + (i // cols) * (ch + lab + 12)
        for k, src in enumerate([first] + ([second] if second else [])):
            sheet.paste(src[n].resize((cw, ch), Image.LANCZOS), (x + k * (cw + 8), y + lab))
        dr.text((x, y + 4), n + ('   before | after' if second else ''), fill=(235, 235, 235), font=font)
    sheet.save(out)
    print('wrote', out, sheet.size, len(names), 'pages')


main()
