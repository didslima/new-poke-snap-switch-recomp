"""Stage 5D-A: validates Linux AOT compilation, archiving (libsnap_main.a / libsnap_sdk.a), ELF metadata, and linking."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'recomp' / 'snap_build_linux.py'
LINK_SRC = ROOT / 'tests' / 'platform' / 'aot_link_test.cpp'
ABI_SRC = ROOT / 'runtime' / 'os' / 'abi.cpp'


def write_synthetic_aot(base_dir):
    main_dir = base_dir / 'main'
    sdk_dir = base_dir / 'sdk'
    main_dir.mkdir(parents=True)
    sdk_dir.mkdir(parents=True)

    (main_dir / 'manifest.json').write_text(json.dumps({
        'module': 'main',
        'chunks': ['main_0000.c'],
    }), encoding='utf-8')
    (main_dir / 'main_0000.c').write_text(
        '#include "cpu.h"\n'
        'void f_7100000030(Cpu *c) { FN_ENTER(0x7100000030ull); c->x[0] += 1; }\n',
        encoding='utf-8')
    (main_dir / 'plt_stubs.c').write_text(
        '#include "cpu.h"\n'
        '__attribute__((weak)) void imp_memcpy(Cpu *c) { rt_import_call(c, 0); }\n',
        encoding='utf-8')
    (main_dir / 'unresolved.c').write_text('#include "cpu.h"\n', encoding='utf-8')
    (main_dir / 'main_table.c').write_text(
        '#include "cpu.h"\n'
        '_Static_assert(SNAP_RUNTIME_ABI == 3, "ABI 3");\n'
        'extern const int snap_runtime_abi_3;\n'
        'const int *const main_abi_ref = &snap_runtime_abi_3;\n'
        'void f_7100000030(Cpu *c);\n'
        'const uint64_t main_fn_addrs[] = { 0x7100000030ull };\n'
        'const GuestFn main_fn_ptrs[] = { f_7100000030 };\n'
        'const uint64_t main_fn_count = 1;\n'
        'const char *const main_import_names[] = { "memcpy" };\n'
        'const uint64_t main_import_stub_addrs[] = { 0x7100001000ull };\n'
        'const uint64_t main_import_count = 1;\n',
        encoding='utf-8')

    (sdk_dir / 'manifest.json').write_text(json.dumps({
        'module': 'sdk',
        'chunks': ['sdk_0000.c'],
    }), encoding='utf-8')
    (sdk_dir / 'sdk_0000.c').write_text(
        '#include "cpu.h"\n'
        'void f_7110000040(Cpu *c) { FN_ENTER(0x7110000040ull); c->x[0] = rt_ticks(); }\n',
        encoding='utf-8')
    (sdk_dir / 'unresolved.c').write_text('#include "cpu.h"\n', encoding='utf-8')
    (sdk_dir / 'sdk_table.c').write_text(
        '#include "cpu.h"\n'
        '_Static_assert(SNAP_RUNTIME_ABI == 3, "ABI 3");\n'
        'extern const int snap_runtime_abi_3;\n'
        'const int *const sdk_abi_ref = &snap_runtime_abi_3;\n'
        'void f_7110000040(Cpu *c);\n'
        'const uint64_t sdk_fn_addrs[] = { 0x7110000040ull };\n'
        'const GuestFn sdk_fn_ptrs[] = { f_7110000040 };\n'
        'const uint64_t sdk_fn_count = 1;\n',
        encoding='utf-8')
    return main_dir, sdk_dir


class AotLinuxBuildTest(unittest.TestCase):
    def test_build_elf_archives_and_link(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            main_gen, sdk_gen = write_synthetic_aot(td / 'gen')
            main_out = td / 'out_main'
            sdk_out = td / 'out_sdk'

            subprocess.check_call([
                sys.executable, '-I', str(SCRIPT),
                '--gen', str(main_gen), '--out', str(main_out), '--lib', 'snap_main', '--jobs', '2',
            ])
            subprocess.check_call([
                sys.executable, '-I', str(SCRIPT),
                '--gen', str(sdk_gen), '--out', str(sdk_out), '--lib', 'snap_sdk', '--jobs', '2',
            ])

            main_lib = main_out / 'lib' / 'libsnap_main.a'
            sdk_lib = sdk_out / 'lib' / 'libsnap_sdk.a'
            self.assertTrue(main_lib.is_file())
            self.assertTrue(sdk_lib.is_file())

            # Incremental rebuild must report 0 TUs built.
            subprocess.check_call([
                sys.executable, '-I', str(SCRIPT),
                '--gen', str(main_gen), '--out', str(main_out), '--lib', 'snap_main', '--jobs', '2',
            ])
            rep = json.loads((main_out / 'build_report.json').read_text(encoding='utf-8'))
            self.assertEqual(rep['tus_built'], 0)

            # Verify archive members and ELF64 x86-64 object format.
            members = subprocess.check_output(['ar', 't', str(main_lib)], text=True).splitlines()
            self.assertEqual(members, ['main_0000.o', 'main_table.o', 'plt_stubs.o', 'unresolved.o'])

            obj = main_out / 'obj' / 'main_table.o'
            if shutil.which('file'):
                f_out = subprocess.check_output(['file', str(obj)], text=True)
                self.assertIn('ELF 64-bit', f_out)
                self.assertIn('x86-64', f_out)
            if shutil.which('readelf'):
                r_out = subprocess.check_output(['readelf', '-h', str(obj)], text=True)
                self.assertIn('ELF64', r_out)
                self.assertIn('Advanced Micro Devices X86-64', r_out)

            # Verify expected defined symbols and absence of Windows CRT/import artifacts.
            nm_main = subprocess.check_output(['nm', '-g', '--defined-only', str(main_lib)], text=True)
            for sym in ('main_fn_addrs', 'main_fn_ptrs', 'main_fn_count',
                        'main_import_names', 'main_import_stub_addrs', 'main_import_count',
                        'f_7100000030', 'imp_memcpy'):
                self.assertIn(sym, nm_main)
            nm_sdk = subprocess.check_output(['nm', '-g', '--defined-only', str(sdk_lib)], text=True)
            for sym in ('sdk_fn_addrs', 'sdk_fn_ptrs', 'sdk_fn_count', 'f_7110000040'):
                self.assertIn(sym, nm_sdk)
            nm_undef = subprocess.check_output(['nm', '-u', str(main_lib)], text=True)
            self.assertNotIn('__imp_', nm_undef)

            # Link a real ELF executable referencing the AOT tables (without running guest code).
            exe = td / 'aot_link_test'
            cc = shutil.which('clang++') or 'clang++'
            subprocess.check_call([
                cc, '-std=c++20', '-O2', '-march=x86-64-v3',
                '-I', str(ROOT / 'runtime'),
                str(LINK_SRC), str(ABI_SRC), str(main_lib), str(sdk_lib),
                '-o', str(exe),
            ])
            out = subprocess.check_output([str(exe)], text=True)
            self.assertIn('aot_link_test: main=1 sdk=1 imports=1', out)

    def test_missing_manifest_fails_clearly(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            r = subprocess.run([
                sys.executable, '-I', str(SCRIPT),
                '--gen', str(td / 'missing'), '--out', str(td / 'out'), '--lib', 'snap_main',
            ], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('manifest.json', r.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
