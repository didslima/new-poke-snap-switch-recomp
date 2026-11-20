"""The engine behind the setup half of SnapRecomp.exe (setup/setup_main.cpp): everything the one-click setup does, without any window.

The setup program (a small Windows program with the wizard pages) starts this with the builder's own Python:
    snap_builder.py setup scan   --folder <game folder> [--keys <prod.keys>]     -> one JSON object on stdout (what was found, what is missing)
    snap_builder.py setup run    --config <json>                                 -> one JSON object per line on stdout (progress events)
    snap_builder.py setup repair                                                  -> run again with the saved choices
    snap_builder.py setup uninstall [--remove-tools] [--remove-saves]            -> one JSON object per line
All questions are asked by the wizard before `run` starts, so `run` never prompts: a download happens only when the config says the
person agreed (`downloads_ok`), Microsoft's SDK files only when `accept_ms_license` is true, and an upscaler only when its license is
listed in `accept_license`.

Resumable: every stage checks what is already there (tools marker files, the extracted game, the build stamps), so running it again
after a crash, a reboot or a closed window continues where it stopped.

Our code from v1.2.0 is GPL-3.0 with the section 7 additional permission in the bundled LICENSE.
Releases up to v1.1.0 were MIT; third-party components retain their own licenses.

Nothing here downloads, requests or points to game files, keys or firmware. Keys are read in place by NSTool and never copied or logged.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import fetch, gamesrc, paths, pipeline, pyenv, toolchain, versioncheck as vc, winsdk
from .util import Log, StepError
from . import safedel

GB = 1 << 30
MB = 1 << 20

# Typical seconds per stage on a mid-range PC with a warm download cache; used only to weigh the one progress bar and to make the
# time-left guess. The guess is then scaled by how fast this PC actually went through the finished weight.
BUILD_STEPS = {'elf': 10, 'maps': 800, 'gen_main': 70, 'gen_sdk': 50, 'compile_main': 200, 'compile_sdk': 120, 'link': 25, 'smoke': 45,
               'assemble': 15}
DOWNLOAD_MBPS = 20.0   # for weighing downloads before they start

STAGE_TITLES = {
    'game': 'Reading your game files',
    'tools': 'Getting the build tools',
    'winsdk': 'Getting the Microsoft C runtime files',
    'build': 'Building Snap Recomp from your game files',
    'finish': 'Finishing up',
}


class Cancelled(Exception):
    pass


# ------------------------------------------------------------------------------------------------ events and progress

class Events:
    """JSON-line events. Printed to stdout (the wizard reads them) and appended to setup-events.log."""

    def __init__(self, out=None, log_path=None):
        self.out = out or sys.stdout
        self.f = open(log_path, 'a', encoding='utf-8', buffering=1) if log_path else None

    def emit(self, **ev):
        ev['ts'] = round(time.time(), 1)
        line = json.dumps(ev, ensure_ascii=False)
        try:
            self.out.write(line + '\n')
            self.out.flush()
        except (OSError, ValueError):
            pass
        if self.f:
            self.f.write(line + '\n')

    def close(self):
        if self.f:
            self.f.close()


class Progress:
    """One bar over all stages. `weights` = {stage: expected seconds}; `set(stage, fraction)` moves it; the time left is the unfinished
    weight scaled by the real speed so far."""

    def __init__(self, ev, weights):
        self.ev = ev
        self.w = dict(weights)
        self.frac = {k: 0.0 for k in self.w}
        self.t0 = time.time()
        self.stage = None
        self.last_emit = 0.0
        self.text = ''

    def total(self):
        return sum(self.w.values()) or 1.0

    def done_weight(self):
        return sum(self.w[k] * self.frac[k] for k in self.w)

    def pct(self):
        return 100.0 * self.done_weight() / self.total()

    def eta(self):
        done = self.done_weight()
        if done < max(20.0, 0.03 * self.total()):
            return None
        speed = (time.time() - self.t0) / done
        return int(max(0.0, (self.total() - done) * min(max(speed, 0.25), 4.0)))

    def set(self, stage, frac, text=None, force=False):
        if stage in self.frac:
            self.frac[stage] = max(self.frac[stage], min(1.0, frac)) if frac < 1.0 else 1.0
        if text is not None:
            self.text = text
        now = time.time()
        if force or now - self.last_emit >= 0.4:
            self.last_emit = now
            self.ev.emit(t='progress', pct=round(self.pct(), 1), eta_s=self.eta(), stage=stage, text=self.text)

    def finish_stage(self, stage, text=None):
        self.set(stage, 1.0, text, force=True)


class EventLog(Log):
    """The builder's Log, turned into events: every line goes to the log file and the wizard's details box; the pipeline's
    "step X" lines move the build part of the bar."""

    def __init__(self, path, ev, prog):
        super().__init__(path, quiet=True)
        self.ev = ev
        self.prog = prog
        self.step_t0 = {}

    def write(self, text, echo=True):
        super().write(text, echo=False)
        for line in str(text).splitlines():
            self.ev.emit(t='log', text=line)
            self._watch(line)

    def _watch(self, line):
        # "step maps" / "step maps done in 12s" / "step maps: up to date, skipped" / "step maps: restored from the cache"
        if not line.startswith('step '):
            return
        rest = line[5:]
        name = rest.split(':')[0].split(' ')[0]
        key = {'nso2elf': 'elf', 'elf': 'elf'}.get(name, name)
        if key not in BUILD_STEPS:
            return
        label = {'elf': 'Preparing the game code', 'maps': 'Finding the game\'s functions (this is the long step)',
                 'gen_main': 'Translating the game code (1 of 2)', 'gen_sdk': 'Translating the game code (2 of 2)',
                 'compile_main': 'Compiling (1 of 2)', 'compile_sdk': 'Compiling (2 of 2)', 'link': 'Linking SnapRecomp.exe',
                 'smoke': 'Starting the game once to check it'}.get(key, key)
        if ': up to date' in rest or ': restored from the cache' in rest or ' done in ' in rest:
            self.prog.set('build:' + key, 1.0, label + ' (done)', force=True)
        else:
            self.prog.set('build:' + key, 0.05, label, force=True)
            self.ev.emit(t='substage', id=key, text=label)


# ------------------------------------------------------------------------------------------------ paths and settings

def setup_dir():
    return paths.setup_dir()   # <the exe's folder>\setup for an install; %LOCALAPPDATA%\SnapRecomp\setup for a source checkout


def default_root():
    return paths.base_dir()    # the exe's folder: work\, game\, tools\ and cache\ all sit in it (snapbuild/paths.py)


def load_settings():
    """settings.json, with its paths moved along when the install folder was moved since it was written (snapbuild/paths.py)."""
    s = gamesrc.load_settings(setup_dir() / 'settings.json')
    root = paths.install_root()
    if root is not None and s.get('install_root') and paths.rebase(s, s['install_root'], root):
        s['install_root'] = str(root)
        try:
            save_settings(s)
        except OSError:
            pass
    return s


def save_settings(s):
    root = paths.install_root()
    if root is not None:
        s['install_root'] = str(root)
    gamesrc.save_settings(s, setup_dir() / 'settings.json')


def fmt_gb(n):
    return f'{n / GB:.1f} GB'


# ------------------------------------------------------------------------------------------------ the wizard's detection and space screens

def drive_of(path):
    p = Path(path).resolve()
    return p.drive or str(p.anchor)


def space_report(cfg, kind, plan_extra=None, table=None):
    """Plain numbers per drive for the space screen. Items: (what, bytes, where). 'needs' is the peak, 'stays' is what is left afterwards."""
    table = table or vc.load_table(pipeline.BUILDER / 'versions.json')
    tools_dir = Path(cfg.get('tools_dir') or fetch.default_tools_dir())
    root = Path(cfg.get('root') or default_root())
    pins = dict(fetch.list_pins(table))
    tool_keys = ['llvm', 'ghidra', 'switch_loader', 'jdk', 'xwin', 'python_embed']
    need_tools = 0
    tools_stays = 0
    # unpacked sizes are larger than the archives (LLVM 0.9 GB archive -> about 3.6 GB unpacked, Ghidra 0.6 -> 1.1, Java 0.2 -> 0.45)
    factor = {'llvm': 4.0, 'ghidra': 2.0, 'jdk': 2.2}
    for k in tool_keys:
        pin = pins.get(k)
        if pin and not tool_marker(tools_dir, k, pin).exists():
            need_tools += int(pin.get('size') or 0)
            tools_stays += int(int(pin.get('size') or 0) * factor.get(k, 2.0))
    items = []
    if kind == 'dump':
        est = gamesrc.space_estimate(None, False)
        items.append({'what': 'Game data extracted from your files (stays: the game reads it while it runs)', 'stays': est['stays'],
                      'peak': est['peak'], 'where': str(root)})
    items.append({'what': 'Build tools, downloaded once (LLVM, Ghidra, Java, Microsoft C runtime)', 'stays': tools_stays,
                  'peak': need_tools + tools_stays, 'where': str(tools_dir)})
    if not winsdk.find(tools_dir) and not winsdk.vs_available():
        items.append({'what': 'Microsoft C runtime and Windows SDK files', 'stays': int(0.65 * GB), 'peak': int(1.8 * GB), 'where': str(tools_dir)})
    items.append({'what': 'The finished game program and its build files', 'stays': int(3 * GB), 'peak': int(6 * GB), 'where': str(root)})
    per_drive = {}
    for it in items:
        d = drive_of(it['where'])
        a = per_drive.setdefault(d, {'peak': 0, 'stays': 0, 'free': 0, 'extra': 0})
        a['stays'] += it['stays']
        a['extra'] = max(a['extra'], it['peak'] - it['stays'])   # the stages run one after another, so temporary copies do not add up
    for d, a in per_drive.items():
        a['peak'] = a.pop('extra') + a['stays']
        try:
            a['free'] = shutil.disk_usage(d + '\\').free
        except OSError:
            a['free'] = 0
        a['ok'] = a['free'] >= a['peak'] + GB
    note = ('The extracted game data stays on disk because the game reads it while it runs, so it cannot be deleted after the build.'
            if kind == 'dump' else '')
    return {'items': items, 'drives': per_drive, 'note': note,
            'ok': all(a['ok'] for a in per_drive.values())}


def scan_text(out):
    """The detection screen as plain lines (the wizard prints them as they are). Never contains key contents, only the keys file's path."""
    L = []
    kind = out.get('kind')
    if out.get('error') and kind not in ('dump', 'extracted'):
        return str(out['error'])
    if kind == 'extracted':
        s = out.get('summary') or {}
        L.append('Found extracted game files:')
        for k in ('main', 'sdk', 'romfs'):
            L.append(f'   {k}: {(s.get("extracted") or {}).get(k, "")}')
        L.append('Version check: ' + ('these are the 2.0.1 files, ready to build.' if out.get('ok') else 'not accepted.'))
    elif kind == 'dump':
        L.append(f'Found {len(out.get("dumps") or [])} game file(s):')
        for d in out.get('dumps') or []:
            vers = ', '.join(t['version_name'] for t in d['titles']) or (d.get('error') or 'could not read')
            L.append(f'   {d["name"]}  ({d["size"] / GB:.2f} GB)  {vers}')
        if out.get('plan'):
            p = out['plan']
            L.append(f'Using the {p["version_name"]} update ({Path(p["patch"]["file"]).name}) with {Path(p["base"]["file"]).name}.')
            L.append('Older updates in this folder are ignored.')
        if out.get('keys'):
            L.append(f'Keys: found at {out["keys"]} (read from there, never copied).')
    if out.get('error'):
        L.append('')
        L.append(str(out['error']))
    sp = out.get('space')
    if sp:
        L.append('')
        L.append('Disk space:')
        for it in sp['items']:
            L.append(f'   {it["what"]}: about {fmt_gb(it["stays"])} kept ({it["where"]})')
        for d, a in sp['drives'].items():
            L.append(f'   Drive {d} needs up to {fmt_gb(a["peak"])} while building and {fmt_gb(a["stays"])} afterwards; {fmt_gb(a["free"])} free'
                     + ('' if a['ok'] else '  <-- NOT ENOUGH'))
        if sp.get('note'):
            L.append(sp['note'])
    return '\n'.join(L)


