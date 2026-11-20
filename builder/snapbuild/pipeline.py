"""The build pipeline: preflight, version check, nso2elf, maps, snap_gen, compile, link, config, smoke run. Each step stamps its
inputs under <work>/stamps so a rerun skips what is finished; everything goes to <work>/builder.log."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import cache as gcache, ghidra_maps, nso as nsomod, patches as gpatches, toolchain
from . import versioncheck as vc
from .util import BELOW_NORMAL, CREATE_NO_WINDOW, Stamps, StepError, files_digest, run, sha256_file, sha256_text
from . import safedel
from . import userdata
from . import paths

REPO = Path(__file__).resolve().parents[2]   # builder/snapbuild/pipeline.py -> repo root (same layout in the release zip)
BUILDER = REPO / 'builder'
STEPS = ('elf', 'maps', 'gen_main', 'gen_sdk', 'compile_main', 'compile_sdk', 'link')
KEEP_BUILDS = ('previous',)   # folders under <work>/builds kept after a successful switch
LAUNCHER_KEYS = ('upscalers_offered', 'update_check', 'update_channel')   # config.json keys the launcher sets; a rebuild carries them over
# build arguments recorded in <work>/last_build.json so an update can run the same build again (prebuilt, force and compare_generated are left out)
RECORD_ARGS = ('main', 'sdk', 'romfs', 'work', 'maps', 'llvm', 'python', 'vulkan_lib', 'ghidra', 'jdk', 'harness', 'ghost', 'cache_dir', 'jobs',
               'smoke_timeout', 'cache', 'skip_smoke', 'upscalers', 'accept_license')


def builder_version():
    try:
        return (BUILDER / 'VERSION').read_text(encoding='utf-8').strip()
    except OSError:
        return 'unknown'


def write_last_build(work, opt, stamps, smoke_seconds):
    """Record what this build used and how long each step took, for the updater (rebuild arguments and the time estimate)."""
    args = {k: getattr(opt, k) for k in RECORD_ARGS if getattr(opt, k, None) not in (None, '', [], 0) or k in ('cache', 'skip_smoke')}
    steps = {}
    for s in STEPS:
        try:
            steps[s] = float(json.loads((stamps.dir / f'{s}.json').read_text(encoding='utf-8')).get('seconds', 0))
        except (OSError, ValueError):
            pass
    doc = {'version': builder_version(), 'finished': time.strftime('%Y-%m-%d %H:%M:%S'), 'args': args, 'steps': steps,
           'smoke_seconds': round(smoke_seconds, 1)}
    (Path(work) / 'last_build.json').write_text(json.dumps(doc, indent=1), encoding='utf-8')
LIFTER_FILES = ('ehlsda.py', 'elfimg.py', 'flags.py', 'gen.py', 'jumptables.py', 'lifter.py', 'module.py', 'patches.py', 'regs.py',
                'simd_c.py', 'snap_common.py', 'snap_gen.py')


class Options:
    """Plain holder for the command-line values."""

    def __init__(self, **kw):
        self.main = self.sdk = self.romfs = self.work = None
        self.maps = self.llvm = self.python = self.vulkan_lib = self.ghidra = self.jdk = None
        self.prebuilt = None
        self.jobs = 0
        self.skip_smoke = False
        self.prepare_shaders = False   # start the shader preparation (SnapRecomp.exe --prepare-shaders) in the background after the build
        self.harness = None
        self.ghost = None
        self.force = []
        self.compare_generated = None
        self.smoke_timeout = 1200   # the first start translates the game's shaders: about 7 minutes on a busy PC, so allow 20
        self.winsdk = 'auto'         # where the C runtime and Windows SDK files come from: auto, vs or xwin
        self.upscalers = None        # names to fetch after the link (default: the ones marked default in versions.json)
        self.accept_license = []     # upscaler names whose vendor license the user accepted on the command line
        self.cache = True            # reuse function lists and generated C from %LOCALAPPDATA%\SnapRecomp\cache
        self.cache_dir = None
        self.__dict__.update(kw)


def tree_digest(folder, pattern='*.c'):
    """Digest over (name, content hash) of the generated C files (manifest.json holds timings and is left out)."""
    h = hashlib.sha256()
    n = 0
    for p in sorted(Path(folder).glob(pattern)):
        h.update(p.name.encode())
        h.update(b'\0')
        h.update(sha256_file(p).encode())
        n += 1
    return h.hexdigest(), n


def find_prebuilt(arg):
    for c in (arg, REPO / 'prebuilt', REPO / 'build' / 'release' / 'prebuilt'):
        if c and (Path(c) / 'manifest.json').exists():
            return Path(c)
    raise StepError('the prebuilt runtime objects were not found. They ship with the builder (folder "prebuilt"); '
                    'a developer makes them with builder/release_prebuilt.py. Pass the folder with --prebuilt.')


def abi_of(include_dir):
    m = re.search(r'^#define SNAP_RUNTIME_ABI (\d+)', (Path(include_dir) / 'cpu.h').read_text(encoding='utf-8'), re.M)
    if not m:
        raise StepError('the prebuilt include folder has no SNAP_RUNTIME_ABI in cpu.h')
    return int(m.group(1))


def check_prebuilt(pre):
    man = json.loads((pre / 'manifest.json').read_text(encoding='utf-8'))
    abi = abi_of(pre / 'include')
    if man.get('runtime_abi') != abi:
        raise StepError(f'the prebuilt folder is inconsistent: manifest says runtime ABI {man.get("runtime_abi")}, headers say {abi}. '
                        f'Use the prebuilt folder and the builder from the same release.')
    again = 'Restore the prebuilt folder from the same release as the builder.'
    for o in man['objs']:
        p = pre / 'obj' / o['name']
        if not p.is_file():
            raise StepError(f'prebuilt object {o["name"]} is missing: the prebuilt folder is incomplete.')
        if sha256_file(p) != o.get('sha256'):
            raise StepError(f'prebuilt object {o["name"]} does not match its manifest SHA-256. {again}')
    for d in man.get('dlls', []):
        rel = d.get('path') or 'bin/' + d['name']
        p = pre / rel
        if not p.is_file():
            raise StepError(f'prebuilt file {rel} is missing: the prebuilt folder is incomplete.')
        if sha256_file(p) != d.get('sha256'):
            raise StepError(f'prebuilt file {rel} does not match its manifest SHA-256. {again}')
    return man


def run_cached(cache_root, kind, key, folder, outputs, fn, log):
    """Restore `folder` from the private cache when a complete entry for (kind, key) exists and holds every output; otherwise run fn
    and store the folder. Cache trouble never fails a build: it is logged and the work runs. Returns True on a cache hit."""
    if cache_root and kind:
        try:
            if gcache.restore(cache_root, kind, key, folder) and all(Path(o).exists() for o in outputs):
                return True
        except (gcache.CacheError, OSError) as e:
            log.write(f'cache: restore failed ({e}); running the step')
    fn()
    if cache_root and kind and all(Path(o).exists() for o in outputs):
        try:
            gcache.store(cache_root, kind, key, folder)
            log.write(f'cache: stored {kind}')
        except (gcache.CacheError, OSError) as e:
            log.write(f'cache: not stored ({e})')
    return False


def build(opt, log):
    t_all = time.time()
    work = Path(opt.work).resolve()
    repo = REPO
    stamps = Stamps(work)
    table = vc.load_table(BUILDER / 'versions.json')
    jobs = opt.jobs or toolchain.default_jobs()

    # ---- preflight and tools
    log.write('step preflight')
    probs = toolchain.preflight(work, need_ghidra=not opt.maps, log=log)
    if probs:
        raise StepError('preflight failed:\n  ' + '\n  '.join(probs))
    llvm = toolchain.find_llvm(opt.llvm, repo)
    py = toolchain.find_python(opt.python, repo)
    link_env, compile_env, sdk_desc = toolchain.native_env(getattr(opt, 'winsdk', 'auto'))
    pre = find_prebuilt(opt.prebuilt)
    vk = toolchain.find_vulkan_lib(opt.vulkan_lib, repo, pre)
    man = check_prebuilt(pre)
    log.write(f'tools: clang {toolchain.clang_version(llvm)} in {llvm}; python {py}; prebuilt {pre} (runtime ABI {man["runtime_abi"]}); system files: {sdk_desc}')
    want_llvm = table.get('tools', {}).get('llvm', {}).get('version_major')
    if want_llvm and not toolchain.clang_version(llvm).startswith(str(want_llvm) + '.'):
        log.write(f'warning: clang {toolchain.clang_version(llvm)} differs from the pinned major version {want_llvm}; the result may differ')

    # ---- version check (always runs, takes about a second)
    log.write('step version check')
    res = vc.check_all(opt.main, opt.sdk, opt.romfs, table)
    for n in res.notes:
        log.write('note: ' + n)
    if not res.ok:
        raise StepError('version check failed:\n  ' + '\n  '.join(res.problems))
    log.write('version check passed: main, sdk and romfs are game version ' + vc.EXPECTED_NAME)
    main_digest, sdk_digest = table['nso']['main']['image_sha256'], table['nso']['sdk']['image_sha256']
    for s in opt.force:
        stamps.clear(s)

    cache_root = None
    if opt.cache:
        try:
            cache_root = gcache.open_root(opt.cache_dir)
            log.write(f'cache: {cache_root}')
        except (gcache.CacheError, OSError) as e:
            log.write(f'cache: not used ({e})')

    def step(name, key, outputs, fn, cache_kind=None, cache_dir=None):
        """Run fn unless the stamp matches. With cache_kind, a finished folder is restored from / stored to the private cache
        (key = the stamp key, which holds no path of this work folder)."""
        if stamps.done(name, key, outputs):
            log.write(f'step {name}: up to date, skipped')
            return False
        log.write(f'step {name}')
        t0 = time.time()
        stamps.clear(name)
        if run_cached(cache_root, cache_kind, key, cache_dir, outputs, fn, log):
            log.write(f'step {name}: restored from the cache (skipped the work)')
        stamps.write(name, key, time.time() - t0)
        log.write(f'step {name} done in {time.time() - t0:.0f}s')
        return True

    # ---- nso2elf
    elf_dir = work / 'elf'
    elf_dir.mkdir(exist_ok=True)
    elf_key = sha256_text(main_digest, sdk_digest, files_digest([repo / 'scripts' / 'nso2elf.py']))

    def do_elf():
        for role, src in (('main', opt.main), ('sdk', opt.sdk)):
            run([py, '-I', repo / 'scripts' / 'nso2elf.py', src, elf_dir / f'{role}.elf'], log, what=f'nso2elf {role}')
    step('elf', elf_key, [elf_dir / 'main.elf', elf_dir / 'sdk.elf'], do_elf)

    # ---- maps
    maps_dir = work / 'maps'
    maps_dir.mkdir(exist_ok=True)
    if opt.maps:
        src = Path(opt.maps)
        for n in ghidra_maps.RESULT_FILES:
            if not (src / n).exists():
                raise StepError(f'--maps folder {src} has no {n}.')
        maps_key = sha256_text('maps-dir', *[sha256_file(src / n) for n in ghidra_maps.RESULT_FILES])
        maps_cache = None   # the user's own lists: nothing to cache

        def do_maps():
            for n in ghidra_maps.RESULT_FILES:
                shutil.copyfile(src / n, maps_dir / n)
    else:
        tl = table.get('tools', {})
        maps_cache = 'maps'
        maps_key = sha256_text('ghidra', main_digest, tl.get('ghidra', {}).get('version', ''), tl.get('switch_loader', {}).get('version', ''),
                               tl.get('jdk', {}).get('version', ''), ghidra_maps.HEAP,
                               files_digest(sorted((repo / 'scripts' / 'ghidra').glob('*.java')) + [repo / 'scripts' / 'reconcile_functions.py']))

        def do_maps():
            ghidra = toolchain.find_ghidra(opt.ghidra, repo)
            jdk = toolchain.find_jdk(opt.jdk, repo)
            ghidra_maps.run_ghidra(ghidra, jdk, py, repo, work, opt.main, log)
    step('maps', maps_key, [maps_dir / n for n in ghidra_maps.RESULT_FILES], do_maps, cache_kind=maps_cache, cache_dir=maps_dir)
    check_maps(maps_dir, table, log)

    # ---- hash-pinned patches: checked against the exact module images before anything is generated (none are staged today)
    log.write('step patch check')
    images = {}
    for role, src, dg in (('main', opt.main, main_digest), ('sdk', opt.sdk, sdk_digest)):
        hd = nsomod.read_header(src)
        images[role] = (nsomod.read_image(src, hd), hd.build_id, dg)
    try:
        patches_key = gpatches.check_dir(BUILDER / 'patches', images)
    except gpatches.PatchError as e:
        raise StepError('patch check failed: ' + str(e))
    log.write('patch check passed' + (f' (patch set {patches_key[:12]})' if patches_key else ' (no patches staged)'))
    images.clear()

    # ---- generate C and compile, per module
    gen_dir = work / 'generated'
    include = pre / 'include'
    abi = man['runtime_abi']
    lifter_key = files_digest([repo / 'recomp' / f for f in LIFTER_FILES])
    maps_files_key = sha256_text(*[sha256_file(maps_dir / n) for n in ghidra_maps.RESULT_FILES])
    gen_env = dict(os.environ, SNAP_ELF_MAIN=str(elf_dir / 'main.elf'), SNAP_ELF_SDK=str(elf_dir / 'sdk.elf'),
                   SNAP_MAPS=str(maps_dir), SNAP_RUNTIME_INCLUDE=str(include))
    build_env = dict(compile_env, SNAP_LLVM_BIN=str(llvm), SNAP_RUNTIME_INCLUDE=str(include))
    compile_keys = {}
    report = {}
    for mod in ('main', 'sdk'):
        gkey = sha256_text(elf_key, maps_files_key, lifter_key, abi, mod, patches_key)

        def do_gen(mod=mod):
            run([py, '-I', repo / 'recomp' / 'snap_gen.py', '--module', mod, '--out', gen_dir, '--jobs', str(min(12, max(2, jobs + 2)))],
                log, env=gen_env, timeout=40 * 60, what=f'snap_gen {mod}')
        step(f'gen_{mod}', gkey, [gen_dir / mod / f'{mod}_table.c'], do_gen, cache_kind=f'gen-{mod}', cache_dir=gen_dir / mod)
        digest, n = tree_digest(gen_dir / mod)
        log.write(f'generated C {mod}: {n} files, tree digest {digest}')
        report[mod] = {'files': n, 'digest': digest}
        cflags = '-march=x86-64-v3'
        ckey = sha256_text(gkey, digest, cflags, toolchain.clang_version(llvm), toolchain.clang_target(llvm), files_digest(sorted(include.glob('*.h'))))
        compile_keys[mod] = ckey
        lib = work / 'build' / mod / 'lib' / f'snap_{mod}.lib'

        def do_compile(mod=mod):
            run([py, '-I', repo / 'recomp' / 'snap_build.py', '--gen', gen_dir / mod, '--out', work / 'build' / mod, '--jobs', str(jobs),
                 '--pgo', 'off', '--lib', f'snap_{mod}', '--cflags=' + cflags], log, env=build_env, timeout=90 * 60, what=f'compile {mod}')
        step(f'compile_{mod}', ckey, [lib], do_compile)
    (work / 'generated_digest.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    if opt.compare_generated:
        cmp = Path(opt.compare_generated)
        same = True
        for mod in ('main', 'sdk'):
            d2, n2 = tree_digest(cmp / mod)
            ok = d2 == report[mod]['digest']
            same &= ok
            log.write(f'compare generated {mod}: {"identical" if ok else "DIFFERENT"} ({report[mod]["files"]} vs {n2} files)')
        if not same:
            raise StepError('the generated C differs from the reference folder (see the log).')

    # ---- link (into the work folder; the user-facing folder is assembled and switched below)
    linkdir = work / 'build' / 'link'
    linkdir.mkdir(parents=True, exist_ok=True)
    exe_link = linkdir / 'SnapRecomp.exe'
    link_key = sha256_text(compile_keys['main'], compile_keys['sdk'], sha256_file(pre / 'manifest.json'), vk.stat().st_size, 'windows-subsystem')   # the subsystem is part of the key: an older cached console-subsystem link is not reused

    def do_link():
        env = link_env
        objs = [pre / 'obj' / o['name'] for o in man['objs']]
        libs = [work / 'build' / 'main' / 'lib' / 'snap_main.lib', work / 'build' / 'sdk' / 'lib' / 'snap_sdk.lib', vk]
        libs += [f'-l{n}' for n in ('user32', 'gdi32', 'bcrypt', 'ole32', 'avrt', 'shell32', 'advapi32', 'winmm', 'oleaut32', 'dbghelp', 'dwmapi', 'winhttp', 'comctl32', 'd3d11', 'dxgi', 'd3dcompiler')]
        for lf in man.get('libs', []):
            libs.append(pre / 'lib' / lf)
        res_file = []
        rc = pre / 'res' / 'snap_icon.rc'
        if rc.exists() and (llvm / 'llvm-rc.exe').exists():   # the shipped fallback icon
            (work / 'build').mkdir(exist_ok=True)
            run([llvm / 'llvm-rc.exe', '/FO', work / 'build' / 'snap_icon.res', rc.name], log, cwd=str(rc.parent), what='compile icon resource')
            res_file = [work / 'build' / 'snap_icon.res']
        cmd = [llvm / 'clang++.exe', '-fuse-ld=lld', '-Wl,/manifest:embed', '-Wl,/manifestinput:' + str(pre / 'res' / 'snap_utf8.manifest'),
               '-Wl,/subsystem:windows', '-Wl,/entry:mainCRTStartup',   # no console window on double-click; host/boot.cpp attaches to the parent terminal when there is one
               '-o', exe_link] + objs + res_file + libs
        run(cmd, log, env=env, timeout=30 * 60, what='link SnapRecomp.exe')
    step('link', link_key, [exe_link], do_link)

    # ---- assemble a NEW output folder; the current one (out/) is replaced only after the self-test and the smoke run pass
    out = work / 'out'
    builds = work / 'builds'
    build_id = time.strftime('%Y%m%d-%H%M%S') + '-' + link_key[:8]
    stage = builds / build_id
    builds.mkdir(exist_ok=True)
    safedel.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    exe = stage / 'SnapRecomp.exe'
    shutil.copyfile(exe_link, exe)
    for d in man.get('dlls', []):
        if d.get('path', 'bin/').startswith('bin/'):
            shutil.copyfile(pre / 'bin' / d['name'], stage / d['name'])
    if man.get('harness') and (pre / man['harness']).exists():   # self-contained shader harness next to the exe
        shutil.copytree(pre / man['harness'], stage / man['harness'])
    for keep in ('upscalers', 'licenses', 'upscalers.json'):   # what the previous build had fetched stays (the user accepted those licenses)
        old = out / keep
        if old.exists():
            (shutil.copytree if old.is_dir() else shutil.copyfile)(old, stage / keep)
    for name in ('LICENSE', 'LICENSE-MIT', 'THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(repo / name, stage / name)
    if (pre / 'licenses').is_dir():
        shutil.copytree(pre / 'licenses', stage / 'licenses', dirs_exist_ok=True)
    cfg = stage / 'config.json'
    # romfs_root is read by the game; main_elf and sdk_elf are the game images the launcher passes as --main and --sdk.
    # builder_version, builder_dir, python and work let the launcher's updater find this builder again.
    conf = {'romfs_root': str(Path(res.romfs_root).resolve()), 'main_elf': str(elf_dir / 'main.elf'), 'sdk_elf': str(elf_dir / 'sdk.elf'),
            'builder_version': builder_version(), 'builder_dir': str(BUILDER), 'python': str(py), 'work': str(work.resolve())}
    conf.update(move_keys(work))
    try:   # settings the launcher itself wrote into the old config.json survive a rebuild
        old_conf = json.loads((out / 'config.json').read_text(encoding='utf-8'))
        for k in LAUNCHER_KEYS:
            if k in old_conf:
                conf[k] = old_conf[k]
    except (OSError, ValueError):
        pass
    cfg.write_text(json.dumps(conf, indent=1), encoding='utf-8')
    # the launcher (the exe started without arguments) checks the romfs against the published sentinels at every start
    # install check (builder/snapbuild/installcheck.py): the launcher's "Check install" button reads the hash lists from launcher_data.json
    # and compares the exe, DLLs, converted game images and the program files against install_record.json written here
    from . import installcheck as _ic
    _ih = _ic.load_hashes(BUILDER)
    (stage / 'launcher_data.json').write_text(json.dumps({'sentinels': table['romfs']['sentinels'], 'file_count': table['romfs']['file_count'],
                                                          'upscalers': table.get('upscalers', {}),
                                                          'install': {'romfs_listing_sha256': table['romfs']['listing_sha256'],
                                                                      'runtime_files': {k: v for k, v in _ih['runtime_files'].items() if isinstance(v, list)}}}), encoding='utf-8')
    _ic.write_record(stage, {'main_elf': elf_dir / 'main.elf', 'sdk_elf': elf_dir / 'sdk.elf', 'main_nso': opt.main, 'sdk_nso': opt.sdk},
                     builder_version(), names={'SnapRecomp.exe'} | {d['name'] for d in man.get('dlls', []) if d.get('path', 'bin/').startswith('bin/')})
    log.write(f'assembled {stage}')
    # Upscaler runtime files are never shipped by us: fetch what the user picked and accepted (a vendor license the user did not accept
    # is skipped, not an error: the game hides that upscaler). The launcher offers the same later.
    from . import fetch as _fetch, upscalers as _up
    want = [n for n in (opt.upscalers if opt.upscalers is not None else _up.default_names(table))]
    todo = [n for n in want if n in opt.accept_license and not _up.installed(_up.pins(table)[n], stage)]
    skipped = [n for n in want if n not in opt.accept_license and not _up.installed(_up.pins(table)[n], stage)]
    if skipped:
        log.write('upscalers not fetched (license not accepted with --accept-license): ' + ', '.join(skipped) + '. Use the launcher or '
                  '"snap_builder.py upscalers --install-dir <out folder>" later.')
    if todo:
        try:
            r = _up.run(table, todo, stage, work / 'downloads', set(todo), lambda q: False, say=log.write)
            log.write('upscalers: ' + ', '.join(f'{k} {v}' for k, v in r.items()))
        except _fetch.FetchError as e:
            log.write('upscaler fetch failed (the build is fine, the game runs without it): ' + str(e))
    log.write(f'build finished in {time.time() - t_all:.0f}s: {exe}')

    # ---- self-test, then smoke run; only then does out/ switch
    problems = selftest(stage, man, cfg)
    if problems:
        log.write('self-test FAILED:\n  ' + '\n  '.join(problems))
        raise StepError(f'the new build did not pass its self-test (it stays in {stage}; {out} is unchanged):\n  ' + '\n  '.join(problems))
    log.write('self-test passed')
    smoke_seconds = 0.0
    if opt.skip_smoke:
        log.write('smoke run skipped (--skip-smoke)')
        ok, info = None, None
    else:
        t_smoke = time.time()
        ok, info = smoke(opt, exe, cfg, work, table, log)
        smoke_seconds = time.time() - t_smoke
        if not ok:
            log.write(f'the new build stays in {stage}; {out} is unchanged')
            return stage / 'SnapRecomp.exe', (False, info)
    promote(work, stage, build_id, log)
    write_last_build(work, opt, stamps, smoke_seconds)
    if opt.prepare_shaders:
        start_prepare_shaders(out / 'SnapRecomp.exe', log)
    return out / 'SnapRecomp.exe', ((True, info) if ok else None)


def selftest(stage, man, cfg):
    """Cheap checks of an assembled build folder: a real x64 exe, the DLLs it needs (hash from the manifest), the harness, and a
    config.json that points at files that exist. Returns a list of problems."""
    probs = []
    stage = Path(stage)
    exe = stage / 'SnapRecomp.exe'
    if not exe.exists() or exe.stat().st_size < 1_000_000:
        return [f'{exe.name} is missing or too small']
    with open(exe, 'rb') as f:
        head = f.read(0x1000)
    pe = int.from_bytes(head[0x3c:0x40], 'little')
    if head[:2] != b'MZ' or head[pe:pe + 4] != b'PE\0\0' or int.from_bytes(head[pe + 4:pe + 6], 'little') != 0x8664:
        probs.append('SnapRecomp.exe is not a 64-bit Windows program')
    for d in man.get('dlls', []):
        if d.get('path', 'bin/').startswith('bin/'):
            f = stage / d['name']
            if not f.exists():
                probs.append(f'{d["name"]} is missing')
            elif d.get('sha256') and sha256_file(f) != d['sha256']:
                probs.append(f'{d["name"]} does not match the manifest hash')
    h = man.get('harness')
    if h and not (stage / h).exists():
        probs.append(f'{h} is missing')
    try:
        c = json.loads(Path(cfg).read_text(encoding='utf-8'))
    except ValueError:
        return probs + ['config.json is not valid JSON']
    for k in ('romfs_root', 'main_elf', 'sdk_elf'):
        if not c.get(k) or not Path(c[k]).exists():
            probs.append(f'config.json {k} points at a missing path')
    if not (stage / 'launcher_data.json').exists():
        probs.append('launcher_data.json is missing')
    return probs


def _replace_retry(src, dst, tries=15, delay=1.0):
    """os.replace that waits a little for a just-stopped process (or a virus scanner) to let go of the folder."""
    for i in range(tries):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(delay)


def move_keys(work, inst=None):
    """config.json keys for a moved install: the exe compares its own place with install_root + exe_rel and moves the stored paths along
    (host/portable.cpp snap_config_rebase). install_root is the install folder when the work folder is inside it, else the work folder's
    parent (so a work folder moved on its own still starts); {} when the work folder sits in a drive's root."""
    wr = Path(work).resolve()
    inst = paths.install_root() if inst is None else inst
    inst = Path(inst).resolve() if inst is not None else None
    if inst is None or inst not in wr.parents:
        inst = wr.parent if wr.parent.parent != wr.parent else None
    if inst is None:
        return {}
    return {'install_root': str(inst), 'exe_rel': str((wr / 'out').relative_to(inst))}


