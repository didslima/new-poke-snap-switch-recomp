"""Player command-line flags on the built exe (docs/command-line.md, host/cli_flags.h).

Runs the built exe as the launcher (config.json next to it, SNAP_LAUNCHER_AUTOPLAY=1, scratch data folders): no window opens and no game
starts, because the converted game files named in config.json do not exist. Checks --help, --version, a mistyped flag, that the launcher
still runs when player flags are on the command line, that --set with a SNAP_* name reaches the environment, and that --data-dir moves the
data folders.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/cli_flags_exe_test.py [path\\to\\snap_boot.exe] [--rebuild]

An exe older than the sources this test is about is refused (exit 3) instead of failing check after check: an old build\\snap\\snap_boot.exe
once reported 27 failures that were only its age. --rebuild runs host/build_snap.py into the exe's folder first.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARGS = [a for a in sys.argv[1:] if a != '--rebuild']
EXE = Path(ARGS[0]) if ARGS else ROOT / 'build' / 'snap' / 'snap_boot.exe'
# What the checks below exercise. An exe built before any of these changed cannot pass, whatever the code says now.
SOURCES = ('host/cli_flags.h', 'host/launcher.cpp', 'host/launcher_upscalers.cpp', 'host/boot.cpp', 'host/portable.cpp')


def stale_against(exe, sources, root=ROOT):
    """The first source file newer than the exe (its path as given), or None when the exe is current."""
    built = Path(exe).stat().st_mtime
    for s in sources:
        f = Path(root) / s
        if f.is_file() and f.stat().st_mtime > built:
            return s
    return None


def dead_pid():
    p = subprocess.Popen(['cmd', '/c', 'exit', '0'], creationflags=0x08000000)
    p.wait()
    return p.pid


def main():
    if '--rebuild' in sys.argv[1:]:
        r = subprocess.run([sys.executable, '-I', str(ROOT / 'host' / 'build_snap.py'), '--out', str(EXE.parent), '--name', EXE.stem])
        if r.returncode != 0:
            print('the rebuild failed (exit code %d)' % r.returncode)
            return 2
    if not EXE.exists():
        print('exe not found:', EXE)
        return 2
    newer = stale_against(EXE, SOURCES)
    if newer:
        print('%s is older than %s, so it cannot pass. Build it again (add --rebuild, or run host/build_snap.py --out %s), or name a current exe.'
              % (EXE, newer, EXE.parent))
        return 3
    work = Path(tempfile.mkdtemp(prefix='snap_cli_'))
    fails = 0
    try:
        game = work / 'game'
        game.mkdir()
        shutil.copy2(EXE, game / 'SnapRecomp.exe')
        for dll in EXE.parent.glob('*.dll'):
            shutil.copy2(dll, game / dll.name)
        (work / 'romfs').mkdir()
        (work / 'none.elf').write_bytes(b'')
        cfg = {'main_elf': str(work / 'none.elf'), 'sdk_elf': str(work / 'none.elf'), 'romfs_root': str(work / 'romfs'),
               'data_root': str(work / 'data'), 'upscalers_offered': True, 'builder_version': '9.8.7'}
        (game / 'config.json').write_text(json.dumps(cfg))
        logs = work / 'local' / 'SnapRecomp' / 'logs'

        def run(args=(), extra_env=None, autoplay=True):
            env = dict(os.environ, SNAP_CONFIG_DIR=str(work / 'cfg'), SNAP_PROFILES_DIR=str(work / 'profiles'), LOCALAPPDATA=str(work / 'local'),
                       APPDATA=str(work / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1')
            if not autoplay:
                env.pop('SNAP_LAUNCHER_AUTOPLAY')
            for k in ('SNAP_SAFE_MODE', 'SNAP_DATA_DIR', 'SNAP_PORTABLE', 'SNAP_REAL_APPDATA', 'SNAP_REAL_LOCALAPPDATA'):
                env.pop(k, None)
            env.update(extra_env or {})
            r = subprocess.run([str(game / 'SnapRecomp.exe'), *args], cwd=game, env=env, capture_output=True, text=True, timeout=60, creationflags=0x08000000)
            return r.returncode, r.stderr + r.stdout

        def check(label, ok, got=''):
            nonlocal fails
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     got: %s' % str(got).strip()[:400])

        code, out = run(['--help'])
        check('--help: exit 0, lists the flags', code == 0 and all(w in out for w in ('--play', '--fullscreen', '--set', '--data-dir', '--profile')), (code, out))
        code, out = run(['--version'])
        check('--version: the version in config.json', code == 0 and 'Snap Recomp 9.8.7' in out, (code, out))
        code, out = run(['--fullscren'])
        check('a mistyped flag: exit 2 and it is named', code == 2 and '--fullscren' in out and '--help' in out, (code, out))
        code, out = run(['--width'])
        check('a flag with no value: exit 2', code == 2 and '--width' in out, (code, out))

        # the launcher still runs with player flags present: it gets as far as the game-folder check, which fails on the empty test files
        code, base = run()
        check('no flags: the launcher checks the game folder', 'launcher:' in base, (code, base))
        flags = ['--play', '--windowed', '--width', '960', '--height', '540', '--monitor', '0', '--profile', 'Test', '--set', 'fps_target=60']
        code2, out = run(flags)
        check('player flags: the launcher still runs, same result', 'launcher:' in out and code2 == code, (code2, out))

        # --set with a SNAP_* name is an environment override: safe mode already asked for, so a dead start is not offered
        logs.mkdir(parents=True, exist_ok=True)
        (logs / 'launch.marker').write_text('pid=%d\r\nstart=1\r\nsafe=0\r\n' % dead_pid())
        code, out = run()
        check('planted dead start: safe mode offered', 'safe mode offered' in out, (code, out))
        code, out = run(['--set', 'SNAP_SAFE_MODE=1'])
        check('--set SNAP_SAFE_MODE=1: no offer', 'safe mode offered' not in out and 'launcher:' in out, (code, out))

        # --play as another launcher uses it (no test switch): no window of ours may open, so every stop is a line on stderr and an
        # exit code. A message box would hang this run until the timeout. Planted: a first start (no "upscalers_offered"), a dead
        # start (the safe-mode question) and converted files that cannot be read.
        first = dict(cfg)
        del first['upscalers_offered']
        (game / 'config.json').write_text(json.dumps(first))
        (logs / 'launch.marker').write_text('pid=%d\r\nstart=1\r\nsafe=0\r\n' % dead_pid())
        try:
            code, out = run(['--play'], autoplay=False)
            hung = False
        except subprocess.TimeoutExpired:
            code, out, hung = -1, 'timed out: a window is waiting for an answer', True
        check('--play without the test switch: exits by itself (no window waits)', not hung, out)
        check('--play: the safe-mode question is a stderr line and the start goes on', 'because of --play' in out and '--safe-mode' in out, (code, out))
        check('--play: unreadable converted files give exit 1 or 2 and the reason', code in (1, 2) and 'launcher:' in out, (code, out))
        check('--play: the upscaler offer is kept for a start with the window', 'upscalers_offered' not in (game / 'config.json').read_text(), (game / 'config.json').read_text())
        (game / 'config.json').write_text(json.dumps(cfg))
        (logs / 'launch.marker').unlink(missing_ok=True)

        # --data-dir: the data folders move, so the planted marker under LOCALAPPDATA is not seen and the new folders appear
        dd = work / 'my data'
        code, out = run(['--data-dir', str(dd)])
        check('--data-dir: data folders created there', (dd / 'Local').is_dir() and (dd / 'Roaming').is_dir(), (code, out, list(dd.glob('*')) if dd.exists() else 'missing'))
        check('--data-dir: the old data folder is not read', 'safe mode offered' not in out and 'launcher:' in out, (code, out))
        (dd / 'Local' / 'SnapRecomp' / 'logs').mkdir(parents=True, exist_ok=True)
        (dd / 'Local' / 'SnapRecomp' / 'logs' / 'launch.marker').write_text('pid=%d\r\nstart=1\r\nsafe=0\r\n' % dead_pid())
        code, out = run(['--data-dir', str(dd)])
        check('--data-dir: its own logs folder is read', 'safe mode offered' in out, (code, out))

        # ---- launcher actions (--update, --check-install, --report, --restore-backup, --upscalers, --hdtex): no window, the game never starts
        code, out = run(['--help'])
        check('--help lists the launcher actions', all(w in out for w in ('--update', '--check-install', '--report', '--restore-backup', '--upscalers', '--hdtex')), out)
        code, out = run(['--update'])
        check('--update without a builder: exit 1 and says why', code == 1 and 'builder folder is missing' in out, (code, out))
        fake = work / 'fake builder'
        fake.mkdir()
        (fake / 'snap_builder.py').write_text('')
        (fake / 'python.exe').write_bytes(b'')
        (game / 'config.json').write_text(json.dumps(dict(cfg, python=str(fake / 'python.exe'), builder_dir=str(fake), work=str(work / 'the work'), update_channel='testing')))
        code, out = run(['--update'])
        check('--update: the updater command (channel, work folder, --yes, waits for this process)',
              code == 0 and all(w in out for w in ('would run:', 'snap_builder.py', 'update apply', '--channel testing', 'the work', '--yes', '--wait-pid')), (code, out))
        (game / 'config.json').write_text(json.dumps(cfg))
        code, out = run(['--check-install'])
        check('--check-install: prints a result, exit 0 or 1, and no launcher start', code in (0, 1) and len(out.strip()) > 20 and 'launcher: game folder' not in out, (code, out))
        rep = work / 'reports'
        code, out = run(['--report'], {'SNAP_REPORT_DIR': str(rep)})
        zips = list(rep.glob('*.zip')) if rep.exists() else []
        check('--report: one zip, its path printed, nothing else started', code == 0 and len(zips) == 1 and str(zips[0]) in out and 'Nothing was uploaded' in out, (code, out, zips))
        # ---- --restore-backup, --upscalers, --hdtex: no window even outside a test run (autoplay=False); a dialog would hang until the timeout
        code, out = run(['--restore-backup'], autoplay=False)
        check('--restore-backup: prints the (empty) list, no window', code == 0 and 'no backups' in out and 'launcher: game folder' not in out, (code, out))
        code, out = run(['--restore-backup', 'latest'], autoplay=False)
        check('--restore-backup latest with no backups: exit 1 and says so', code == 1 and 'no backups' in out, (code, out))
        code, out = run(['--restore-backup', 'newest'], autoplay=False)
        check('--restore-backup with a wrong argument: exit 2', code == 2 and 'latest or a number' in out, (code, out))
        # a save with one backup: listed as number 1, and "latest" puts it back
        code, out = run(['--upscalers'], autoplay=False)
        check('--upscalers: prints the list, no window', code == 0 and ('lists no upscalers' in out or 'license:' in out) and 'launcher: game folder' not in out, (code, out))
        code, out = run(['--upscalers', 'nope'], autoplay=False)
        check('--upscalers with an unknown name: exit 2', code == 2 and 'no upscaler named' in out, (code, out))
        code, out = run(['--upscalers', '--accept-license'], autoplay=False)
        check('--upscalers --accept-license without a name: exit 2', code == 2, (code, out))
        code, out = run(['--hdtex'], autoplay=False)
        check('--hdtex without a builder: exit 1 and says why, no window', code == 1 and 'builder folder is missing' in out, (code, out))
        (game / 'config.json').write_text(json.dumps(dict(cfg, python=str(fake / 'python.exe'), builder_dir=str(fake))))
        code, out = run(['--hdtex'])
        check('--hdtex: the HD tool with --status (named in a test run)', code == 0 and 'would run:' in out and 'hdtex --status' in out, (code, out))
        code, out = run(['--hdtex', '--level', 'hd', '--set', 'creature.scale=4', '--work', 'a b'])
        check('--hdtex <options>: passed on word for word', code == 0 and 'hdtex --level hd --set creature.scale=4 --work "a b"' in out, (code, out))
        (game / 'config.json').write_text(json.dumps(cfg))
        code, out = run(['--update', '--report'])
        check('two actions: exit 2', code == 2 and '--report' in out, (code, out))
        code, out = run(['--play', '--hdtex'])
        check('an action with --play: exit 2', code == 2 and '--play' in out, (code, out))
        bare = work / 'bare'
        bare.mkdir()
        shutil.copy2(EXE, bare / 'SnapRecomp.exe')
        for dll in EXE.parent.glob('*.dll'):
            shutil.copy2(dll, bare / dll.name)
        r = subprocess.run([str(bare / 'SnapRecomp.exe'), '--check-install'], cwd=bare, capture_output=True, text=True, timeout=60, creationflags=0x08000000,
                           env=dict(os.environ, LOCALAPPDATA=str(work / 'local'), APPDATA=str(work / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1'))
        check('an action without a finished build: exit 2 and says so', r.returncode == 2 and 'finished build' in r.stderr + r.stdout, (r.returncode, r.stderr + r.stdout))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('cli flags exe test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
