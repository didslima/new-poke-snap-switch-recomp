"""Finding the tools the build needs and the preflight checks. Download and verify by pinned URL is a later milestone:
for now every tool is a local path (a flag, an environment variable, or a standard install location)."""
import ctypes
import os
import platform
import shutil
import subprocess
from pathlib import Path

from .util import StepError, cpu_count, total_ram_gb


def managed():
    """The folder `snap_builder.py fetch` and `pyenv` unpack into (checked after the repo's own tools folder)."""
    from .fetch import default_tools_dir
    return default_tools_dir()


def _first_existing(cands):
    for c in cands:
        if c and Path(c).exists():
            return Path(c)
    return None


def _msvc_target(c):
    """True when this clang targets windows-msvc. A llvm-mingw clang on PATH has the same file names (clang, clang++, llvm-ar, ld.lld) but links in
    GNU mode and fails on the MSVC objects ("could not open 'liblibcpmt.a'"), so it must never be taken for the build's compiler."""
    try:
        out = subprocess.run([str(Path(c) / 'clang.exe'), '-print-target-triple'], capture_output=True, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return False
    return out.endswith('windows-msvc')


def find_llvm(arg, root):
    """Folder holding clang.exe, clang++.exe, lld-link or ld.lld, llvm-ar.exe (and llvm-rc.exe), built for the windows-msvc target.
    The folders this builder fetches or ships come before whatever clang is on PATH."""
    cands = [arg, os.environ.get('SNAP_LLVM_BIN'), managed() / 'llvm' / 'bin', Path(root) / 'tools' / 'llvm' / 'bin']
    on_path = shutil.which('clang.exe')
    cands += [Path(on_path).parent if on_path else None]
    for c in cands:
        if c and all((Path(c) / n).exists() for n in ('clang.exe', 'clang++.exe', 'llvm-ar.exe'))                 and ((Path(c) / 'lld-link.exe').exists() or (Path(c) / 'ld.lld.exe').exists()) and _msvc_target(c):
            return Path(c)
    raise StepError('clang with lld not found. Install LLVM (clang, clang++, lld, llvm-ar) for the windows-msvc target (not llvm-mingw) and pass its bin folder with --llvm, '
                    'or set SNAP_LLVM_BIN, or run: python builder/snap_builder.py fetch llvm')


def clang_target(llvm):
    """The compiler's target triple; part of the compile cache keys, so objects made by a different compiler are never reused."""
    return subprocess.run([str(Path(llvm) / 'clang.exe'), '-print-target-triple'], capture_output=True, text=True).stdout.strip()


def clang_version(llvm):
    out = subprocess.run([str(Path(llvm) / 'clang.exe'), '--version'], capture_output=True, text=True).stdout
    for tok in out.split():
        if tok[:1].isdigit() and '.' in tok:
            return tok
    return out.splitlines()[0] if out else 'unknown'


def find_vcvars():
    """vcvars64.bat of Visual Studio Build Tools (Windows SDK and MSVC libraries are needed to link)."""
    pf86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = Path(pf86) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if vswhere.exists():
        r = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                            '-property', 'installationPath', '-utf8'], capture_output=True, encoding='utf-8')
        p = r.stdout.strip().splitlines()
        if p:
            v = Path(p[0]) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
            if v.exists():
                return v
    v = _first_existing([Path(pf86) / 'Microsoft Visual Studio' / '2022' / 'BuildTools' / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'])
    if v:
        return v
    raise StepError('Visual Studio Build Tools 2022 (C++ build tools with the Windows SDK) were not found. Install them from Microsoft '
                    '(free) and run the builder again. They provide the system libraries the final link needs.')


def vc_env(vcvars):
    env = dict(os.environ)
    # cmd /u writes UTF-16, so a non-ASCII user name survives; a failing vcvars must stop the build here, not later in the link
    r = subprocess.run(f'cmd /u /d /c ""{vcvars}" >nul && set"', capture_output=True, encoding='utf-16-le', shell=True)
    if r.returncode != 0:
        raise StepError(f'{vcvars} failed with exit code {r.returncode}; the MSVC and Windows SDK environment was not created.')
    for line in r.stdout.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def native_env(mode='auto', tools_dir=None):
    """The environment for compiling and linking the Windows exe, and a short description of where the system files come from.

    mode 'vs'   : Visual Studio Build Tools (vcvars64.bat), as before.
         'xwin' : the Microsoft C runtime and Windows SDK files downloaded by winsdk.install (no Visual Studio needed).
         'auto' : Visual Studio when it is installed, else the downloaded files.
    Returns (link_env, compile_env, description). compile_env is the plain environment for Visual Studio (clang finds it itself)."""
    from . import winsdk
    if mode not in ('auto', 'vs', 'xwin'):
        raise StepError(f'unknown --winsdk value "{mode}" (use auto, vs or xwin)')
    if mode in ('auto', 'vs'):
        try:
            vcvars = find_vcvars()
            return vc_env(vcvars), dict(os.environ), f'Visual Studio Build Tools ({vcvars})'
        except StepError:
            if mode == 'vs':
                raise
    root = winsdk.find(tools_dir)
    if root:
        env = winsdk.env_for(root)
        return env, env, f'Microsoft C runtime and Windows SDK files downloaded earlier ({root})'
    raise StepError('Neither Visual Studio Build Tools nor the downloaded Microsoft SDK files were found. '
                    'The setup program downloads them for you (it asks you to accept the Microsoft license first); '
                    'from the command line run: python builder/snap_builder.py winsdk --accept-license')


def find_python(arg, root):
    """A Python with capstone, pyelftools and numpy (the lifter needs them)."""
    cands = [arg, Path(root) / 'tools' / 'venv-recomp' / 'Scripts' / 'python.exe', Path(root) / 'tools' / 'venv' / 'Scripts' / 'python.exe',
             managed() / 'venv' / 'Scripts' / 'python.exe', managed() / 'python' / 'python.exe']
    import sys
    cands.append(sys.executable)
    for c in cands:
        if c and Path(c).exists():
            r = subprocess.run([str(c), '-I', '-c', 'import capstone, elftools, numpy'], capture_output=True)
            if r.returncode == 0:
                return Path(c)
    raise StepError('no Python with capstone, pyelftools and numpy was found. Install them (pip install capstone pyelftools numpy lz4) '
                    'and pass that Python with --python, or run: python builder/snap_builder.py pyenv')


def find_vulkan_lib(arg, root, prebuilt=None):
    cands = [arg]
    if prebuilt:   # the prebuilt folder ships a small import library for the Vulkan loader: no SDK needed
        cands.append(Path(prebuilt) / 'lib' / 'vulkan-1.lib')
    if os.environ.get('VULKAN_SDK'):
        cands.append(Path(os.environ['VULKAN_SDK']) / 'Lib' / 'vulkan-1.lib')
    cands.append(Path(root) / 'tools' / 'vulkan_sdk' / 'Lib' / 'vulkan-1.lib')
    p = _first_existing(cands)
    if not p:
        raise StepError('vulkan-1.lib was not found. Install the Vulkan SDK (it sets VULKAN_SDK) or pass the file with --vulkan-lib.')
    return p


def find_ghidra(arg, root):
    cands = [arg, os.environ.get('GHIDRA_DIR')]
    cands += sorted((Path(root) / 'tools').glob('ghidra_*_PUBLIC')) + sorted(managed().glob('ghidra_*_PUBLIC'))
    for c in cands:
        if c and (Path(c) / 'support' / 'analyzeHeadless.bat').exists():
            return Path(c)
    return None


def find_jdk(arg, root):
    # Ghidra needs a JDK (javac.exe), not a JRE. A JAVA_HOME that points at a JRE is skipped, and the JDK this builder fetches comes
    # before JAVA_HOME so a stray environment variable cannot break the build.
    cands = [arg] + sorted((Path(root) / 'tools').glob('jdk-21*')) + sorted(managed().glob('jdk-21*')) + [os.environ.get('JAVA_HOME')]
    for c in cands:
        if c and (Path(c) / 'bin' / 'java.exe').exists() and (Path(c) / 'bin' / 'javac.exe').exists():
            return Path(c)
    return None


# ------------------------------------------------------------------------------------------------ preflight

def free_gb(path):
    return shutil.disk_usage(path).free / 2**30


def has_avx2():
    try:
        return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))   # PF_AVX2_INSTRUCTIONS_AVAILABLE
    except Exception:
        return True


