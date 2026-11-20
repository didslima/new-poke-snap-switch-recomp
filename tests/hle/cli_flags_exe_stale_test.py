"""The built-exe flag test refuses an exe older than its sources (tests/hle/cli_flags_exe_test.py stale_against).

Usage: python -I tests/hle/cli_flags_exe_stale_test.py
"""
import importlib.util
import os
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('cli_flags_exe_test', Path(__file__).with_name('cli_flags_exe_test.py'))
t = importlib.util.module_from_spec(spec)
sys.argv = sys.argv[:1]
spec.loader.exec_module(t)


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'host').mkdir()
        exe, a, b = d / 'snap_boot.exe', d / 'host' / 'a.h', d / 'host' / 'b.cpp'
        for f in (exe, a, b):
            f.write_bytes(b'x')
        os.utime(a, (1000, 1000))
        os.utime(b, (1000, 1000))
        os.utime(exe, (2000, 2000))
        checks = [('an exe newer than every source is current', t.stale_against(exe, ('host/a.h', 'host/b.cpp'), d) is None)]
        os.utime(b, (3000, 3000))
        checks.append(('an exe older than one source names that source', t.stale_against(exe, ('host/a.h', 'host/b.cpp'), d) == 'host/b.cpp'))
        checks.append(('a source that is not there is skipped', t.stale_against(exe, ('host/gone.h',), d) is None))
        checks.append(('the list covers the flag parser and the launcher actions', {'host/cli_flags.h', 'host/launcher.cpp'} <= set(t.SOURCES)))
    for label, ok in checks:
        print('%s %s' % ('ok  ' if ok else 'FAIL', label))
        fails += not ok
    print('cli flags exe stale test:', 'PASS' if not fails else '%d FAILED' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