def leave_folder(folder, to):
    """Windows does not rename a folder that a process has as its working directory. The game runs with out/ as its working
    directory and the updater it starts inherits that, as does every process the updater starts, so the switch failed with "used by
    another process" (2026-11-18, the first full update of a one-exe install). Step out to `to` first. True when the directory changed."""
    try:
        cwd, f = Path(os.getcwd()).resolve(), Path(folder).resolve()
        if cwd == f or f in cwd.parents:
            os.chdir(str(to))
            return True
    except OSError:
        pass
    return False


def promote(work, stage, build_id, log):
    """Switch out/ to the new build: the old out/ becomes builds/previous (one older build is kept), current.json records the id.
    Both moves are renames on one volume. A running game from out/ blocks the rename: the message says so."""
    work, stage = Path(work), Path(stage)
    out, builds = work / 'out', work / 'builds'
    prev = builds / 'previous'
    moved = []
    leave_folder(out, work)
    try:
        if out.exists():
            # portable mode: portable.txt and portable_data go to the new build first (renames), so the player's data is never left in prev,
            # which the next update deletes
            moved = userdata.carry_portable(out, stage)
            safedel.rmtree(prev, ignore_errors=True)
            _replace_retry(out, prev)
        _replace_retry(stage, out)
        # Portable by default (release standard 2): every build keeps the player's data next to the exe. The file is written only after the
        # switch, so the smoke run of a staged build never copies the player's data into a folder that may be thrown away. The game copies
        # an existing %APPDATA%\SnapRecomp once on its first portable start (host/portable.cpp snap_portable_adopt).
        flag = out / userdata.PORTABLE_FLAG
        if not flag.exists():
            try:
                flag.write_text('Portable mode: saves, settings, mods, caches and logs stay in portable_data next to this file.\n', encoding='utf-8')
            except OSError:
                pass   # a read-only folder: the game keeps the per-user folders (host/portable.cpp falls back the same way)
    except OSError as e:
        if not out.exists() and prev.exists():   # put the old build back
            os.replace(prev, out)
        if moved and out.exists():
            userdata.carry_back(out, stage, moved)
        raise StepError(f'could not switch to the new build ({e}). Close the game and the folder {out} if they are open and run the builder again; '
                        f'the new build is in {stage}.')
    for d in builds.iterdir():   # nothing but the previous build stays
        if d.name not in KEEP_BUILDS and d.is_dir():
            safedel.rmtree(d, ignore_errors=True)
    (work / 'current.json').write_text(json.dumps({'build': build_id, 'out': str(out), 'previous': str(prev) if prev.exists() else None,
                                                   'switched': time.strftime('%Y-%m-%d %H:%M:%S')}, indent=1), encoding='utf-8')
    log.write(f'switched {out} to build {build_id}' + (f' (the earlier build is in {prev})' if prev.exists() else ''))