def preflight(work, need_ghidra, log, min_free_gb=6.0):
    """Returns a list of problems (empty = go). Warnings go to the log."""
    problems = []
    if platform.system() != 'Windows' or platform.machine().lower() not in ('amd64', 'x86_64'):
        problems.append('this builder needs 64-bit Windows 10 or 11 on an x86-64 PC.')
        return problems
    if not has_avx2():
        problems.append('this PC lacks AVX2; the build targets x86-64-v3 (AVX2 and FMA) and the game would not run here.')
    Path(work).mkdir(parents=True, exist_ok=True)
    g = free_gb(work)
    if g < min_free_gb:
        problems.append(f'only {g:.1f} GB free on the work folder drive; at least {min_free_gb:.0f} GB are needed.')
    tot, avail = total_ram_gb()
    log.write(f'preflight: {cpu_count()} logical CPUs, {tot:.1f} GB RAM ({avail:.1f} GB free), {g:.1f} GB free on the work drive')
    if tot < 8:
        problems.append(f'{tot:.1f} GB of RAM is too little: code generation peaks at about 3.5 GB and compiling needs about 0.5 GB per job.')
    if need_ghidra and tot < 16:
        problems.append(f'{tot:.1f} GB of RAM is too little for the Ghidra step (12 GB heap, 16 GB minimum). '
                        f'Use --maps with ready-made function lists instead.')
    if not (Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'vulkan-1.dll').exists():
        log.write('preflight: warning: vulkan-1.dll is not in System32; the game needs a Vulkan 1.3 GPU driver to run.')
    return problems


def default_jobs():
    tot, _ = total_ram_gb()
    return max(1, min(8, cpu_count(), int(tot // 2)))
