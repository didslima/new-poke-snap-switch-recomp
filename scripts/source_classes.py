#!/usr/bin/env python3
"""Source classification for the cross-platform build (docs/linux-port.md).

Every C/C++ file of the game (host/, runtime/) is listed in cmake/source_manifest.txt with one class:

  shared   portable as written: no Windows header, Win32 API or compiler-specific construct (checked by scan() below)
  windows  needs Windows today (a Win32 header or API, a `_WIN32` branch, MSVC-only syntax). When a Linux implementation
           lands, the file moves to `shared` (if the branches are complete) or gets a `linux` sibling
  linux    needs Linux (SDL3, POSIX, ELF ...). None yet
  offline  not part of any game target: spikes, unit-test sources and reference code that live under host/ or runtime/

A file that is not listed, or a listed file that no longer exists, fails tests/recomp/test_source_classification.py.

    python3 -I scripts/source_classes.py            # check the manifest against the tree; new files get a suggested class
    python3 -I scripts/source_classes.py --scan F   # show what the scanner finds in file F
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'cmake' / 'source_manifest.txt'
CLASSES = ('shared', 'windows', 'linux', 'offline')
SUFFIXES = ('.cpp', '.c', '.cc')
SCOPES = ('host', 'runtime')   # the directories the game binary is built from

# Windows-only headers (a file that includes one of these is not portable as written).
_WIN_HEADERS = (
    'windows', 'winsock2', 'ws2tcpip', 'winternl', 'winioctl', 'winhttp', 'wincodec', 'wincrypt', 'wingdi', 'winuser',
    'shlobj', 'shlobj_core', 'shellapi', 'shlwapi', 'objbase', 'ole2', 'oleauto', 'comdef', 'combaseapi', 'initguid',
    'mmdeviceapi', 'audioclient', 'audiopolicy', 'avrt', 'endpointvolume', 'functiondiscoverykeys_devpkey', 'propkey',
    'psapi', 'dbghelp', 'tlhelp32', 'bcrypt', 'versionhelpers', 'xinput', 'timeapi', 'mmsystem', 'powrprof', 'knownfolders',
    'processthreadsapi', 'memoryapi', 'fileapi', 'synchapi', 'setupapi', 'hidsdi', 'dwmapi', 'commctrl', 'commdlg',
    'wrl', 'dxgi', 'dxgi1_2', 'dxgi1_4', 'dxgi1_5', 'dxgi1_6', 'd3d11', 'd3d12', 'd3dcompiler', 'd3dkmthk',
    'sapi', 'spatialaudioclient', 'conio', 'process', 'io', 'direct', 'intrin', 'excpt', 'tchar', 'aclapi',
    'sddl', 'iphlpapi', 'netioapi', 'ntddk', 'ntstatus', 'winnt', 'winbase', 'imm', 'vulkan_win32',
)
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
# Win32 / MSVC identifiers and constructs. `\b` keeps them from matching inside longer names.
_API_RE = re.compile(
    r'\b(HANDLE|HWND|HMODULE|HINSTANCE|HRESULT|DWORD|DWORD64|WORD|LPVOID|LPCSTR|LPCWSTR|PCSTR|PWSTR|WINAPI|CALLBACK|BOOL|'
    r'VirtualAlloc|VirtualQuery|VirtualProtect|VirtualFree|CreateFileW?|CreateFileA|CreateThread|CreateEventW?|CreateMutexW?|'
    r'CreateFiberEx|SwitchToFiber|ConvertThreadToFiber\w*|DeleteFiber|WaitForSingleObject|WaitForMultipleObjects|'
    r'GetModuleHandle\w*|LoadLibrary\w*|GetProcAddress|FreeLibrary|SetThreadPriority|GetCurrentThread\w*|GetCurrentProcess\w*|'
    r'GetLastError|OutputDebugString\w*|GetTickCount\w*|QueryPerformanceCounter|QueryPerformanceFrequency|'
    r'GetEnvironmentVariable\w*|SetEnvironmentVariable\w*|MultiByteToWideChar|WideCharToMultiByte|SHGetKnownFolderPath|'
    r'CoInitialize\w*|CoCreateInstance|ExitProcess|TerminateProcess|InterlockedIncrement\w*|InterlockedCompareExchange\w*|'
    r'_aligned_malloc|_aligned_free|_stricmp|_strnicmp|_wfopen|_wgetenv|_wmkdir|_mkdir|_wstat\w*|_fileno|_setmode|'
    r'__cpuid\w*|__debugbreak|__try|__except|__declspec|__forceinline|__stdcall|__fastcall|'
    r'_WIN32|_WIN64|_MSC_VER|WIN32_LEAN_AND_MEAN|NOMINMAX|VK_USE_PLATFORM_WIN32_KHR)\b|#\s*pragma\s+comment')


def strip_comments(text):
    """Removes // and /* */ comments (not string-aware; good enough to keep prose mentions of Win32 out of the scan)."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