def check_maps(maps_dir, table, log):
    """Row counts against the published check (versions.json "maps"), when the table has one."""
    exp = table.get('maps') or {}
    for name, key in (('functions_final.tsv', 'functions_final_rows'), ('jump_tables.tsv', 'jump_tables_rows')):
        n = sum(1 for line in open(Path(maps_dir) / name, encoding='utf-8') if line.strip() and not line.startswith('#'))
        want = exp.get(key)
        log.write(f'maps: {name} has {n} rows' + (f' (expected {want})' if want else ''))
        if want and n != want:
            raise StepError(f'{name} has {n} rows but {want} are expected for game version {vc.EXPECTED_NAME}: the function discovery '
                            f'result differs (other Ghidra or loader version?). The generated code would differ from the reference.')


# ------------------------------------------------------------------------------------------------ smoke run

def start_prepare_shaders(exe, log):
    """Start `SnapRecomp.exe --prepare-shaders` (host/shaderprep.cpp) as its own windowless, below-normal process and return at once: it plays one
    hidden ride per course (about 30 to 40 minutes) to fill this PC's shader cache, and the launcher's shader chip shows its progress. The cache is
    made here, on the player's PC; nothing of it is part of the build."""
    try:
        subprocess.Popen([str(exe), '--prepare-shaders'], cwd=str(Path(exe).parent), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=BELOW_NORMAL | CREATE_NO_WINDOW | 0x8, close_fds=True)   # 0x8 = DETACHED_PROCESS
        log.write('shader preparation started in the background (the launcher shows its progress)')
    except OSError as e:
        log.write(f'could not start the shader preparation: {e}. Press the shader chip in the launcher to run it.')