def scan_log(lines):
    """Append what a scan found to setup-events.log (the file a problem report carries). File names, key names and the file reader's
    filtered complaint only: no key values, no folder paths. Never fails the scan."""
    try:
        d = setup_dir()
        d.mkdir(parents=True, exist_ok=True)
        with open(d / 'setup-events.log', 'a', encoding='utf-8') as f:
            for ln in lines:
                f.write(json.dumps({'t': 'log', 'text': str(ln), 'at': time.strftime('%Y-%m-%d %H:%M:%S')}, ensure_ascii=False) + '\n')
    except OSError:
        pass


def scan_folder(folder, keys=None, nstool_exe=None, table=None, cfg=None):
    out = _scan_folder(folder, keys, nstool_exe, table, cfg)
    out['text'] = scan_text(out)
    return out


def _scan_folder(folder, keys=None, nstool_exe=None, table=None, cfg=None):
    """What the wizard shows after "Choose your game files". Never raises for a normal bad folder: returns {'ok': False, 'error': text}."""
    out = {'ok': False, 'folder': str(folder), 'kind': None, 'error': None, 'notes': []}
    try:
        sc = gamesrc.scan(folder)
    except StepError as e:
        out['error'] = str(e)
        return out
    out['summary'] = sc.summary()
    out['notes'] = list(sc.notes)
    out['kind'] = sc.kind
    if keys:
        sc.keys = Path(keys)
    out['keys'] = str(sc.keys) if sc.keys else None
    out['emulator_keys'] = [str(p) for p in sc.emulator_keys]
    if sc.kind == 'extracted':
        table = table or vc.load_table(pipeline.BUILDER / 'versions.json')
        r = vc.check_all(sc.extracted['main'], sc.extracted['sdk'], sc.extracted['romfs'], table)
        out['check'] = {'ok': r.ok, 'problems': list(r.problems), 'notes': list(r.notes)}
        out['ok'] = r.ok
        if not r.ok:
            out['error'] = ' '.join(r.problems)
        out['version'] = vc.EXPECTED_NAME if r.ok else None
        out['space'] = space_report(cfg or {}, 'extracted', table=table)
        return out
    if sc.kind == 'dump':
        # listed before any early return below, so the screen's count always matches the log's "scan: N game file(s)"
        out['dumps'] = [{'name': d.path.name, 'size': d.size, 'titles': [], 'error': 'not read yet', 'error_code': None, 'reason': None}
                        for d in sc.dumps]
        if not sc.keys:
            out['error'] = ('This folder has game files but no prod.keys file next to them. Put your own prod.keys in the same folder '
                            '(the file must be named prod.keys; Windows may hide a ".txt" ending), or choose it below. '
                            'If you have an emulator installed, its keys file may already be on this PC.')
            out['needs_keys'] = True
            out['error_code'] = 'keys_missing'
            scan_log([f'scan {sc.folder.name}: no keys file found'])
            return out
        if not nstool_exe or not Path(nstool_exe).exists():
            out['error'] = 'the file reader (NSTool) is not available yet.'
            return out
        lines = [f'scan: {len(sc.dumps)} game file(s)']
        kinfo = gamesrc.inspect_keys(sc.keys)
        if kinfo.problem:       # a file that cannot be a working keys file: say so before any game file is blamed
            lines.append(kinfo.summary() if kinfo.names else f'Keys file "{kinfo.path.name}": {kinfo.problem}')
            scan_log(lines)
            out['error'] = kinfo.problem[0].upper() + kinfo.problem[1:]
            out['error_code'] = kinfo.code
            out['needs_keys'] = True
            return out
        ns = gamesrc.NsTool(nstool_exe, sc.keys)
        scratch = setup_dir() / 'scratch_ident'
        try:
            gamesrc.identify_all(sc, ns, scratch, say=lines.append, keys=kinfo)
        finally:
            safedel.rmtree(scratch, ignore_errors=True)
        out['dumps'] = [{'name': d.path.name, 'size': d.size, 'titles': [{'title': t['title_id'], 'version': t['version'],
                                                                         'version_name': gamesrc.version_name(t['version'])} for t in d.titles],
                         'error': d.error, 'error_code': d.code, 'reason': d.reason} for d in sc.dumps]
        try:
            plan = gamesrc.pick(sc)
        except StepError as e:
            out['error'] = str(e)
            out['error_code'] = getattr(e, 'code', 'step')
            # a real keys problem brings back "Choose prod.keys..." so another file can be picked without moving files around
            out['needs_keys'] = out['error_code'] in gamesrc.KEY_CODES
            lines.append(f'scan result ({out["error_code"]}): {out["error"]}')
            scan_log(lines)
            return out
        scan_log(lines)
        out['plan'] = plan.brief()
        out['plan']['version_name'] = gamesrc.version_name(plan.patch[1]['version'])
        out['ok'] = True
        out['space'] = space_report(cfg or {}, 'dump', table=table)
        return out
    out['error'] = ' '.join(sc.notes) or 'No game files found in this folder.'
    return out