# Where `#include "x.h"` is looked up (the -I list of host/build_snap.py, minus the SDK directories), after the including file's own directory.
_INCLUDE_ROOTS = ('runtime', 'host', 'runtime/nvn', 'runtime/ui', 'runtime/ui/third_party/imgui')


# Reviewed platform switches: a header whose `_WIN32` branch is the abstraction itself, with a working other side, so the listed
# tokens do not make the files that include it Windows-only. Add an entry only after reading both sides of the branch.
_REVIEWED_SWITCHES = {
    'runtime/simd.h': {'_WIN32', '__declspec'},   # SIMD_SELECTANY: __declspec(selectany) on Windows, __attribute__((weak)) elsewhere
}


def _vendored(path):
    return 'third_party' in Path(path).parts


def _direct(path):
    """{token: count} of the Windows-only things `path` itself mentions (comments excluded)."""
    text = strip_comments(Path(path).read_text(encoding='utf-8', errors='replace'))
    found = {}
    for m in _INCLUDE_RE.finditer(text):
        inc = m.group(1)
        base = inc.rsplit('/', 1)[-1] if inc.split('/')[0] != 'wrl' else inc
        stem = base[:-2] if base.endswith('.h') else base
        if stem.lower() in _WIN_HEADERS:
            found['#include <' + inc + '>'] = found.get('#include <' + inc + '>', 0) + 1
    for m in _API_RE.finditer(text):
        name = 'pragma comment' if m.group(0).startswith('#') else m.group(0)
        found[name] = found.get(name, 0) + 1
    return found


def _resolve(including, inc):
    for base in (Path(including).parent, *(ROOT / r for r in _INCLUDE_ROOTS)):
        cand = (base / inc)
        if cand.is_file():
            return cand.resolve()
    return None


def scan(path):
    """-> {token: [repo-relative files that contain it]}: everything Windows-only that compiling `path` pulls in, following
    `#include "..."` into the repo's own headers. Third-party (vendored) files are not scanned: their `_WIN32`/`_MSC_VER`
    branches are the library's own portability code, and their Windows backends are classified by name in the manifest."""
    path = Path(path)
    if not path.is_absolute():
        path = ROOT / path
    out, seen, todo = {}, set(), [path.resolve()]
    while todo:
        f = todo.pop()
        if f in seen or _vendored(f):
            continue
        seen.add(f)
        rel = f.relative_to(ROOT).as_posix()
        for tok in _direct(f):
            if tok not in _REVIEWED_SWITCHES.get(rel, ()):
                out.setdefault(tok, []).append(rel)
        text = strip_comments(f.read_text(encoding='utf-8', errors='replace'))
        for m in _INCLUDE_RE.finditer(text):
            dep = _resolve(f, m.group(1))
            if dep is not None and ROOT in dep.parents:
                todo.append(dep)
    return out


def is_portable(path):
    return not scan(path)


def game_sources():
    """Every tracked C/C++ file under the game's source directories, as sorted repo-relative posix paths.
    Uses git (tracked files only), so build output and untracked scratch files never count."""
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', *SCOPES], capture_output=True, text=True, check=True).stdout
    return sorted(p for p in out.splitlines() if p.endswith(SUFFIXES))


def load_manifest(path=MANIFEST):
    """-> {path: class}. Raises ValueError on a malformed line, an unknown class or a duplicate path."""
    entries = {}
    for n, raw in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            raise ValueError(f'{path}:{n}: expected "<class> <path>", got {raw!r}')
        cls, rel = parts
        if cls not in CLASSES:
            raise ValueError(f'{path}:{n}: unknown class {cls!r} (one of {", ".join(CLASSES)})')
        if rel in entries:
            raise ValueError(f'{path}:{n}: {rel} is listed twice')
        entries[rel] = cls
    return entries


