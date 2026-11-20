# SPDX-License-Identifier: GPL-3.0-or-later
"""The updater with a one-exe release: the asset is SnapRecomp.exe plus SnapRecomp.exe.sig, the builder is the zip at the end of the
exe, and after the rebuild passes the exe the player starts is replaced while it runs."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapbuild import ed25519, paths, update   # noqa: E402
from test_update import PUB, SEED, Quiet, Rig   # noqa: E402


def make_exe_release(rig, version='1.0.1', stub=b'MZ-new-top-exe' + b'\0' * 64, trailer=True, wrong_ver_in_zip=None, with_zip_too=False,
                     prerelease=False, doc_name='release.json'):
    """A release as release_setup.py makes it: program bytes, the builder zip, its size and the mark. Returns the release document's URL."""
    files = {'builder/snap_builder.py': b'print("new")', 'builder/VERSION': (wrong_ver_in_zip or version).encode(),
             'builder/update_pubkey.txt': PUB.hex().encode(), 'prebuilt/manifest.json': b'{}', 'python/python.exe': b'MZ-py'}
    zp = rig.rel / 'payload.zip'
    with zipfile.ZipFile(zp, 'w') as z:
        for n, b in files.items():
            z.writestr(n, b)
    payload = zp.read_bytes()
    zp.unlink()
    exe = rig.rel / 'SnapRecomp.exe'
    exe.write_bytes(stub + payload + (struct.pack('<Q', len(payload)) + b'SNPSET01' if trailer else b''))
    digest, size = update.sha256_of(exe), exe.stat().st_size
    sig = ed25519.sign(SEED, update.signed_message(version, digest, size))
    sigp = rig.rel / 'SnapRecomp.exe.sig'
    sigp.write_text(json.dumps({'format': 1, 'version': version, 'sha256': digest, 'size': size, 'signature': sig.hex()}))
    assets = [{'name': 'SnapRecomp.exe', 'size': size, 'browser_download_url': exe.as_uri()},
              {'name': 'SnapRecomp.exe.sig', 'size': sigp.stat().st_size, 'browser_download_url': sigp.as_uri()}]
    if with_zip_too:
        n = f'SnapRecomp-Builder-v{version}-win-x64.zip'
        assets += [{'name': n, 'size': 10, 'browser_download_url': 'https://example.invalid/z'}, {'name': n + '.sig', 'size': 10, 'browser_download_url': 'https://example.invalid/s'}]
    doc = {'tag_name': 'v' + version, 'body': 'One exe.', 'draft': False, 'prerelease': prerelease, 'html_url': 'https://example.invalid/r', 'assets': assets}
    j = rig.rel / doc_name
    j.write_text(json.dumps(doc))
    return j.as_uri()


