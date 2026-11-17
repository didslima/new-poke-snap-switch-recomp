"""Source classification (cmake/source_manifest.txt, scripts/source_classes.py, docs/linux-port.md).

  python3 -I tests/recomp/test_source_classification.py

Needs git and Python only (no compiler, no game files). It fails when
  - a tracked C/C++ file under host/ or runtime/ is not in the manifest (a new file, or an old one nobody classified),
  - the manifest lists a file that no longer exists,
  - a file classified `shared` uses a Windows header, a Win32 API or an MSVC-only construct, directly or through the repo headers it includes,
  - a file that host/build_snap.py compiles for Windows is classified anything but `shared` or `windows`.
The first part tests those rules on synthetic data; the second applies them to the real tree.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import source_classes as sc  # noqa: E402


def clean(_path):
    return {}


def dirty(_path):
    return {'#include <windows.h>': ['runtime/x.h']}


class Rules(unittest.TestCase):
    def test_consistent(self):
        self.assertEqual(sc.problems({'a.cpp': 'shared', 'b.cpp': 'windows'}, ['a.cpp', 'b.cpp'], {'a.cpp', 'b.cpp'}, clean), [])

    def test_new_file_is_reported_with_a_suggestion(self):
        msgs = sc.problems({'a.cpp': 'shared'}, ['a.cpp', 'new.cpp'], set(), clean)
        self.assertEqual(len(msgs), 1)
        self.assertIn('new.cpp: not classified', msgs[0])
        self.assertIn('"shared"', msgs[0])
        msgs = sc.problems({}, ['w.cpp'], set(), dirty)
        self.assertIn('"windows"', msgs[0])

    def test_forgotten_old_file_is_reported(self):
        # an existing file that was never classified is indistinguishable from a new one: both are "not in the manifest"
        msgs = sc.problems({'a.cpp': 'shared'}, ['a.cpp', 'old.cpp'], {'a.cpp', 'old.cpp'}, clean)
        self.assertTrue(any('old.cpp: not classified' in m for m in msgs))

    def test_stale_entry_is_reported(self):
        msgs = sc.problems({'gone.cpp': 'shared'}, [], set(), clean)
        self.assertEqual(len(msgs), 1)
        self.assertIn('gone.cpp', msgs[0])

    def test_shared_file_with_windows_code_is_reported(self):
        msgs = sc.problems({'a.cpp': 'shared'}, ['a.cpp'], set(), dirty)
        self.assertEqual(len(msgs), 1)
        self.assertIn('classified shared but uses Windows-only things', msgs[0])
        self.assertIn('runtime/x.h', msgs[0])

    def test_windows_file_may_use_windows_code(self):
        self.assertEqual(sc.problems({'a.cpp': 'windows'}, ['a.cpp'], {'a.cpp'}, dirty), [])

    def test_windows_build_files_cannot_be_linux_or_offline(self):
        for cls in ('linux', 'offline'):
            msgs = sc.problems({'a.cpp': cls}, ['a.cpp'], {'a.cpp'}, clean)
            self.assertEqual(len(msgs), 1, cls)
            self.assertIn('host/build_snap.py', msgs[0])

    def test_manifest_parser(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'm.txt'
            p.write_text('# comment\nshared a.cpp  # why\n\nwindows b.cpp\n')
            self.assertEqual(sc.load_manifest(p), {'a.cpp': 'shared', 'b.cpp': 'windows'})
            for text, needle in (('cloud a.cpp\n', 'unknown class'), ('shared a.cpp\nwindows a.cpp\n', 'twice'), ('shared\n', 'expected')):
                p.write_text(text)
                with self.assertRaisesRegex(ValueError, needle):
                    sc.load_manifest(p)


class Scanner(unittest.TestCase):
    """The scanner on scratch files: sc.ROOT is pointed at a temporary directory for the duration of each test."""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.saved_root = sc.ROOT
        sc.ROOT = Path(self.tmp.name).resolve()

    def tearDown(self):
        sc.ROOT = self.saved_root
        self.tmp.cleanup()

    def scan_text(self, text, extra=None):
        for n, t in (extra or {}).items():
            (sc.ROOT / n).write_text(t)
        (sc.ROOT / 'f.cpp').write_text(text)
        return sc.scan('f.cpp')

    def test_portable_file(self):
        self.assertEqual(self.scan_text('#include <vector>\nint f() { return 1; }\n'), {})

    def test_windows_header_and_api(self):
        found = self.scan_text('#include <windows.h>\nvoid f() { VirtualAlloc(0, 1, 0, 0); }\n')
        self.assertIn('#include <windows.h>', found)
        self.assertIn('VirtualAlloc', found)

    def test_comments_do_not_count(self):
        self.assertEqual(self.scan_text('// #include <windows.h> VirtualAlloc\n/* _WIN32 */\nint x;\n'), {})

    def test_windows_code_reached_through_a_header(self):
        found = self.scan_text('#include "h.h"\n', extra={'h.h': '#ifdef _WIN32\n#endif\n'})
        self.assertEqual(list(found), ['_WIN32'])
        self.assertEqual(found['_WIN32'], ['h.h'])

    def test_longer_names_are_not_matched(self):
        self.assertEqual(self.scan_text('int MY_WIN32_FLAG; int HANDLER;\n'), {})


class RealTree(unittest.TestCase):
    def test_manifest_matches_the_tree(self):
        msgs = sc.problems(sc.load_manifest(), sc.game_sources(), sc.windows_build_set())
        self.assertEqual(msgs, [], '\n' + '\n'.join(msgs))

    def test_windows_selection_is_preserved(self):
        # host/build_snap.py is untouched; the set it compiles is rebuilt from its own code and must be fully classified
        build = sc.windows_build_set()
        self.assertGreater(len(build), 100)
        manifest = sc.load_manifest()
        self.assertEqual(sorted(p for p in build if manifest.get(p) not in ('shared', 'windows')), [])

    def test_linux_selection_has_no_windows_files(self):
        manifest = sc.load_manifest()
        linux = [p for p, c in manifest.items() if c in ('shared', 'linux')]
        self.assertTrue(linux)
        self.assertFalse([p for p in linux if manifest[p] == 'windows'])
        # and nothing in it is Windows-only by content
        self.assertEqual([p for p in linux if manifest[p] == 'shared' and sc.scan(p)], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
