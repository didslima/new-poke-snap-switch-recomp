"""Incremental per-translation-unit Linux/ELF build of generated AOT C into static archives (lib*.a).

Usage: python3 -I recomp/snap_build_linux.py --gen <dir> --out <dir> --lib <snap_main|snap_sdk>
                                             [--jobs N] [--opt -O2] [--cflags ...] [--cc clang] [--ar llvm-ar]

Reads <gen>/manifest.json to determine the exact C translation units (chunks + <table>.c + plt_stubs.c +
unresolved.c), compiles them to x86-64 ELF objects (<out>/obj/*.o), and archives <out>/lib/lib<lib>.a.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = Path(os.environ.get('SNAP_RUNTIME_INCLUDE') or ROOT / 'runtime')
TU_TIMEOUT = 30 * 60


def find_tool(preferred, fallbacks):
    for cand in (preferred, *fallbacks):
        if not cand:
            continue
        p = shutil.which(cand)
        if p:
            return p
    return None


def collect_sources(gen_dir):
    manifest_path = gen_dir / 'manifest.json'
    if not manifest_path.is_file():
        raise FileNotFoundError(f'missing manifest.json in {gen_dir}')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    module = manifest.get('module')
    if module not in ('main', 'sdk'):
        raise ValueError(f'unsupported module {module!r} in {manifest_path}')

    names = list(manifest.get('chunks', []))
    names.append(f'{module}_table.c')
    if module == 'main' or (gen_dir / 'plt_stubs.c').is_file():
        names.append('plt_stubs.c')
    names.append('unresolved.c')

    srcs = []
    for name in names:
        p = gen_dir / name
        if not p.is_file():
            raise FileNotFoundError(f'missing generated source {p}')
        srcs.append(p)
    return module, srcs


def compile_one(cc, src, obj, flags):
    t0 = time.perf_counter()
    try:
        r = subprocess.run([cc] + flags + ['-c', str(src), '-o', str(obj)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=TU_TIMEOUT)
        rc = r.returncode
        log = r.stdout[-1500:] if r.stdout else ''
    except subprocess.TimeoutExpired:
        rc = 124
        log = f'timeout after {TU_TIMEOUT}s'
    return {
        'src': src.name,
        'seconds': time.perf_counter() - t0,
        'rc': rc,
        'log': log,
        'obj_bytes': obj.stat().st_size if obj.exists() else 0,
        'src_bytes': src.stat().st_size,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, required=True, help='generated AOT C directory containing manifest.json')
    ap.add_argument('--out', type=Path, required=True, help='output directory for obj/ and lib/')
    ap.add_argument('--lib', required=True, help='archive base name (produces lib<name>.a)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    ap.add_argument('--opt', default='-O2')
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    ap.add_argument('--cc', default='clang')
    ap.add_argument('--ar', default='llvm-ar')
    ap.add_argument('--no-lib', action='store_true')
    args = ap.parse_args()

    cc = find_tool(args.cc, ('clang', 'gcc'))
    ar = find_tool(args.ar, ('llvm-ar', 'ar'))
    if not cc:
        print('snap_build_linux: C compiler not found', file=sys.stderr)
        return 1
    if not ar:
        print('snap_build_linux: archiver (llvm-ar/ar) not found', file=sys.stderr)
        return 1

    try:
        module, srcs = collect_sources(args.gen)
    except (OSError, ValueError) as exc:
        print(f'snap_build_linux: {exc}', file=sys.stderr)
        return 1

    obj_dir, lib_dir = args.out / 'obj', args.out / 'lib'
    obj_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)

    flags = [args.opt, '-fno-strict-aliasing', '-w', '-I', str(RUNTIME)] + args.cflags.split()
    header_hashes = ''.join(hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(RUNTIME.glob('*.h')))
    stamp = hashlib.sha256((cc + ' ' + ' '.join(flags) + header_hashes).encode()).hexdigest()
    stamp_f = args.out / 'flags.stamp'
    stale_all = not stamp_f.exists() or stamp_f.read_text() != stamp

    todo = []
    objs = []
    for s in srcs:
        o = obj_dir / (s.stem + '.o')
        objs.append(o)
        if stale_all or not o.exists() or o.stat().st_mtime < s.stat().st_mtime:
            todo.append((s, o))

    print(f'{module}: {len(srcs)} TUs, {len(todo)} to build '
          f'({"flags/runtime header changed" if stale_all else "incremental"}), {args.jobs} jobs', flush=True)

    t0 = time.perf_counter()
    results, failed = [], []
    todo.sort(key=lambda t: -t[0].stat().st_size)
    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [ex.submit(compile_one, cc, s, o, flags) for s, o in todo]
        for i, f in enumerate(cf.as_completed(futs)):
            r = f.result()
            results.append(r)
            if r['rc'] != 0:
                failed.append(r)
                print(f'FAILED {r["src"]} rc={r["rc"]}\n{r["log"]}', file=sys.stderr, flush=True)
            if (i + 1) % 20 == 0 or i + 1 == len(futs):
                print(f'compiled {i + 1}/{len(futs)} TUs, {time.perf_counter() - t0:.1f}s', flush=True)

    wall = time.perf_counter() - t0
    if failed:
        return 1

    stamp_f.write_text(stamp)
    lib_name = args.lib[3:] if args.lib.startswith('lib') else args.lib
    lib = lib_dir / f'lib{lib_name}.a'
    t1 = time.perf_counter()
    if todo or not lib.exists():
        if not args.no_lib:
            lib.unlink(missing_ok=True)
            subprocess.check_call([ar, 'rcs', str(lib)] + [str(o) for o in objs])
    t_ar = time.perf_counter() - t1

    rep = {
        'module': module,
        'tus_total': len(srcs),
        'tus_built': len(todo),
        'wall_s': round(wall, 2),
        'ar_s': round(t_ar, 2),
        'flags': flags,
        'lib': str(lib),
        'lib_bytes': lib.stat().st_size if lib.exists() else 0,
    }
    (args.out / 'build_report.json').write_text(json.dumps(rep, indent=1), encoding='utf-8')
    print(json.dumps(rep, indent=1), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
