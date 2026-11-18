"""The neutral OS interface (runtime/os, shared by every platform) must stay free of Windows-only content and of names that POSIX headers
define as macros. Pure text checks: no compiler needed (the compile-time half is tests/platform/os_posix_header_test.cpp)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import source_classes as sc  # noqa: E402

# the interface headers every OS-layer file and the host include
NEUTRAL_HEADERS = ['runtime/os/os.h', 'runtime/os/os_host.h', 'runtime/os/os_internal.h', 'runtime/os/os_layout.h', 'runtime/os/perf.h', 'runtime/hle/guest_tls.h']
# macros of <unistd.h> / <fcntl.h> that an identifier of ours must not be spelled like (os_internal.h once declared `constexpr uint32_t R_OK`)
POSIX_MACROS = ('R_OK', 'W_OK', 'X_OK', 'F_OK')


class NeutralInterfaces(unittest.TestCase):
    def test_interface_headers_have_no_windows_content(self):
        for h in NEUTRAL_HEADERS:
            self.assertEqual(sc.scan(h), {}, h)

    def test_no_identifier_collides_with_a_posix_macro(self):
        pat = re.compile(r'\b(' + '|'.join(POSIX_MACROS) + r')\b')
        bad = []
        for f in sorted((ROOT / 'runtime' / 'os').glob('*')):
            if f.suffix not in ('.h', '.cpp', '.c'):
                continue
            for n, line in enumerate(sc.strip_comments(f.read_text(encoding='utf-8', errors='replace')).splitlines(), 1):
                if pat.search(line):
                    bad.append(f'{f.relative_to(ROOT).as_posix()}:{n}: {line.strip()}')
        self.assertEqual(bad, [], '\n' + '\n'.join(bad))

    def test_the_success_code_is_declared_once(self):
        text = sc.strip_comments((ROOT / 'runtime/os/os_internal.h').read_text(encoding='utf-8'))
        self.assertEqual(len(re.findall(r'\bR_SUCCESS\s*=\s*0\b', text)), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
