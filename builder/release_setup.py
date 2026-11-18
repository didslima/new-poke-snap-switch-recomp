"""Build SnapRecomp.exe, the one file a release ships (developer tool; run after release_prebuilt.py).

It is the setup wizard with the builder inside. On the player's PC it builds the game into its own folder on the first start and
starts that build on every later one (release standard 1: one exe; setup/setup_main.cpp HandOff). The builder zip is no longer a
release asset: release_package.py still makes it for developers and the CI gate.

  python -I builder/release_setup.py [--prebuilt build/release/prebuilt] [--out build/release] [--llvm tools/llvm/bin]
                                     [--winsdk <xwin folder>] [--offline-dir <folder with the pinned archives>] [--sign]

The exe is a small Win32 wizard (setup/setup_main.cpp) followed by a zip payload and an 16-byte trailer (size + "SNPSET01"):

  python/        the pinned python.org embeddable Python (no packages: the setup engine uses only the standard library)
  builder/ recomp/ scripts/ prebuilt/ licenses/ README.md LICENSE LICENSE-MIT   the same files as the builder zip (release_package.collect)
  offline/       the small pinned tools shipped inside (NSTool, MIT); every large tool is fetched on the player's PC with a SHA-256 check

Our code from v1.2.0 is GPL-3.0 with the section 7 additional permission in LICENSE. Releases up to v1.1.0 were MIT.
Third-party components retain their own licenses. No game data, keys, vendor upscaler binaries or Microsoft files are inside. The finished exe is leak scanned like the builder zip.
`--sign` writes SnapRecomp.exe.sig with the update key (release_sign.py)."""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'builder'))
import ci_gate  # noqa: E402
import release_package  # noqa: E402
from snapbuild import fetch, winsdk  # noqa: E402
from snapbuild import safedel  # noqa: E402

MAGIC = b'SNPSET01'


RELEASE_EXE = 'SnapRecomp.exe'   # the only file in a release download (plus its .sig)


def pinned_table():
    return json.loads((ROOT / 'builder' / 'versions.json').read_text(encoding='utf-8'))


