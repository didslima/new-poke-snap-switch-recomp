"""The cache, the patch check and the output switch as the pipeline uses them (fake files only)."""
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import cache, nso, patches, pipeline  # noqa: E402
from snapbuild.util import Log, StepError  # noqa: E402


class Quiet(Log):
    def __init__(self, path):
        super().__init__(path, quiet=True)


class RunCachedTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.root = cache.open_root(self.t / 'cache')
        self.log = Quiet(self.t / 'log.txt')
        self.calls = 0

    def tearDown(self):
        self.log.close()

    def work(self, folder, text='data'):
        def fn():
            self.calls += 1
            Path(folder).mkdir(parents=True, exist_ok=True)
            (Path(folder) / 'a.c').write_text(text)
        return fn

    def test_miss_then_hit_in_another_work_folder(self):
        f1, f2 = self.t / 'w1' / 'gen', self.t / 'w2' / 'gen'
        hit = pipeline.run_cached(self.root, 'gen-main', 'k1', f1, [f1 / 'a.c'], self.work(f1), self.log)
        self.assertFalse(hit)
        self.assertEqual(self.calls, 1)
        hit = pipeline.run_cached(self.root, 'gen-main', 'k1', f2, [f2 / 'a.c'], self.work(f2, 'other'), self.log)
        self.assertTrue(hit)
        self.assertEqual(self.calls, 1)   # the step did not run again
        self.assertEqual((f2 / 'a.c').read_text(), 'data')

    def test_different_key_runs_again(self):
        f = self.t / 'w' / 'gen'
        pipeline.run_cached(self.root, 'gen-main', 'k1', f, [f / 'a.c'], self.work(f), self.log)
        pipeline.run_cached(self.root, 'gen-main', 'k2', f, [f / 'a.c'], self.work(f), self.log)
        self.assertEqual(self.calls, 2)

    def test_no_cache_root_just_runs(self):
        f = self.t / 'w' / 'gen'
        self.assertFalse(pipeline.run_cached(None, 'gen-main', 'k1', f, [f / 'a.c'], self.work(f), self.log))
        self.assertFalse(pipeline.run_cached(None, 'gen-main', 'k1', f, [f / 'a.c'], self.work(f), self.log))
        self.assertEqual(self.calls, 2)

    def test_entry_missing_an_output_is_not_a_hit(self):
        f = self.t / 'w' / 'gen'
        pipeline.run_cached(self.root, 'gen-main', 'k1', f, [f / 'a.c'], self.work(f), self.log)
        f2 = self.t / 'w2' / 'gen'
        hit = pipeline.run_cached(self.root, 'gen-main', 'k1', f2, [f2 / 'a.c', f2 / 'missing.c'], self.work(f2), self.log)
        self.assertFalse(hit)
        self.assertEqual(self.calls, 2)

    def test_cache_failure_does_not_fail_the_step(self):
        f = self.t / 'w' / 'gen'
        bad = self.t / 'notcache'
        bad.mkdir()
        (bad / 'x.txt').write_text('user file')   # no marker: the cache refuses it, the build goes on
        with self.assertRaises(cache.CacheError):
            cache.open_root(bad)
        # a root that disappeared after open_root: restore returns False, store fails quietly
        gone = self.t / 'gone'
        gone.mkdir()
        hit = pipeline.run_cached(gone, 'gen-main', 'k', f, [f / 'a.c'], self.work(f), self.log)
        self.assertFalse(hit)
        self.assertEqual(self.calls, 1)


def fake_nso(path, text, ro, data, build_id=b'\x11' * 20, compress=False):
    """A tiny NSO0 file with uncompressed segments."""
    segs = [text, ro, data]
    head = bytearray(0x100)
    head[:4] = b'NSO0'
    struct.pack_into('<I', head, 0xC, 0x38)   # hash flags only, no compression
    off = 0x100
    for i, s in enumerate(segs):
        struct.pack_into('<3I', head, 0x10 + 0x10 * i, off, off - 0x100, len(s))
        struct.pack_into('<I', head, 0x60 + 4 * i, len(s))
        head[0xA0 + 32 * i:0xA0 + 32 * (i + 1)] = hashlib.sha256(s).digest()
        off += len(s)
    head[0x40:0x40 + 20] = build_id
    Path(path).write_bytes(bytes(head) + b''.join(segs))


class PatchWiringTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.f = self.t / 'main'
        fake_nso(self.f, b'T' * 64, b'R' * 32, b'D' * 16)
        h = nso.read_header(self.f)
        self.img = nso.read_image(self.f, h)
        self.sha = nso.image_digest(self.f, h)
        self.images = {'main': (self.img, h.build_id, self.sha)}
        self.pd = self.t / 'patches'
        self.pd.mkdir()

    def write_patch(self, name='p1.json', **kw):
        p = {'name': 'p1', 'module': 'main', 'image_sha256': self.sha, 'build_id': self.images['main'][1], 'offset': 8,
             'original_sha256': hashlib.sha256(self.img[8:12]).hexdigest(), 'replacement_hex': 'deadbeef'}
        p.update(kw)
        (self.pd / name).write_text(json.dumps(p))

    def test_image_is_what_is_hashed(self):
        self.assertEqual(hashlib.sha256(self.img).hexdigest(), self.sha)
        self.assertEqual(len(self.img), 64 + 32 + 16)

    def test_no_patches_dir_or_empty_is_fine(self):
        self.assertEqual(patches.check_dir(self.t / 'nonexistent', self.images), '')
        self.assertEqual(patches.check_dir(self.pd, self.images), '')

    def test_reviewed_patch_passes_and_changes_the_key(self):
        self.write_patch()
        (self.pd / 'reviewed.json').write_text(json.dumps({'patches': ['p1.json']}))
        k1 = patches.check_dir(self.pd, self.images)
        self.assertTrue(k1)
        self.write_patch(replacement_hex='cafebabe')
        self.assertNotEqual(patches.check_dir(self.pd, self.images), k1)

    def test_extra_unreviewed_patch_stops(self):
        self.write_patch()
        with self.assertRaises(patches.PatchError):
            patches.check_dir(self.pd, self.images)   # no reviewed list names it

    def test_patch_for_other_image_stops(self):
        self.write_patch(image_sha256='0' * 64)
        (self.pd / 'reviewed.json').write_text(json.dumps({'patches': ['p1.json']}))
        with self.assertRaises(patches.PatchError):
            patches.check_dir(self.pd, self.images)

    def test_changed_bytes_at_the_span_stop(self):
        self.write_patch(original_sha256='1' * 64)
        (self.pd / 'reviewed.json').write_text(json.dumps({'patches': ['p1.json']}))
        with self.assertRaises(patches.PatchError):
            patches.check_dir(self.pd, self.images)

    def test_unknown_module(self):
        self.write_patch(module='rtld')
        (self.pd / 'reviewed.json').write_text(json.dumps({'patches': ['p1.json']}))
        with self.assertRaises(patches.PatchError):
            patches.check_dir(self.pd, self.images)

    def test_shipped_patch_folder_matches_its_reviewed_list(self):
        pd = pipeline.BUILDER / 'patches'
        self.assertEqual(patches.check_dir(pd, self.images) if pd.exists() else '', '')