# ------------------------------------------------------------------------------------------------ tools

def tool_marker(tools_dir, key, pin):
    return Path(tools_dir) / '.installed' / f'{key}-{pin["sha256"][:12]}'


def pending_tools(table, tools_dir, keys):
    pins = dict(fetch.list_pins(table))
    return [k for k in keys if not tool_marker(tools_dir, k, pins[k]).exists()]


def ensure_tool(key, pin, tools_dir, offline_dirs, downloads_ok, on_bytes, say, opener=None):
    """Verify, download if allowed, unpack and mark one pinned tool. The archive is deleted after unpacking (it can be 0.9 GB)."""
    tools_dir = Path(tools_dir)
    dl = tools_dir / 'downloads'
    have = fetch.find_local(pin, [dl] + list(offline_dirs))
    if not have:
        if not downloads_ok:
            raise StepError(f'{key} has to be downloaded and the download was not allowed. Run the setup again and allow the downloads.')
        say(f'Downloading {pin["filename"]} ({(pin.get("size") or 0) / MB:.0f} MB)')
        have = fetch.download(pin['url'], dl / pin['filename'], pin['sha256'], opener=opener, progress=on_bytes, size_hint=pin.get('size'), say=say)
    fetch.install_pin(key, pin, have, tools_dir, say)
    m = tool_marker(tools_dir, key, pin)
    m.parent.mkdir(parents=True, exist_ok=True)
    m.write_text(time.strftime('%Y-%m-%d %H:%M:%S'), encoding='ascii')
    try:
        if Path(have).parent == dl:
            Path(have).unlink()
    except OSError:
        pass