def windows_build_set():
    """The files host/build_snap.py compiles for the Windows game, rebuilt from the way that script selects them.
    host/build_snap.py is not changed by the cross-platform work; if its selection code changes shape this function raises,
    so the test that uses it fails loudly instead of checking a stale set."""
    text = (ROOT / 'host' / 'build_snap.py').read_text(encoding='utf-8')
    m = re.search(r"srcs = \[\(ROOT / 'host' / f, 17\) for f in \(([^)]*)\)\]", text)
    if not m:
        raise RuntimeError('host/build_snap.py: the explicit host source tuple was not found')
    files = {f'host/{n}' for n in re.findall(r"'([^']+\.cpp)'", m.group(1))}
    globs = re.findall(r"srcs \+= \[\(p, 20\) for p in sorted\(\(ROOT / ([^)]*?)\)\.glob\('\*\.cpp'\)\)\]", text)
    dirs = set()
    for g in globs:
        dirs.add('/'.join(re.findall(r"'([^']+)'", g)))
    loop = re.search(r"for d in \(([^)]*)\):\s*\n\s*srcs \+= \[\(p, 20\) for p in sorted\(\(ROOT / 'runtime' / d\)\.glob", text)
    if loop:
        dirs |= {f'runtime/{d}' for d in re.findall(r"'([^']+)'", loop.group(1))}
    if not dirs or not loop:
        raise RuntimeError('host/build_snap.py: the directory globs were not found')
    tracked = game_sources()
    for d in dirs:
        files |= {p for p in tracked if p.startswith(d + '/') and p.count('/') == d.count('/') + 1 and p.endswith('.cpp')}
    one = re.search(r"srcs \+= \[\(ROOT / ((?:'[^']+'(?: / )?)+), 20\)\]", text)
    if one:
        files.add('/'.join(re.findall(r"'([^']+)'", one.group(1))))
    imgui = re.search(r"srcs \+= \[\(imgui / f, 17\) for f in \(([^)]*)\)\]", text)
    if not imgui:
        raise RuntimeError('host/build_snap.py: the Dear ImGui source tuple was not found')
    files |= {f'runtime/ui/third_party/imgui/{n}' for n in re.findall(r"'([^']+\.cpp)'", imgui.group(1))}
    if "'runtime' / 'fma.c'" in text:
        files.add('runtime/fma.c')
    return files


def suggest(path, scan_fn=scan):
    return 'shared' if not scan_fn(path) else 'windows'


def problems(manifest, sources, build_set, scan_fn=scan):
    """Everything wrong with a classification, as messages (empty list = consistent).
    manifest: {path: class}; sources: the tracked game sources; build_set: what the Windows build compiles."""
    out = []
    for p in sorted(set(sources) - set(manifest)):
        out.append(f'{p}: not classified (the scanner suggests "{suggest(p, scan_fn)}"); add it to cmake/source_manifest.txt')
    for p in sorted(set(manifest) - set(sources)):
        out.append(f'{p}: listed in cmake/source_manifest.txt but not a tracked game source (renamed or deleted?)')
    for p, cls in sorted(manifest.items()):
        if cls == 'shared' and p in sources:
            found = scan_fn(p)
            if found:
                what = '; '.join(f'{t} ({", ".join(sorted(set(fs)))})' for t, fs in sorted(found.items()))
                out.append(f'{p}: classified shared but uses Windows-only things: {what}')
    for p in sorted(build_set):
        cls = manifest.get(p)
        if cls not in ('shared', 'windows'):
            out.append(f'{p}: compiled by host/build_snap.py for Windows but classified "{cls}" (must be shared or windows)')
    return out


def main(argv):
    if argv[:1] == ['--scan']:
        for p in argv[1:]:
            found = scan(p)
            print(p, '->', 'portable' if not found else 'Windows-only things found:')
            for tok, files in sorted(found.items()):
                print(f'    {tok}: {", ".join(files)}')
        return 0
    msgs = problems(load_manifest(), game_sources(), windows_build_set())
    for m in msgs:
        print(m)
    print('source classification:', 'OK' if not msgs else f'{len(msgs)} problem(s)')
    return 1 if msgs else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