def fake_stage(folder, with_exe=True):
    folder.mkdir(parents=True, exist_ok=True)
    if with_exe:
        exe = bytearray(2_000_000)
        exe[:2] = b'MZ'
        struct.pack_into('<I', exe, 0x3c, 0x80)
        exe[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<H', exe, 0x84, 0x8664)
        (folder / 'SnapRecomp.exe').write_bytes(bytes(exe))
    (folder / 'launcher_data.json').write_text('{}')


class SelftestPromoteTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.log = Quiet(self.t / 'log.txt')
        self.dll = b'dllbytes'
        self.man = {'dlls': [{'name': 'SDL3.dll', 'path': 'bin/', 'sha256': hashlib.sha256(self.dll).hexdigest()}], 'harness': 'shader_harness'}
        for n in ('main.elf', 'sdk.elf'):
            (self.t / n).write_bytes(b'x')
        (self.t / 'romfs').mkdir()

    def tearDown(self):
        self.log.close()

    def stage(self, name='s'):
        s = self.t / name
        fake_stage(s)
        (s / 'SDL3.dll').write_bytes(self.dll)
        (s / 'shader_harness').mkdir()
        (s / 'config.json').write_text(json.dumps({'romfs_root': str(self.t / 'romfs'), 'main_elf': str(self.t / 'main.elf'),
                                                   'sdk_elf': str(self.t / 'sdk.elf')}))
        return s

    def test_good_stage_passes(self):
        s = self.stage()
        self.assertEqual(pipeline.selftest(s, self.man, s / 'config.json'), [])

    def test_problems_are_named(self):
        s = self.stage()
        (s / 'SDL3.dll').write_bytes(b'tampered')
        (s / 'shader_harness').rmdir()
        (self.t / 'romfs').rmdir()
        p = ' | '.join(pipeline.selftest(s, self.man, s / 'config.json'))
        self.assertIn('SDL3.dll does not match', p)
        self.assertIn('shader_harness is missing', p)
        self.assertIn('romfs_root points at a missing path', p)

    def test_not_a_pe_file(self):
        s = self.stage()
        (s / 'SnapRecomp.exe').write_bytes(b'\0' * 2_000_000)
        self.assertIn('not a 64-bit Windows program', ' '.join(pipeline.selftest(s, self.man, s / 'config.json')))

    def test_promote_switches_and_keeps_the_previous_build(self):
        work = self.t / 'work'
        (work / 'builds').mkdir(parents=True)
        old = work / 'out'
        old.mkdir()
        (old / 'marker.txt').write_text('old')
        new = work / 'builds' / 'b2'
        new.mkdir()
        (new / 'marker.txt').write_text('new')
        stray = work / 'builds' / 'failed-earlier'
        stray.mkdir()
        pipeline.promote(work, new, 'b2', self.log)
        self.assertEqual((work / 'out' / 'marker.txt').read_text(), 'new')
        self.assertEqual((work / 'builds' / 'previous' / 'marker.txt').read_text(), 'old')
        self.assertFalse(stray.exists())
        self.assertEqual(json.loads((work / 'current.json').read_text())['build'], 'b2')
        # a third build: only one older build stays
        n3 = work / 'builds' / 'b3'
        n3.mkdir()
        (n3 / 'marker.txt').write_text('third')
        pipeline.promote(work, n3, 'b3', self.log)
        self.assertEqual((work / 'builds' / 'previous' / 'marker.txt').read_text(), 'new')
        self.assertEqual((work / 'out' / 'marker.txt').read_text(), 'third')

    def test_first_build_has_no_previous(self):
        work = self.t / 'work'
        (work / 'builds').mkdir(parents=True)
        new = work / 'builds' / 'b1'
        new.mkdir()
        pipeline.promote(work, new, 'b1', self.log)
        self.assertTrue((work / 'out').exists())
        self.assertIsNone(json.loads((work / 'current.json').read_text())['previous'])
        self.assertTrue((work / 'out' / 'portable.txt').is_file())   # portable by default: written after the switch, never into the staged folder
        self.assertFalse((work / 'out' / 'portable_data').exists())  # the game makes it (and copies an existing %APPDATA% save) on its first start

    def test_promote_from_inside_out(self):
        """The updater is started by the game and stands in out/ (and so does the rebuild): the switch steps out first."""
        import os
        old = os.getcwd()
        d = self.t / 'inside'
        if True:
            work = d / 'work'
            (work / 'out' / 'sub').mkdir(parents=True)
            (work / 'out' / 'marker.txt').write_text('old')
            stage = work / 'builds' / 'b2'
            stage.mkdir(parents=True)
            (stage / 'marker.txt').write_text('new')
            os.chdir(work / 'out' / 'sub')
            try:
                pipeline.promote(work, stage, 'b2', self.log)
                self.assertEqual(Path(os.getcwd()).resolve(), work.resolve())
                self.assertEqual((work / 'out' / 'marker.txt').read_text(), 'new')
                self.assertEqual((work / 'builds' / 'previous' / 'marker.txt').read_text(), 'old')
                # outside out/ nothing changes
                os.chdir(d)
                self.assertFalse(pipeline.leave_folder(work / 'out', work))
                self.assertEqual(Path(os.getcwd()).resolve(), Path(d).resolve())
            finally:
                os.chdir(old)

    def test_failed_switch_restores_out(self):
        work = self.t / 'work'
        (work / 'builds').mkdir(parents=True)
        (work / 'out').mkdir()
        (work / 'out' / 'm').write_text('old')
        with self.assertRaises(StepError):
            pipeline.promote(work, work / 'builds' / 'does-not-exist', 'bx', self.log)
        self.assertEqual((work / 'out' / 'm').read_text(), 'old')


if __name__ == '__main__':
    unittest.main()