def ensure_tools(cfg, table, keys, prog, stage, say, opener=None):
    tools_dir = Path(cfg['tools_dir'])
    pins = dict(fetch.list_pins(table))
    todo = pending_tools(table, tools_dir, keys)
    total = sum(int(pins[k].get('size') or 1) for k in todo) or 1
    done = [0]
    for k in todo:
        pin = pins[k]
        base = done[0]
        size = int(pin.get('size') or 1)

        def on_bytes(n, k=k, base=base, size=size):
            prog.set(stage, (base + n * 0.85) / total, f'Downloading {k}: {n / MB:.0f} of {size / MB:.0f} MB')
        prog.set(stage, base / total, f'Getting {k}')
        ensure_tool(k, pin, tools_dir, cfg.get('offline_dirs') or [], cfg.get('downloads_ok'), on_bytes, say, opener)
        done[0] += size
        prog.set(stage, done[0] / total, f'{k} ready')
    return todo


def needs_winsdk(mode, tools_dir):
    """True when the Microsoft C runtime files still have to be fetched (no Visual Studio to use, none fetched before)."""
    if mode == 'vs' or winsdk.find(tools_dir):
        return False
    return not (mode == 'auto' and winsdk.vs_available())


def ensure_python(cfg, table, say):
    tools_dir = Path(cfg['tools_dir'])
    py = tools_dir / 'python' / 'python.exe'
    if py.exists() and pyenv.import_ok(py):
        return py
    dl = tools_dir / 'downloads'
    safedel.rmtree(tools_dir / 'python', ignore_errors=True)
    return pyenv.build_embedded(table, tools_dir / 'python', dl, dl, lambda q: bool(cfg.get('downloads_ok')) or _all_local(cfg, table),
                                cfg.get('offline_dirs') or [], say=say)


def _all_local(cfg, table):
    """Without downloads_ok, pyenv may still run when the payload carries the python and the wheels."""
    dirs = [Path(cfg['tools_dir']) / 'downloads'] + [Path(d) for d in cfg.get('offline_dirs') or []]
    pins = [p for k, p in fetch.list_pins(table) if k == 'python_embed' or k.startswith('wheel:')]
    return all(fetch.find_local(p, dirs) for p in pins)