def smoke(opt, exe, cfg, work, table, log):
    """Start the exe on a scratch profile, wait for the title screen marker, stop it. Returns (reached, message)."""
    sm = work / 'smoke'
    safedel.rmtree(sm / 'localappdata', ignore_errors=True)   # a fresh %LOCALAPPDATA%: the first launch must translate shaders itself
    for sub in ('save', 'mods', 'shadercache', 'localappdata', 'appdata'):
        (sm / sub).mkdir(parents=True, exist_ok=True)
    trace = sm / 'trace.log'
    trace.unlink(missing_ok=True)
    env = dict(os.environ, SNAP_LAUNCHER_AUTOPLAY='1', SNAP_BOOT_TRACE=str(trace), SNAP_SAVE_DIR=str(sm / 'save'), SNAP_MODS_DIR=str(sm / 'mods'),
               SNAP_SHADERCACHE_DIR=str(sm / 'shadercache'), LOCALAPPDATA=str(sm / 'localappdata'), APPDATA=str(sm / 'appdata'),
               SNAP_AUDIO_BACKEND='null', SNAP_HLE_TRACE='open',   # open traces carry the title-screen marker
               SNAP_WINDOW_PAUSE='0')   # the setup window or another app is in front during the check; the game must not freeze on focus loss
    for k in ('NVN_CACHE_DIR', 'NVN_SHADER_HARNESS', 'SNAP_HLE_CONFIG'):
        env.pop(k, None)   # the launcher must find everything itself: config.json and shader_harness\ next to the exe
    if opt.harness:
        env['NVN_SHADER_HARNESS'] = str(Path(opt.harness).resolve())
    elif not (exe.parent / 'shader_harness' / 'ShaderHarness.exe').exists():
        log.write('smoke: no shader harness next to the exe and no --harness given; graphics may not start')
    args = [str(exe)]   # launcher mode: no command-line arguments, config.json next to the exe
    if opt.ghost:   # hidden desktop launcher (developer use)
        env.update(SNAP_INPUT_BACKGROUND='1', SNAP_HEADLESS='1', SNAP_WINDOW_PAUSE='0')
        args = [os.environ.get('PYTHON_FOR_GHOST', 'python'), '-I', str(opt.ghost)] + args
    log.write('smoke: starting the game (hidden and muted when --ghost is used)')
    err = open(sm / 'stderr.log', 'wb')
    p = subprocess.Popen(args, env=env, cwd=str(exe.parent), stdout=err, stderr=err, creationflags=BELOW_NORMAL | CREATE_NO_WINDOW)
    want = table.get('smoke', {}).get('title_marker_sha256')
    marker = re.compile(r'fs: open \S*?([^/\\ ]+\.drp)\b')
    t0 = time.time()
    seen_at, presents = None, 0
    reached, why = False, 'the game did not reach the title screen in time'
    try:
        while time.time() - t0 < opt.smoke_timeout:
            time.sleep(1.0)
            if trace.exists():
                text = trace.read_text(encoding='utf-8', errors='replace')
                if seen_at is None and want:
                    for m in marker.finditer(text):
                        if hashlib.sha256(m.group(1).lower().encode()).hexdigest() == want:
                            seen_at = time.time() - t0
                            break
                pm = re.findall(r'nvn: present #(\d+)', text)
                presents = int(pm[-1]) if pm else 0
                if seen_at is not None and presents >= table.get('smoke', {}).get('min_presents', 60):
                    reached, why = True, f'title screen loaded {seen_at:.0f}s after start, {presents} frames presented'
                    break
            if p.poll() is not None:
                why = f'the game exited with code {p.returncode} before the title screen (see {sm / "stderr.log"})'
                break
    finally:
        if p.poll() is None:
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        stop_processes_under(exe.parent, log)
        err.close()
    log.write(('smoke: PASS: ' if reached else 'smoke: FAIL: ') + why)
    return reached, why


def stop_processes_under(folder, log, wait_s=30):
    """Stop the processes this smoke run started from `folder` (the game, the shader harness; a hidden-desktop launcher may have
    detached them from the process tree), by PID, and wait until none is left. Windows will not rename a folder a process still holds."""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($env:SNAP_STOP_UNDER, "
          "[StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { $_.ProcessId }")
    env = dict(os.environ, SNAP_STOP_UNDER=str(Path(folder).resolve()) + os.sep)
    t0 = time.time()
    while True:
        try:
            r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps], env=env, capture_output=True, text=True, timeout=60,
                               creationflags=CREATE_NO_WINDOW)
            pids = [int(x) for x in r.stdout.split() if x.isdigit() and int(x) != os.getpid()]
        except (OSError, subprocess.SubprocessError):
            return
        if not pids:
            return
        for pid in pids:
            subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True)
        if time.time() - t0 > wait_s:
            log.write(f'smoke: processes still running from {folder}: {pids}')
            return
        time.sleep(1.0)
