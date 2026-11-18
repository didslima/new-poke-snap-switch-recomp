"""Function and jump-table discovery with headless Ghidra (the same commands as scripts/ghidra_analyze.ps1).

The lifter needs functions_final.tsv and jump_tables.tsv. Both list addresses inside the game, so they cannot be shipped; they are
made on the user's PC: import the decompressed main NSO, analyze (about 13 minutes with a 12 GB heap), export, reconcile.
Ghidra 12.1.2 with the Switch loader extension and JDK 21 must be installed; the first release takes local paths (--ghidra, --jdk).
Not exercised end to end yet: the clean-machine milestone runs it for real. build_commands() is unit tested.
"""
import hashlib
import os
import shutil
import time
from pathlib import Path

from . import nso as nsomod
from . import paths
from .util import StepError, run

PROJECT = 'snap-v262144'
HEAP = '12G'
RESULT_FILES = ('functions_final.tsv', 'jump_tables.tsv')


def layout(work):
    """The folder tree scripts/reconcile_functions.py expects, under <work>/ghidra/root."""
    root = Path(work) / 'ghidra' / 'root'
    return {
        'root': root,
        'nso': root / 'extracted' / 'v262144' / 'exefs' / 'main.uncompressed.nso',
        'project_dir': root / 'analysis' / 'ghidra',
        'maps': root / 'analysis' / 'maps',
        'fde': root / 'analysis' / 'ghidra' / f'{PROJECT}.fde.tsv',
    }


def build_commands(ghidra, py, repo, work, nso_sha256, build_id, timeout_min=60):
    """The three command lines: analyze, export, reconcile."""
    L = layout(work)
    headless = str(Path(ghidra) / 'support' / 'analyzeHeadless.bat')
    scripts = str(Path(repo) / 'scripts' / 'ghidra')
    pd = str(L['project_dir'])
    analyze = [headless, pd, PROJECT, '-import', str(L['nso']), '-overwrite', '-analysisTimeoutPerFile', str(timeout_min * 60),
               '-scriptPath', scripts, '-preScript', 'SeedFdeStarts.java', str(L['fde']),
               '-postScript', 'DumpAnalysisOptions.java', str(L['project_dir'] / f'{PROJECT}.analyzer-options.tsv')]
    export = [headless, pd, PROJECT, '-process', L['nso'].name, '-noanalysis', '-readOnly', '-scriptPath', scripts,
              '-postScript', 'ExportMaps.java', str(L['maps']), str(L['fde']), nso_sha256, build_id]
    reconcile = [str(py), '-I', str(Path(repo) / 'scripts' / 'reconcile_functions.py'), '--root', str(L['root']),
                 '--project', PROJECT, '--out', str(L['maps'])]
    return analyze, export, reconcile


def java_env(jdk, user_dir=None):
    """The environment for Ghidra. user_dir: keep Ghidra's own settings, cache and temp files there instead of the user's AppData
    and Temp (a portable install writes nothing outside its folder; 2026-11-18: a setup build left %APPDATA%\\ghidra,
    %LOCALAPPDATA%\\ghidra and %TEMP%\\<user>-ghidra behind). Ghidra and Java take these places from the variables set here."""
    env = dict(os.environ)
    if jdk:
        env['JAVA_HOME'] = str(jdk)
        env['PATH'] = str(Path(jdk) / 'bin') + os.pathsep + env.get('PATH', '')
    env['GHIDRA_HEADLESS_MAXMEM'] = HEAP
    if user_dir:
        u = Path(user_dir)
        places = {'APPDATA': u / 'Roaming', 'XDG_CONFIG_HOME': u / 'Roaming', 'LOCALAPPDATA': u / 'Local', 'XDG_CACHE_HOME': u / 'Local',
                  'TEMP': u / 'Temp', 'TMP': u / 'Temp', 'XDG_RUNTIME_DIR': u / 'Temp'}
        for name, d in places.items():
            d.mkdir(parents=True, exist_ok=True)
            env[name] = str(d)
    return env


def user_dir_for(work, env=None, builder_dir=None):
    """<work>\\ghidra_user for a portable install, None for a source checkout or an unpacked builder zip (there Ghidra keeps using the
    developer's own settings, which may hold a Switch loader installed per user)."""
    return Path(work) / 'ghidra_user' if paths.install_root(env, builder_dir) else None


def run_ghidra(ghidra, jdk, py, repo, work, main_nso, log, timeout_min=60):
    """Make functions_final.tsv and jump_tables.tsv under <work>/maps with Ghidra. Returns the folder."""
    if not ghidra:
        raise StepError('Ghidra 12.1.2 was not found. Install it with its Switch loader extension and JDK 21 and pass --ghidra <folder> '
                        '(and --jdk <folder>), or pass --maps <folder> with functions_final.tsv and jump_tables.tsv you already have.')
    L = layout(work)
    for d in (L['project_dir'], L['maps'], L['nso'].parent):
        d.mkdir(parents=True, exist_ok=True)
    head = nsomod.read_header(main_nso)
    nsomod.write_uncompressed(main_nso, head, L['nso'])
    with open(L['nso'], 'rb') as f:
        sha = hashlib.sha256(f.read()).hexdigest()   # Ghidra's export script records it; it is not printed
    env = java_env(jdk, user_dir_for(work))
    analyze, export, reconcile = build_commands(ghidra, py, repo, work, sha, head.build_id, timeout_min)
    t0 = time.time()
    log.write('ghidra: analyzing (about 13 minutes, 12 GB heap)')
    run(analyze, log, env=env, timeout=timeout_min * 60 + 300, what='Ghidra analysis')
    log.write(f'ghidra: analysis done in {time.time() - t0:.0f}s; exporting')
    run(export, log, env=env, timeout=20 * 60, what='Ghidra export')
    log.write('ghidra: reconciling the function list')
    run(reconcile, log, timeout=30 * 60, what='function reconcile')
    out = Path(work) / 'maps'
    out.mkdir(parents=True, exist_ok=True)
    for n in RESULT_FILES:
        if not (L['maps'] / n).exists():
            raise StepError(f'Ghidra finished but {n} was not produced (see the log).')
        shutil.copyfile(L['maps'] / n, out / n)
    return out
