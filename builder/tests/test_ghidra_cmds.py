"""The Ghidra wrapper builds the same command lines as scripts/ghidra_analyze.ps1 (checked without running Ghidra)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import ghidra_maps as gm  # noqa: E402


class GhidraCmdTests(unittest.TestCase):
    def test_commands(self):
        an, ex, rc = gm.build_commands(Path('G'), Path('py.exe'), Path('R'), Path('W'), 'sha', 'BID', 60)
        self.assertTrue(an[0].endswith('analyzeHeadless.bat'))
        self.assertIn('-import', an)
        self.assertIn('-analysisTimeoutPerFile', an)
        self.assertEqual(an[an.index('-analysisTimeoutPerFile') + 1], '3600')
        self.assertIn('SeedFdeStarts.java', an)
        self.assertIn('-process', ex)
        self.assertEqual(ex[-2:], ['sha', 'BID'])
        self.assertIn('ExportMaps.java', ex)
        self.assertIn('-readOnly', ex)
        self.assertEqual(rc[1], '-I')
        self.assertTrue(rc[2].endswith('reconcile_functions.py'))
        self.assertIn('--out', rc)
        # the three stages agree on one project folder and one fde file
        self.assertIn(str(gm.layout(Path('W'))['fde']), an)
        self.assertIn(str(gm.layout(Path('W'))['fde']), ex)

    def test_java_env(self):
        env = gm.java_env(Path('J'))
        self.assertEqual(env['GHIDRA_HEADLESS_MAXMEM'], '12G')
        self.assertEqual(env['JAVA_HOME'], 'J')

    def test_portable_install_keeps_ghidra_files_in_its_folder(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'Snap'
            work = root / 'work'
            builder = root / 'setup' / 'app' / '1.2.0' / 'builder'
            u = gm.user_dir_for(work, env={}, builder_dir=builder)
            self.assertEqual(u, work / 'ghidra_user')
            env = gm.java_env(Path('J'), u)
            for name in ('APPDATA', 'LOCALAPPDATA', 'TEMP', 'TMP', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
                self.assertTrue(Path(env[name]).is_dir(), name)
                self.assertEqual(Path(env[name]).parent, u, name)
            # a source checkout or an unpacked zip: Ghidra's places are left alone
            self.assertIsNone(gm.user_dir_for(work, env={}, builder_dir=Path(d) / 'repo' / 'builder'))
            plain = gm.java_env(Path('J'))
            self.assertEqual(plain.get('APPDATA'), os.environ.get('APPDATA'))
            self.assertEqual(plain.get('TEMP'), os.environ.get('TEMP'))

    def test_missing_ghidra_message(self):
        from snapbuild.util import Log, StepError
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            log = Log(Path(d) / 'l.log', quiet=True)
            with self.assertRaises(StepError) as cm:
                gm.run_ghidra(None, None, 'py', Path('R'), Path(d), 'x', log)
            log.close()
        self.assertIn('--maps', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