# ------------------------------------------------------------------------------------------------ shortcuts, registry, uninstall

UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\SnapRecomp'


def _ps(script):
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, creationflags=0x08000000)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def _q(s):
    return "'" + str(s).replace("'", "''") + "'"


def special_folder(name):
    """Desktop or Programs folder of the real shell (OneDrive-redirected desktops included)."""
    rc, out, _ = _ps(f"[Environment]::GetFolderPath({_q(name)})")
    if rc == 0 and out:
        return Path(out)
    return None


def make_shortcut(lnk, target, args='', workdir=None, desc='', icon=None):
    Path(lnk).parent.mkdir(parents=True, exist_ok=True)
    script = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut({_q(lnk)});$s.TargetPath={_q(target)};"
              + (f"$s.Arguments={_q(args)};" if args else '')
              + (f"$s.WorkingDirectory={_q(workdir)};" if workdir else '')
              + (f"$s.Description={_q(desc)};" if desc else '')
              + (f"$s.IconLocation={_q(icon)};" if icon else '') + '$s.Save()')
    rc, _, err = _ps(script)
    if rc != 0 or not Path(lnk).exists():
        raise StepError(f'could not create the shortcut {lnk} ({err[:160]})')


def create_shortcuts(game_exe, setup_exe, want_desktop, want_start, say):
    made = []
    desc = 'Snap Recomp'
    # one exe: a shortcut starts the exe the player downloaded (it hands off to the built game); the built exe only lends its icon
    target = setup_exe if setup_exe and Path(setup_exe).exists() else game_exe
    workdir = str(Path(target).parent)
    if want_desktop:
        d = special_folder('Desktop')
        if d:
            make_shortcut(d / 'Snap Recomp.lnk', target, workdir=workdir, desc=desc, icon=str(game_exe))
            made.append(str(d / 'Snap Recomp.lnk'))
    if want_start:
        p = special_folder('Programs')
        if p:
            folder = p / 'Snap Recomp'
            make_shortcut(folder / 'Snap Recomp.lnk', target, workdir=workdir, desc=desc, icon=str(game_exe))
            made.append(str(folder / 'Snap Recomp.lnk'))
            if setup_exe and Path(setup_exe).exists():
                make_shortcut(folder / 'Repair Snap Recomp.lnk', setup_exe, '--repair', desc='Rebuild Snap Recomp from your game files')
                make_shortcut(folder / 'Uninstall Snap Recomp.lnk', setup_exe, '--uninstall', desc='Remove Snap Recomp')
                made += [str(folder / 'Repair Snap Recomp.lnk'), str(folder / 'Uninstall Snap Recomp.lnk')]
    say(f'{len(made)} shortcut(s) made')
    return made