class OneExeRelease(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.addCleanup(self.td.cleanup)
        self.rig = Rig(self.td.name)
        self.top = self.rig.t / 'SnapRecomp.exe'          # the exe the player starts
        self.top.write_bytes(b'MZ-old-top-exe')

    def apply(self, url, run=lambda cmd, log: 0, **kw):
        return update.apply(self.rig.builder, self.rig.work, self.rig.state, url=url, log=Quiet(), run=run, data_root=self.rig.user, **kw)

    def test_release_with_the_exe_is_read(self):
        r = update.parse_release(json.loads(Path(self.rig.rel / 'release.json').read_text()) if make_exe_release(self.rig) else None)
        self.assertEqual((r.version, r.zip_name), ('1.0.1', 'SnapRecomp.exe'))
        self.assertTrue(r.sig_url.endswith('SnapRecomp.exe.sig'))

    def test_the_exe_wins_when_an_old_style_zip_is_also_attached(self):
        make_exe_release(self.rig, with_zip_too=True)
        r = update.parse_release(json.loads((self.rig.rel / 'release.json').read_text()))
        self.assertEqual(r.zip_name, 'SnapRecomp.exe')

    def test_a_release_with_neither_is_refused(self):
        with self.assertRaises(update.UpdateError) as c:
            update.parse_release({'tag_name': 'v1.0.1', 'assets': [{'name': 'notes.txt', 'size': 3}]})
        self.assertIn('SnapRecomp.exe', str(c.exception))

    def test_update_rebuilds_and_then_replaces_the_exe_the_player_starts(self):
        url = make_exe_release(self.rig)
        new_bytes = (self.rig.rel / 'SnapRecomp.exe').read_bytes()
        seen = {}

        def run(cmd, log):
            seen['top_during_build'] = self.top.read_bytes()   # not touched until the build has passed
            seen['cmd'] = cmd
            return 0
        before_user = self.rig.user_hash()
        r = self.apply(url, run, top=self.top)
        self.assertTrue(r['updated'] and r['exe_replaced'])
        self.assertEqual(seen['top_during_build'], b'MZ-old-top-exe')
        self.assertEqual(self.top.read_bytes(), new_bytes)
        self.assertEqual(self.top.with_name('SnapRecomp.exe.old').read_bytes(), b'MZ-old-top-exe')
        vdir = self.rig.state / 'versions' / '1.0.1'
        self.assertEqual((vdir / 'builder' / 'VERSION').read_text(), '1.0.1')
        self.assertTrue((vdir / 'python' / 'python.exe').is_file())          # the payload came out of the exe
        self.assertIn(str(vdir / 'builder' / 'snap_builder.py'), seen['cmd'])
        self.assertFalse((self.rig.state / 'downloads').exists())             # nothing left behind
        self.assertEqual(self.rig.user_hash(), before_user)

    def test_rollback_puts_the_old_exe_back_and_a_second_rollback_the_new_one(self):
        url = make_exe_release(self.rig)
        new_bytes = (self.rig.rel / 'SnapRecomp.exe').read_bytes()

        def run(cmd, log):   # what the real rebuild does at its end: the old build moves to builds\previous, the new one becomes out\
            out, prev = self.rig.work / 'out', self.rig.work / 'builds' / 'previous'
            for d, exe, ver in ((prev, b'MZ-old-game', '1.0.0'), (out, b'MZ-new-game', '1.0.1')):
                d.mkdir(parents=True, exist_ok=True)
                (d / 'SnapRecomp.exe').write_bytes(exe)
                (d / 'config.json').write_text(json.dumps({'builder_version': ver}))
            return 0
        r = self.apply(url, run, top=self.top)
        self.assertTrue(r['exe_replaced'])
        self.assertEqual(self.top.read_bytes(), new_bytes)
        r = update.rollback(self.rig.work, self.rig.state, Quiet(), data_root=self.rig.user)
        self.assertTrue(r['exe_restored'])
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')                      # the exe matches the build that is active again
        self.assertEqual((self.rig.work / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-game')
        r = update.rollback(self.rig.work, self.rig.state, Quiet(), data_root=self.rig.user)
        self.assertTrue(r['exe_restored'])
        self.assertEqual(self.top.read_bytes(), new_bytes)                              # and forward again
        self.assertEqual((self.rig.work / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-new-game')

    def test_failed_rebuild_leaves_the_exe_and_the_build_alone(self):
        url = make_exe_release(self.rig)
        with self.assertRaises(update.UpdateError):
            self.apply(url, lambda cmd, log: 3, top=self.top)
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')
        self.assertFalse(self.top.with_name('SnapRecomp.exe.old').exists())
        self.assertFalse((self.rig.state / 'versions' / '1.0.1').exists())
        self.assertFalse(list((self.rig.state / 'downloads').glob('*')) if (self.rig.state / 'downloads').exists() else [])

    def test_game_started_directly_updates_the_build_and_says_the_exe_was_not_replaced(self):
        url = make_exe_release(self.rig)
        with unittest.mock.patch.dict(os.environ):
            os.environ.pop('SNAP_TOP_EXE', None)
            r = self.apply(url)
        self.assertTrue(r['updated'])
        self.assertIs(r['exe_replaced'], False)
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')

    def test_the_top_exe_is_found_from_the_environment_the_exe_sets(self):
        url = make_exe_release(self.rig)
        with unittest.mock.patch.dict(os.environ, {'SNAP_TOP_EXE': str(self.top)}):
            r = self.apply(url)
        self.assertTrue(r['exe_replaced'])
        self.assertNotEqual(self.top.read_bytes(), b'MZ-old-top-exe')

    def test_exe_without_the_payload_mark_is_refused_and_nothing_changes(self):
        url = make_exe_release(self.rig, trailer=False)
        with self.assertRaises(update.UpdateError) as c:
            self.apply(url, top=self.top)
        self.assertIn('payload', str(c.exception))
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')
        self.assertEqual((self.rig.work / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-exe')

    def test_payload_with_another_version_inside_is_refused(self):
        url = make_exe_release(self.rig, wrong_ver_in_zip='9.9.9')
        with self.assertRaises(update.UpdateError):
            self.apply(url, top=self.top)
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')

    def test_tampered_exe_fails_the_signature(self):
        url = make_exe_release(self.rig)
        e = self.rig.rel / 'SnapRecomp.exe'
        b = bytearray(e.read_bytes())
        b[3] ^= 1                                  # same size, one bit changed in the program part
        e.write_bytes(bytes(b))
        with self.assertRaises(update.UpdateError) as c:
            self.apply(url, top=self.top)
        self.assertIn('signature', str(c.exception))
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')

    def test_exe_that_cannot_be_moved_aside_is_put_back(self):
        new = self.rig.t / 'new.exe'
        new.write_bytes(b'MZ-new')
        log = Quiet()
        with open(self.top, 'rb'):                 # an open handle without delete sharing: the rename is refused
            ok = update.replace_top_exe(new, self.top, log)
        self.assertFalse(ok)
        self.assertEqual(self.top.read_bytes(), b'MZ-old-top-exe')
        self.assertFalse(self.top.with_name('SnapRecomp.exe.new').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows only')
    def test_a_running_exe_is_replaced(self):
        ping = Path(os.environ.get('SystemRoot', 'C:\\Windows')) / 'System32' / 'PING.EXE'
        top = self.rig.t / 'run' / 'SnapRecomp.exe'
        top.parent.mkdir()
        top.write_bytes(ping.read_bytes())
        new = self.rig.t / 'new.exe'
        new.write_bytes(b'MZ-new-version')
        p = subprocess.Popen([str(top), '-n', '20', '127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x08000000)
        try:
            time.sleep(0.5)
            self.assertIsNone(p.poll())
            self.assertTrue(update.replace_top_exe(new, top, Quiet()))
            self.assertEqual(top.read_bytes(), b'MZ-new-version')
            self.assertEqual(top.with_name('SnapRecomp.exe.old').read_bytes(), ping.read_bytes())
            self.assertIsNone(p.poll())            # the running program was not disturbed
        finally:
            subprocess.run(['taskkill', '/PID', str(p.pid), '/F'], capture_output=True, creationflags=0x08000000)
            p.wait(timeout=10)

    def test_updated_builder_still_knows_the_install_folder(self):
        root = self.rig.t / 'Snap'
        b = root / 'work' / 'updates' / 'versions' / '1.0.1' / 'builder'
        env = {'LOCALAPPDATA': str(self.rig.t / 'local')}
        self.assertEqual(paths.install_root(env, b), root)
        self.assertEqual(paths.tools_dir(env, b), root / 'tools')
        legacy = self.rig.t / 'local' / 'SnapRecomp' / 'updates' / 'versions' / '1.0.1' / 'builder'
        self.assertIsNone(paths.install_root(env, legacy))   # the per-user updates folder of an older install is not an install folder


if __name__ == '__main__':
    import unittest.mock   # noqa: F401
    unittest.main()
