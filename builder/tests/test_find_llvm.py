"""find_llvm must skip a clang that is not built for windows-msvc (llvm-mingw on PATH linked in GNU mode and broke the build, 2026-11-19)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import toolchain  # noqa: E402
from snapbuild.util import StepError  # noqa: E402


def fake_llvm(folder):
    folder.mkdir(parents=True)
    for n in ('clang.exe', 'clang++.exe', 'llvm-ar.exe', 'lld-link.exe'):
        (folder / n).write_bytes(b'')
    return folder


class FindLlvm(unittest.TestCase):
    def run_with(self, triples, on_path, root, env=None):
        def fake_run(cmd, **kw):
            folder = str(Path(cmd[0]).parent)
            return mock.Mock(stdout=triples.get(folder, '') + '\n')
        with mock.patch.object(toolchain.subprocess, 'run', fake_run), \
                mock.patch.object(toolchain.shutil, 'which', lambda n: str(Path(on_path) / 'clang.exe') if on_path else None), \
                mock.patch.object(toolchain, 'managed', lambda: Path(root) / 'nomanaged'), \
                mock.patch.dict(os.environ, env or {}, clear=False):
            os.environ.pop('SNAP_LLVM_BIN', None)
            return toolchain.find_llvm(None, root)

    def test_mingw_on_path_is_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            mingw = fake_llvm(d / 'mingw' / 'bin')
            ours = fake_llvm(d / 'tools' / 'llvm' / 'bin')
            got = self.run_with({str(mingw): 'x86_64-w64-windows-gnu', str(ours): 'x86_64-pc-windows-msvc'}, mingw, d)
            self.assertEqual(got, ours)

    def test_only_mingw_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            mingw = fake_llvm(d / 'mingw' / 'bin')
            with self.assertRaises(StepError):
                self.run_with({str(mingw): 'x86_64-w64-windows-gnu'}, mingw, d)


if __name__ == '__main__':
    unittest.main()
