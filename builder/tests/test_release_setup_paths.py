# SPDX-License-Identifier: GPL-3.0-or-later
"""release_setup.py with a relative --out: llvm-rc runs in setup/, so the output folder has to be made absolute first
(2026-11-18: `--out build\\w\\rel\\out` failed with "Error opening output file ... setup.res")."""
import os
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_setup   # noqa: E402


class RelativeOut(unittest.TestCase):
    def test_stub_only_with_a_relative_out(self):
        seen = {}

        def fake_compile(llvm_bin, out_exe, version, env):
            seen['out'] = Path(out_exe)
            Path(out_exe).write_bytes(b'MZ-stub')

        old = os.getcwd()
        with tempfile.TemporaryDirectory() as td:
            os.chdir(td)
            try:
                with unittest.mock.patch.object(release_setup, 'compile_stub', fake_compile), \
                        unittest.mock.patch.object(release_setup.ci_gate, 'gate', lambda *a, **k: None), \
                        unittest.mock.patch.object(release_setup.winsdk, 'find', lambda: None), \
                        unittest.mock.patch.object(sys, 'argv', ['release_setup.py', '--out', 'rel\\out', '--stub-only']):
                    release_setup.main()
                self.assertTrue(seen['out'].is_absolute(), seen['out'])
                self.assertEqual(seen['out'].parent, (Path(td) / 'rel' / 'out').resolve())
                self.assertEqual((Path(td) / 'rel' / 'out' / release_setup.RELEASE_EXE).read_bytes(), b'MZ-stub')
            finally:
                os.chdir(old)


if __name__ == '__main__':
    unittest.main()
