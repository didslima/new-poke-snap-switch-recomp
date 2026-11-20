import argparse
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import fetch, setup_engine as se  # noqa: E402
from snapbuild.util import StepError  # noqa: E402


class Res:
    def __init__(self, ok=True, problems=(), notes=()):
        self.ok, self.problems, self.notes = ok, list(problems), list(notes)


def make_pin(folder, name='tool.zip', files=None):
    """A tiny pinned archive on disk; returns (pin, path)."""
    p = Path(folder) / name
    with zipfile.ZipFile(p, 'w') as z:
        for n, c in (files or {'bin/tool.exe': b'x'}).items():
            z.writestr(n, c)
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    return {'url': 'https://example.invalid/' + name, 'filename': name, 'sha256': h, 'size': p.stat().st_size,
            'install': {'dir': 'tool', 'strip': 0}}, p


class SetupEngineTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.env = mock.patch.dict(os.environ, {'SNAP_SETUP_DIR': str(self.t / 'setup'), 'SNAP_TOOLS_DIR': str(self.t / 'tools'),
                                                'APPDATA': str(self.t / 'appdata'), 'LOCALAPPDATA': str(self.t / 'local')})
        self.env.start()
        self.addCleanup(self.env.stop)

    def events(self):
        buf = io.StringIO()
        return se.Events(out=buf), buf

    @staticmethod
    def parsed(buf):
        return [json.loads(l) for l in buf.getvalue().splitlines()]

    # ---------------------------------------------------------------- progress
    def test_progress_is_monotonic_and_eta_waits_for_data(self):
        ev, buf = self.events()
        pr = se.Progress(ev, {'a': 100, 'b': 100})
        pr.set('a', 0.01, force=True)
        self.assertIsNone(pr.eta())
        pr.set('a', 1.0, force=True)
        self.assertAlmostEqual(pr.pct(), 50.0)
        pr.set('a', 0.2, force=True)           # never goes back
        self.assertAlmostEqual(pr.pct(), 50.0)
        last = self.parsed(buf)[-1]
        self.assertEqual(last['t'], 'progress')

    def test_eventlog_maps_pipeline_step_lines(self):
        ev, buf = self.events()
        w = {'build:elf': 10, 'build:maps': 800, 'build:link': 25}
        pr = se.Progress(ev, w)
        log = se.EventLog(self.t / 'b.log', ev, pr)
        log.write('step elf')
        self.assertLess(pr.frac['build:elf'], 1.0)
        log.write('step elf done in 3s')
        self.assertEqual(pr.frac['build:elf'], 1.0)
        log.write('step maps: restored from the cache (skipped the work)')
        self.assertEqual(pr.frac['build:maps'], 1.0)
        log.write('step link: up to date, skipped')
        self.assertEqual(pr.frac['build:link'], 1.0)
        log.close()
        self.assertTrue(any(e['t'] == 'log' for e in self.parsed(buf)))

    # ---------------------------------------------------------------- tools
    def test_ensure_tool_offline_marks_and_second_run_is_noop(self):
        off = self.t / 'offline'
        off.mkdir()
        pin, _ = make_pin(off)
        self.assertEqual(se.tool_marker(self.t / 'tools', 'tool', pin).exists(), False)
        se.ensure_tool('tool', pin, self.t / 'tools', [off], False, lambda n: None, lambda m: None)
        self.assertTrue((self.t / 'tools' / 'tool' / 'bin' / 'tool.exe').exists())
        self.assertTrue(se.tool_marker(self.t / 'tools', 'tool', pin).exists())
        self.assertTrue((off / pin['filename']).exists(), 'an archive from the payload folder is not deleted')
        table = {'tools': {'tool': pin}}
        self.assertEqual(se.pending_tools(table, self.t / 'tools', ['tool']), [])

    def test_ensure_tool_without_permission_downloads_nothing(self):
        pin, p = make_pin(self.t)
        p.unlink()
        with mock.patch.object(fetch, 'download') as dl:
            with self.assertRaises(StepError) as e:
                se.ensure_tool('tool', pin, self.t / 'tools', [], False, lambda n: None, lambda m: None)
            dl.assert_not_called()
        self.assertIn('not allowed', str(e.exception))

    def test_downloaded_archive_is_deleted_after_install(self):
        pin, p = make_pin(self.t)
        dl = self.t / 'tools' / 'downloads'
        dl.mkdir(parents=True)
        (dl / pin['filename']).write_bytes(p.read_bytes())
        se.ensure_tool('tool', pin, self.t / 'tools', [], True, lambda n: None, lambda m: None)
        self.assertFalse((dl / pin['filename']).exists())

    # ---------------------------------------------------------------- scan and space
    def test_scan_missing_folder_is_plain(self):
        r = se.scan_folder(self.t / 'nope')
        self.assertFalse(r['ok'])
        self.assertIn('is not there', r['error'])

    def test_scan_count_matches_log_when_scan_stops_early(self):
        # a dump folder that stops before the files are read (no keys, or no file reader) still lists the files it found
        g = self.t / 'dumpdir'
        g.mkdir()
        (g / 'game.xci').write_bytes(b'x')
        r = se.scan_folder(g)
        self.assertFalse(r['ok'])
        self.assertEqual(len(r['dumps']), 1)
        self.assertIn('Found 1 game file(s)', r['text'])
        self.assertNotIn('Found 0 game file(s)', r['text'])

    def test_scan_empty_folder(self):
        (self.t / 'empty').mkdir()
        r = se.scan_folder(self.t / 'empty')
        self.assertFalse(r['ok'])
        self.assertTrue(r['error'])

    def test_space_report_shape_and_romfs_note(self):
        r = se.space_report({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools')}, 'dump')
        self.assertIn('cannot be deleted', r['note'])
        self.assertTrue(all(k in next(iter(r['drives'].values())) for k in ('peak', 'stays', 'free', 'ok')))
        names = ' '.join(i['what'] for i in r['items'])
        self.assertIn('Game data extracted', names)
        r2 = se.space_report({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools')}, 'extracted')
        self.assertNotIn('Game data extracted', ' '.join(i['what'] for i in r2['items']))

    # ---------------------------------------------------------------- the run, with the heavy steps replaced
    def fake_run_env(self, game_folder):
        exe = self.t / 'work' / 'out' / 'SnapRecomp.exe'

        def fake_build(opt, log):
            log.write('step elf')
            log.write('step elf done in 1s')
            exe.parent.mkdir(parents=True, exist_ok=True)
            exe.write_bytes(b'MZ')
            return exe, (True, 'title screen reached')
        patches = [mock.patch.object(se.pipeline, 'build', side_effect=fake_build),
                   mock.patch.object(se.vc, 'check_all', return_value=Res()),
                   mock.patch.object(se, 'ensure_tools', return_value=[]),
                   mock.patch.object(se, 'ensure_python', return_value=Path('python.exe')),
                   mock.patch.object(se, 'needs_winsdk', return_value=False),
                   mock.patch.object(se, 'create_shortcuts', return_value=[str(self.t / 'Snap Recomp.lnk')]),
                   mock.patch.object(se, 'register_uninstall')]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        g = Path(game_folder)
        (g / 'exefs').mkdir(parents=True)
        (g / 'exefs' / 'main').write_bytes(b'NSO0' + b'\0' * 100)
        (g / 'exefs' / 'sdk').write_bytes(b'NSO0' + b'\0' * 100)
        (g / 'romfs' / 'nx').mkdir(parents=True)
        (g / 'romfs' / 'common').mkdir(parents=True)
        return exe

    def test_run_extracted_folder_end_to_end(self):
        exe = self.fake_run_env(self.t / 'game')
        ev, buf = self.events()
        cfg = {'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'game_dir': str(self.t / 'game'), 'kind': 'extracted',
               'downloads_ok': True, 'accept_ms_license': True, 'setup_exe': str(self.t / 'SnapRecomp.exe'),
               'shortcuts': {'desktop': True, 'start': True}}
        got = se.run(cfg, ev)
        self.assertEqual(Path(got), exe)
        evs = self.parsed(buf)
        stages = [e['id'] for e in evs if e['t'] == 'stage']
        self.assertEqual(stages, ['game', 'tools', 'winsdk', 'build', 'finish'])
        pcts = [e['pct'] for e in evs if e['t'] == 'progress']
        self.assertEqual(pcts, sorted(pcts))
        self.assertEqual(pcts[-1], 100.0)
        done = evs[-1]
        self.assertEqual(done['t'], 'done')
        self.assertEqual(done['exe'], str(exe))
        s = se.load_settings()
        self.assertTrue(s['completed'])
        self.assertEqual(s['game']['kind'], 'extracted')
        self.assertFalse(s['game']['extracted_by_setup'])
        self.assertNotIn('keys', json.dumps(s).lower().replace('keys_path', ''))

    def test_run_stops_with_plain_message_when_files_are_wrong(self):
        self.fake_run_env(self.t / 'game')
        se.vc.check_all.return_value = Res(False, ['main is from game version 1.0.0. Expected 2.0.1 (v262144). Update the game to 2.0.1 and extract again.'])
        ev, buf = self.events()
        with self.assertRaises(StepError) as e:
            se.run({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'game_dir': str(self.t / 'game'), 'kind': 'extracted'}, ev)
        self.assertIn('not the right game files', str(e.exception))
        self.assertFalse(se.load_settings().get('completed'))

    def test_run_without_ms_license_never_fetches_sdk(self):
        self.fake_run_env(self.t / 'game')
        se.needs_winsdk.return_value = True
        ev, buf = self.events()
        with mock.patch.object(se.winsdk, 'install') as inst:
            with self.assertRaises(StepError) as e:
                se.run({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'game_dir': str(self.t / 'game'), 'kind': 'extracted',
                        'accept_ms_license': False}, ev)
            inst.assert_not_called()
        self.assertIn('license', str(e.exception))

    def test_cli_turns_step_error_into_retryable_error_event(self):
        self.fake_run_env(self.t / 'game')
        se.pipeline.build.side_effect = StepError('the compiler stopped')
        cfgf = self.t / 'cfg.json'
        cfgf.write_text(json.dumps({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'game_dir': str(self.t / 'game'),
                                    'kind': 'extracted'}), encoding='utf-8-sig')
        out = io.StringIO()
        with mock.patch.object(sys, 'stdout', out):
            rc = se.cli(argparse.Namespace(action='run', config=str(cfgf)))
        self.assertEqual(rc, 1)
        last = json.loads(out.getvalue().splitlines()[-1])
        self.assertEqual(last['t'], 'error')
        self.assertTrue(last['retry'])
        self.assertIn('compiler stopped', last['text'])

    def test_repair_asks_to_choose_again_when_game_files_moved(self):
        s = {'game': {'folder': str(self.t / 'gone'), 'kind': 'extracted', 'files': {'main': {'path': str(self.t / 'gone' / 'main')}}}}
        se.save_settings(s)
        out = io.StringIO()
        with mock.patch.object(sys, 'stdout', out):
            rc = se.cli(argparse.Namespace(action='repair', config=None))
        self.assertEqual(rc, 1)
        last = json.loads(out.getvalue().splitlines()[-1])
        self.assertEqual(last['action'], 'choose_folder')

    # ---------------------------------------------------------------- uninstall
    def test_uninstall_never_touches_the_players_game_files(self):
        game = self.t / 'players_dumps'
        game.mkdir()
        (game / 'game.xci').write_bytes(b'x')
        work = self.t / 'root' / 'work'
        work.mkdir(parents=True)
        (work / 'f').write_bytes(b'x')
        extracted = self.t / 'root' / 'game'
        extracted.mkdir()
        (extracted / 'romfs.bin').write_bytes(b'x')
        lnk = self.t / 'desk' / 'Snap Recomp.lnk'
        lnk.parent.mkdir()
        lnk.write_bytes(b'x')
        saves = self.t / 'appdata' / 'SnapRecomp' / 'save'
        saves.mkdir(parents=True)
        (saves / 's').write_bytes(b'x')
        se.save_settings({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'shortcuts': [str(lnk)],
                          'game': {'folder': str(game), 'extract_dir': str(extracted), 'extracted_by_setup': True}})
        ev, buf = self.events()
        r = se.uninstall(False, False, ev)
        self.assertTrue((game / 'game.xci').exists())
        self.assertFalse(work.exists())
        self.assertFalse(extracted.exists())
        self.assertFalse(lnk.exists())
        self.assertTrue((saves / 's').exists(), 'saves stay unless the player asks')
        self.assertTrue(any('saves' in k for k in r['kept']))
        se.save_settings({'root': str(self.t / 'root'), 'game': {}})
        r2 = se.uninstall(False, True, ev)
        self.assertFalse((self.t / 'appdata' / 'SnapRecomp').exists())

    def test_uninstall_keeps_portable_saves_inside_the_work_folder(self):
        work = self.t / 'root' / 'work'
        saves = work / 'out' / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'profiles'
        saves.mkdir(parents=True)
        (saves / 's').write_bytes(b'save')
        (work / 'out' / 'SnapRecomp.exe').write_bytes(b'MZ')
        (work / 'out' / 'portable.txt').write_text('')
        (work / 'obj').mkdir()
        (work / 'obj' / 'a.o').write_bytes(b'x')
        (work / 'builds' / 'previous').mkdir(parents=True)
        se.save_settings({'root': str(self.t / 'root'), 'game': {}})
        ev, buf = self.events()
        r = se.uninstall(False, False, ev)
        self.assertEqual((saves / 's').read_bytes(), b'save')
        self.assertEqual(sorted(p.name for p in work.iterdir()), ['out'])
        self.assertEqual(sorted(p.name for p in (work / 'out').iterdir()), ['portable_data'])
        self.assertTrue(any('portable_data' in k for k in r['kept']))
        se.save_settings({'root': str(self.t / 'root'), 'game': {}})
        se.uninstall(False, True, ev)   # the player asked for the saves to go too
        self.assertFalse(work.exists())

    def test_shortcuts_start_the_one_exe(self):
        top = self.t / 'Snap' / 'SnapRecomp.exe'
        built = self.t / 'Snap' / 'work' / 'out' / 'SnapRecomp.exe'
        built.parent.mkdir(parents=True)
        top.write_bytes(b'MZ')
        built.write_bytes(b'MZ')
        made = []
        note = lambda lnk, target, args='', **kw: made.append((Path(lnk).name, str(target), args, kw.get('workdir'), kw.get('icon')))   # noqa: E731
        with mock.patch.object(se, 'special_folder', side_effect=lambda n: self.t / n), mock.patch.object(se, 'make_shortcut', side_effect=note):
            se.create_shortcuts(built, str(top), True, True, lambda *_: None)
            by = {m[0]: m for m in made}
            self.assertEqual(len(made), 4)
            self.assertTrue(all(m[1] == str(top) for m in made), made)             # every shortcut starts the downloaded exe
            self.assertEqual(by['Snap Recomp.lnk'][3:], (str(top.parent), str(built)))   # its folder, the game's icon
            self.assertEqual((by['Repair Snap Recomp.lnk'][2], by['Uninstall Snap Recomp.lnk'][2]), ('--repair', '--uninstall'))
            made.clear()
            se.create_shortcuts(built, None, True, False, lambda *_: None)          # a build without the exe (developer run)
            self.assertEqual([(m[0], m[1]) for m in made], [('Snap Recomp.lnk', str(built))])

    def test_shortcuts_are_off_unless_asked_for_and_no_registry_entry_is_written(self):
        self.assertEqual(se.normalize_cfg({})['shortcuts'], {'desktop': False, 'start': False})
        self.fake_run_env(self.t / 'game')
        ev, buf = self.events()
        with mock.patch.object(se, 'unregister_uninstall') as unreg:
            se.run({'root': str(self.t / 'root'), 'tools_dir': str(self.t / 'tools'), 'game_dir': str(self.t / 'game'), 'kind': 'extracted',
                    'setup_exe': str(self.t / 'SnapRecomp.exe')}, ev)
            se.create_shortcuts.assert_not_called()
            se.register_uninstall.assert_not_called()
            unreg.assert_called_once()   # an entry an older version wrote is taken away
        self.assertEqual(se.load_settings()['shortcuts'], [])


if __name__ == '__main__':
    unittest.main()