def compile_stub(llvm_bin, out_exe, version, env):
    """clang++ + lld-link, static C++ runtime, Windows subsystem, UTF-8 manifest, no console."""
    src = ROOT / 'setup' / 'setup_main.cpp'
    rc = ROOT / 'setup' / 'setup.rc'
    obj_dir = out_exe.parent / 'setup_obj'
    obj_dir.mkdir(parents=True, exist_ok=True)
    clang = Path(llvm_bin) / 'clang++.exe'
    res = obj_dir / 'setup.res'
    r = subprocess.run([str(Path(llvm_bin) / 'llvm-rc.exe'), '/FO', str(res), str(rc)], env=env, cwd=str(ROOT / 'setup'), capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('llvm-rc failed:\n' + r.stdout + r.stderr)
    cmd = [str(clang), '-std=c++17', '-O2', '-municode', f'-DSETUP_VERSION="{version}"', '-ffile-prefix-map=' + str(ROOT) + '=.',
           '-fuse-ld=lld', str(src), str(res), '-o', str(out_exe), '-Wl,/SUBSYSTEM:WINDOWS', '-lcomctl32', '-lshell32', '-lole32', '-lcomdlg32', '-lgdiplus',
           '-luser32', '-lgdi32', '-ladvapi32', '-luuid', '-Wl,/DEBUG:NONE']
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('compiling the setup wizard failed:\n' + (r.stdout + r.stderr)[-4000:])


def build_payload(prebuilt, zp, python_dir, offline_files, version):
    items = release_package.collect(prebuilt)
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p, name in items:
            z.write(p, name)
        for p in sorted(Path(python_dir).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:
                z.write(p, 'python/' + p.relative_to(python_dir).as_posix())
        for p in offline_files:
            z.write(p, 'offline/' + Path(p).name)
        z.writestr('payload.json', json.dumps({'version': version, 'files': len(items)}))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--prebuilt', default=str(ROOT / 'build' / 'release' / 'prebuilt'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'release'))
    ap.add_argument('--llvm', default=str(ROOT / 'tools' / 'llvm' / 'bin'))
    ap.add_argument('--winsdk', help='xwin folder (crt/ and sdk/); default: the managed one, else the INCLUDE and LIB already set')
    ap.add_argument('--offline-dir', action='append', default=[], help='folder with the pinned archives (python_embed, nstool); else downloaded and hash checked')
    ap.add_argument('--romfs', help='romfs folder for the game-file-name check of the leak scan (read only)')
    ap.add_argument('--sign', action='store_true')
    ap.add_argument('--stub-only', action='store_true', help='compile the wizard exe without a payload (UI tests)')
    ci_gate.add_args(ap)
    a = ap.parse_args()
    ci_gate.gate(a, 'release_setup')
    version = (ROOT / 'builder' / 'VERSION').read_text(encoding='utf-8').strip()
    out = Path(a.out).resolve()   # llvm-rc runs in setup/, so a relative --out must not stay relative
    a.prebuilt = str(Path(a.prebuilt).resolve())
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    root = Path(a.winsdk) if a.winsdk else winsdk.find()
    if root:
        env = winsdk.env_for(root)
    env['PATH'] = str(a.llvm) + os.pathsep + env.get('PATH', '')
    stub = out / 'setup_stub.exe'
    compile_stub(a.llvm, stub, version, env)
    print(f'wizard compiled: {stub} {stub.stat().st_size / 1e3:.0f} KB')
    final = out / RELEASE_EXE
    if a.stub_only:
        shutil.copyfile(stub, final)
        return
    table = pinned_table()
    work = Path(tempfile.mkdtemp(prefix='snapsetup_', dir=str(out)))
    try:
        dl = work / 'dl'
        dl.mkdir()
        ask = lambda *_: True   # noqa: E731 (the packager runs on the developer PC; the pins carry the hashes)
        py_arc = fetch.fetch_pin('python_embed', table['tools']['python_embed'], dl, ask, a.offline_dir, None, print)
        ns_arc = fetch.fetch_pin('nstool', table['tools']['nstool'], dl, ask, a.offline_dir, None, print)
        pydir = work / 'python'
        fetch.extract(py_arc, pydir)
        zp = work / 'payload.zip'
        build_payload(a.prebuilt, zp, pydir, [ns_arc], version)
        scanned = out / 'SnapRecomp.payload.zip'   # the payload alone, so the leak scan can read it (it reads zips, not PE files)
        # python/ and offline/ are the python.org and NSTool archives, checked by SHA-256 against versions.json (fetch_pin above);
        # their third-party binaries are not ours to scan, so the scan covers everything else in the payload
        with zipfile.ZipFile(zp) as zin, zipfile.ZipFile(scanned, 'w', zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                if not info.filename.startswith(('python/', 'offline/')):
                    zout.writestr(info, zin.read(info.filename))
        payload = zp.read_bytes()
        tmp = final.with_name(final.name + '.part')
        with open(tmp, 'wb') as f:
            f.write(stub.read_bytes())
            f.write(payload)
            f.write(struct.pack('<Q', len(payload)) + MAGIC)
        tmp.replace(final)
    finally:
        safedel.rmtree(work, ignore_errors=True)
    h = hashlib.sha256(final.read_bytes()).hexdigest()
    print(f'{final} {final.stat().st_size / 1e6:.1f} MB sha256 {h}')
    cmd = [sys.executable, '-I', str(ROOT / 'builder' / 'leak_scan.py'), str(scanned), '--strip-prefix', 'prebuilt/',
           '--manifest', str(Path(a.prebuilt) / 'manifest.json')]
    if a.romfs:
        cmd += ['--romfs', a.romfs]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        final.unlink()
        raise SystemExit(f'leak scan failed; {RELEASE_EXE} was deleted')
    print('leak scan of the payload clean')
    if a.sign:
        r = subprocess.run([sys.executable, '-I', str(ROOT / 'builder' / 'release_sign.py'), '--zip', str(final), '--version', version])
        if r.returncode != 0:
            raise SystemExit('signing failed')


if __name__ == '__main__':
    main()