def register_uninstall(setup_exe, install_location, version, size_bytes=0):
    import winreg
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY, 0, winreg.KEY_SET_VALUE) as k:
        vals = {'DisplayName': 'Snap Recomp', 'DisplayVersion': version, 'Publisher': 'Snap Recomp',
                'InstallLocation': str(install_location), 'DisplayIcon': str(setup_exe),
                'UninstallString': f'"{setup_exe}" /uninstall', 'ModifyPath': f'"{setup_exe}" /repair',
                'QuietUninstallString': f'"{setup_exe}" /uninstall /quiet'}
        for n, v in vals.items():
            winreg.SetValueEx(k, n, 0, winreg.REG_SZ, v)
        winreg.SetValueEx(k, 'NoRepair', 0, winreg.REG_DWORD, 0)
        winreg.SetValueEx(k, 'NoModify', 0, winreg.REG_DWORD, 0)
        if size_bytes:
            winreg.SetValueEx(k, 'EstimatedSize', 0, winreg.REG_DWORD, int(size_bytes // 1024))


def unregister_uninstall():
    import winreg
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY)
    except OSError:
        pass


def dir_size(p):
    n = 0
    for r, _, fs in os.walk(p):
        for f in fs:
            try:
                n += os.path.getsize(os.path.join(r, f))
            except OSError:
                pass
    return n


def uninstall(remove_tools=False, remove_saves=False, ev=None):
    """Take Snap Recomp off this PC. The player's own game files (the folder they chose) are never touched. Saves and settings in
    %APPDATA%\\SnapRecomp stay unless remove_saves. Returns {'removed': [...], 'kept': [...], 'leftover': [paths a running process held]}."""
    ev = ev or Events()
    s = load_settings()
    removed, kept, left = [], [], []

    def rm(p, what):
        p = Path(p)
        if not p.exists():
            return
        safedel.rmtree(p, ignore_errors=True)
        if p.exists():
            left.append(str(p))
        else:
            removed.append(what)
            ev.emit(t='log', text=f'removed {what}')
    for lnk in s.get('shortcuts', []):
        try:
            Path(lnk).unlink()
            removed.append(lnk)
        except OSError:
            pass
    for lnk in s.get('shortcuts', []):
        try:
            Path(lnk).parent.rmdir() if Path(lnk).parent.name == 'Snap Recomp' else None
        except OSError:
            pass
    unregister_uninstall()
    root = s.get('root')
    if root:
        work = Path(root) / 'work'
        pdata = work / 'out' / 'portable_data'
        if pdata.is_dir() and not remove_saves:
            # portable mode keeps the saves inside the work folder: everything else in it goes, the saves stay where they are
            for d in (work, work / 'out'):
                for child in list(d.iterdir()):
                    if child == work / 'out' or child == pdata:
                        continue
                    if child.is_dir():
                        safedel.rmtree(child, ignore_errors=True)
                    else:
                        try:
                            child.unlink()
                        except OSError:
                            pass
                    if child.exists():
                        left.append(str(child))
            removed.append('the built game program (work folder)')
            ev.emit(t='log', text='removed the built game program (work folder)')
            kept.append(f'your saves and settings in {pdata}')
        else:
            rm(work, 'the built game program and, with it, any saves kept in its folder' if pdata.is_dir() else 'the built game program (work folder)')
        g = s.get('game') or {}
        if g.get('extracted_by_setup') and g.get('extract_dir'):
            rm(g['extract_dir'], 'the game data extracted by setup')
        elif g.get('folder'):
            kept.append(f'your game files in {g["folder"]} (never touched)')
    td = s.get('tools_dir') or str(fetch.default_tools_dir())
    if remove_tools:
        rm(td, 'the build tools')
    else:
        kept.append(f'the build tools in {td} (about 8 GB; delete the folder if you do not need them)')
    appdata = Path(os.environ.get('APPDATA') or '') / 'SnapRecomp'
    if remove_saves:
        rm(appdata, 'your saves and settings')
    else:
        kept.append(f'your saves and settings in {appdata}')
    try:
        sf = setup_dir() / 'settings.json'
        sf.unlink()
    except OSError:
        pass
    return {'removed': removed, 'kept': kept, 'leftover': left}


# ------------------------------------------------------------------------------------------------ the run

def game_progress(prog):
    """gamesrc.extract reports (stage, fraction); the game stage's share of the bar: copy update 0-5%, program files 5-10%,
    copy base 10-65%, extract game data 65-100%."""
    spans = {'copy_update': (0.0, 0.05, 'Copying the update data'), 'exefs': (0.05, 0.10, 'Reading the game program'),
             'copy_base': (0.10, 0.65, 'Copying the base game data'), 'romfs': (0.65, 1.0, 'Extracting the game data')}

    def cb(stage, f):
        a, b, text = spans.get(stage, (0.0, 0.1, 'Reading your game files'))
        prog.set('game', a + (b - a) * f, text)
    return cb


def normalize_cfg(cfg):
    c = dict(cfg)
    c['root'] = str(Path(c.get('root') or default_root()))
    c['tools_dir'] = str(Path(c.get('tools_dir') or fetch.default_tools_dir()))
    c.setdefault('shortcuts', {'desktop': False, 'start': False})   # portable: nothing outside the folder unless asked for
    c.setdefault('offline_dirs', [])
    c.setdefault('upscalers', [])
    c.setdefault('accept_license', [])
    return c


def run(cfg, ev, opener=None):
    """The whole setup. Raises StepError with a plain message (the caller turns it into an error event) or Cancelled."""
    cfg = normalize_cfg(cfg)
    root = Path(cfg['root'])
    tools_dir = Path(cfg['tools_dir'])
    work = root / 'work'
    root.mkdir(parents=True, exist_ok=True)
    os.environ['SNAP_TOOLS_DIR'] = str(tools_dir)   # the builder's find_* helpers look there
    table = vc.load_table(pipeline.BUILDER / 'versions.json')
    settings = load_settings()
    settings.update({'root': str(root), 'tools_dir': str(tools_dir), 'completed': False, 'started': time.strftime('%Y-%m-%d %H:%M:%S')})
    settings['cfg'] = {k: cfg.get(k) for k in ('root', 'tools_dir', 'upscalers', 'accept_license', 'shortcuts', 'skip_smoke')}
    save_settings(settings)
    offline = list(cfg['offline_dirs'])

    # which stages have work left decides the weights
    sc_kind = cfg.get('kind')
    if sc_kind not in ('dump', 'extracted'):   # the wizard passes "auto": look at the folder again (a second of reading file names)
        try:
            folder0 = cfg.get('game_dir') or (settings.get('game') or {}).get('folder')
            sc_kind = gamesrc.scan(folder0).kind if folder0 else None
        except StepError as e:
            raise StepError(str(e))
        cfg['kind'] = sc_kind
    need_game = sc_kind == 'dump'
    game_dir = root / 'game'
    already = gamesrc.extracted_already(game_dir) if need_game else None
    weights = {}
    if need_game and not already:
        weights['game'] = 150
    elif need_game:
        weights['game'] = 3
    else:
        weights['game'] = 3
    tool_keys = ['llvm', 'ghidra', 'switch_loader', 'jdk']
    if cfg.get('maps'):
        tool_keys = ['llvm']       # ready-made function lists (developer use): no Ghidra, no Java
    pins = dict(fetch.list_pins(table))
    todo_tools = pending_tools(table, tools_dir, tool_keys)
    py_missing = not (tools_dir / 'python' / 'python.exe').exists()
    weights['tools'] = 5 + (sum(int(pins[k].get('size') or 0) for k in todo_tools) + (15 * MB if py_missing else 0)) / MB / DOWNLOAD_MBPS * 1.4
    mode = cfg.get('winsdk', 'auto')
    need_sdk = needs_winsdk(mode, tools_dir)
    weights['winsdk'] = 90 if need_sdk else 1
    for k, v in BUILD_STEPS.items():
        weights['build:' + k] = v
    if cfg.get('maps'):
        weights['build:maps'] = 3
    weights['finish'] = 10
    prog = Progress(ev, weights)
    work.mkdir(parents=True, exist_ok=True)
    log = EventLog(work / 'builder.log', ev, prog)
    say = log.write

    def stage(name):
        ev.emit(t='stage', id=name, title=STAGE_TITLES[name])
        prog.set(name, 0.0, STAGE_TITLES[name], force=True)

    try:
        # ---- 1. game files
        stage('game')
        folder = cfg.get('game_dir')
        if not folder and not settings.get('game', {}).get('folder'):
            raise StepError('no game folder was chosen.')
        folder = folder or settings['game']['folder']
        keys = cfg.get('keys') or settings.get('game', {}).get('keys_path')
        if need_game:
            ensure_tools(cfg, table, ['nstool'], prog, 'game', say, opener)
            ns_exe = next(Path(tools_dir / 'nstool').rglob('nstool.exe'), None)
            sc = gamesrc.scan(folder)
            if keys:
                sc.keys = Path(keys)
            if not sc.keys:
                raise StepError('the keys file was not found next to the game files. Choose it in the setup again.')
            kinfo = gamesrc.inspect_keys(sc.keys)
            if kinfo.problem:
                say(kinfo.summary() if kinfo.names else f'Keys file "{kinfo.path.name}": {kinfo.problem}')
                raise gamesrc.GameFilesError(kinfo.problem, kinfo.code)
            ns = gamesrc.NsTool(ns_exe, sc.keys)
            scratch = setup_dir() / 'scratch_ident'
            try:
                gamesrc.identify_all(sc, ns, scratch, say=say, keys=kinfo)
            finally:
                safedel.rmtree(scratch, ignore_errors=True)
            plan = gamesrc.pick(sc)
            say(f'Using {plan.patch[0].path.name} (version {gamesrc.version_name(plan.patch[1]["version"])}) with {plan.base[0].path.name}.')
            if already:
                paths, hashes = already, already.get('hashes', {})
                say('The game data was extracted earlier; using it.')
            else:
                free = shutil.disk_usage(root).free
                need = gamesrc.space_estimate(plan)['peak'] + GB
                if free < need:
                    raise StepError(f'not enough free space on {drive_of(root)}: extracting the game needs about {fmt_gb(need)} for a short time '
                                    f'and {fmt_gb(free)} is free. Choose another drive in the setup (the extracted game data stays: '
                                    f'{fmt_gb(gamesrc.space_estimate(plan)["stays"])}).')
                paths = gamesrc.extract(plan, ns, game_dir, say=say, progress=game_progress(prog), keys=kinfo)
                hashes = paths.get('hashes', {})
            main, sdk, romfs = str(paths['main']), str(paths['sdk']), str(paths['romfs'])
            gamesrc.remember(settings, sc, plan, hashes, keys)
            settings['game'].update({'main': main, 'sdk': sdk, 'romfs': romfs, 'extract_dir': str(game_dir), 'extracted_by_setup': True})
        else:
            sc = gamesrc.scan(folder)
            if not sc.extracted:
                raise StepError('the extracted game files (main, sdk and the romfs folder) were not found in the chosen folder.')
            main, sdk, romfs = (str(sc.extracted[k]) for k in ('main', 'sdk', 'romfs'))
            gamesrc.remember(settings, sc)
            settings['game'].update({'main': main, 'sdk': sdk, 'romfs': romfs, 'extracted_by_setup': False})
        res = vc.check_all(main, sdk, romfs, table)
        if not res.ok:
            raise StepError('these are not the right game files: ' + ' '.join(res.problems))
        for n in res.notes:
            say('note: ' + n)
        save_settings(settings)
        prog.finish_stage('game', 'Your game files are the right version (2.0.1)')

        # ---- 2. tools
        stage('tools')
        ensure_tools(cfg, table, tool_keys, prog, 'tools', say, opener)
        ensure_python(cfg, table, say)
        prog.finish_stage('tools', 'Build tools ready')

        # ---- 3. Microsoft C runtime files
        stage('winsdk')
        if needs_winsdk(mode, tools_dir):
            if not cfg.get('accept_ms_license'):
                raise StepError("Microsoft's license for the C runtime and Windows SDK files was not accepted, so they were not downloaded. "
                                f'Read it at {winsdk.LICENSE_URL}, then run the setup again and accept it.')
            ensure_tools(cfg, table, ['xwin'], prog, 'winsdk', say, opener)
            xwin_exe = next((tools_dir / 'xwin').rglob('xwin.exe'))
            prog.set('winsdk', 0.1, 'Downloading the Microsoft C runtime files (about 600 MB)', force=True)
            winsdk.install(xwin_exe, tools_dir, True, say=say)
        prog.finish_stage('winsdk', 'Microsoft C runtime files ready')

        # ---- 4. build
        stage('build')
        opt = pipeline.Options(main=main, sdk=sdk, romfs=romfs, work=str(work), maps=cfg.get('maps'), prebuilt=cfg.get('prebuilt'),
                               jobs=int(cfg.get('jobs') or 0), skip_smoke=bool(cfg.get('skip_smoke')), harness=cfg.get('harness'),
                               smoke_timeout=int(cfg.get('smoke_timeout') or 1200), winsdk=mode, upscalers=cfg.get('upscalers') or [],
                               accept_license=list(cfg.get('accept_license') or []), cache_dir=cfg.get('cache_dir'),
                               llvm=cfg.get('llvm'), python=cfg.get('python'))
        exe, smoke = pipeline.build(opt, log)
        if smoke is not None and not smoke[0]:
            raise StepError('the program was built, but the check run did not reach the title screen: ' + str(smoke[1]))
        for k in list(prog.w):
            if k.startswith('build:'):
                prog.set(k, 1.0)
        prog.set('build:assemble', 1.0, 'Built', force=True)

        # ---- 5. shortcuts and registration
        stage('finish')
        setup_exe = cfg.get('setup_exe')
        game_exe = Path(exe)
        shortcuts = []
        sc_cfg = cfg.get('shortcuts') or {}
        if sc_cfg.get('desktop') or sc_cfg.get('start'):
            shortcuts = create_shortcuts(game_exe, setup_exe, sc_cfg.get('desktop'), sc_cfg.get('start'), say)
        ver = pipeline.builder_version()
        unregister_uninstall()   # an older version added an "Apps & features" entry; a portable folder has none (delete the folder to remove)
        settings.update({'completed': True, 'finished': time.strftime('%Y-%m-%d %H:%M:%S'), 'exe': str(game_exe), 'shortcuts': shortcuts,
                         'setup_exe': setup_exe, 'builder_version': ver, 'work': str(work)})
        save_settings(settings)
        prog.finish_stage('finish', 'Done')
        ev.emit(t='done', exe=str(game_exe), seconds=round(time.time() - prog.t0), shortcuts=shortcuts, work=str(work),
                disk_used=dir_size(work) + (dir_size(game_dir) if need_game else 0))
        return game_exe
    finally:
        log.close()


# ------------------------------------------------------------------------------------------------ command line

def friendly(e):
    return str(e)


def cli(a):
    """Entry for `snap_builder.py setup <action>`. Returns an exit code."""
    action = a.action
    if action == 'scan':
        cfg = {'root': a.root, 'tools_dir': a.tools_dir}
        nstool = None
        td = Path(a.tools_dir or fetch.default_tools_dir())
        table = vc.load_table(pipeline.BUILDER / 'versions.json')
        offline = list(a.offline_dir or [])
        if True:
            try:   # NSTool (0.5 MB, MIT) is bundled with the setup; fall back to the pinned download
                ensure_tools({'tools_dir': str(td), 'offline_dirs': offline, 'downloads_ok': a.yes}, table, ['nstool'],
                             Progress(Events(out=open(os.devnull, 'w')), {'x': 1}), 'x', lambda m: None)
            except (StepError, fetch.FetchError):
                pass
            nstool = next((td / 'nstool').rglob('nstool.exe'), None) if (td / 'nstool').exists() else None
        out = scan_folder(a.folder, a.keys, nstool, table, cfg)
        print(json.dumps(out, ensure_ascii=False))
        return 0
    setup_dir().mkdir(parents=True, exist_ok=True)
    ev = Events(log_path=str(setup_dir() / 'setup-events.log'))
    try:
        if action in ('run', 'repair'):
            if action == 'repair':
                s = load_settings()
                g = s.get('game') or {}
                ok, why = gamesrc.still_there(s)
                if not ok:
                    ev.emit(t='error', code='game_moved', text=f'Your game files were not found where setup left them ({why}). Choose the folder again.', retry=False,
                            action='choose_folder')
                    return 1
                cfg = dict(s.get('cfg') or {})
                cfg.update({'game_dir': g.get('folder'), 'keys': g.get('keys_path'), 'kind': g.get('kind'), 'downloads_ok': True,
                            'accept_ms_license': True, 'setup_exe': s.get('setup_exe')})
                if g.get('extracted_by_setup'):
                    cfg['kind'] = 'dump'
            else:
                cfg = json.loads(Path(a.config).read_text(encoding='utf-8-sig'))
            run(cfg, ev)
            return 0
        if action == 'uninstall':
            r = uninstall(a.remove_tools, a.remove_saves, ev)
            ev.emit(t='done', **r)
            return 0
    except StepError as e:
        ev.emit(t='error', code='step', text=friendly(e), retry=True)
        return 1
    except fetch.FetchError as e:
        ev.emit(t='error', code='download', text=str(e), retry=True)
        return 1
    except Cancelled:
        ev.emit(t='error', code='cancelled', text='Setup was stopped. Run it again to continue where it stopped.', retry=True)
        return 1
    except Exception:
        import traceback
        tb = traceback.format_exc()
        try:
            (setup_dir() / 'setup-crash.log').write_text(tb, encoding='utf-8')
        except OSError:
            pass
        ev.emit(t='error', code='internal', text='Setup hit an unexpected problem. The details are in ' + str(setup_dir() / 'setup-crash.log'), retry=True)
        return 1
    finally:
        ev.close()
    return 2
