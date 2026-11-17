"""Stage 5D-B1: tests snap_boot_linux validate across synthetic guest ELFs and AOT archives."""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = ROOT / 'recomp' / 'snap_build_linux.py'


def make_synthetic_elf(machine=0xB7, elf_type=3, memsz=0x2000, truncate_to=None):
    # Minimal valid ELF64 LE with 2 PT_LOAD (text + dynamic/rodata) and 1 PT_DYNAMIC
    ehdr_sz = 64
    phdr_sz = 56
    phnum = 2
    # Layout:
    # 0x000..0x0d0: ehdr + 2 phdrs
    # 0x100..0x200: .dynstr (16B) + .dynsym (24B) + .hash (16B) + .dynamic (6*16=96B)
    dynstr_va = 0x100
    dynsym_va = 0x110
    hash_va = 0x130
    dyn_va = 0x140
    total_file = 0x200

    ehdr = struct.pack('<16sHHIQQQIHHHHHH',
                       b'\x7fELF\x02\x01\x01' + b'\x00' * 9,
                       elf_type, machine, 1,
                       0, ehdr_sz, 0,
                       0, ehdr_sz, phdr_sz, phnum, 64, 0, 0)
    # PT_LOAD (R|X): covers [0, memsz)
    ph_load = struct.pack('<IIQQQQQQ', 1, 5, 0, 0, 0, total_file, memsz, 0x1000)
    # PT_DYNAMIC
    ph_dyn = struct.pack('<IIQQQQQQ', 2, 6, dyn_va, dyn_va, dyn_va, 6 * 16, 6 * 16, 8)

    buf = bytearray(total_file)
    buf[0:64] = ehdr
    buf[64:120] = ph_load
    buf[120:176] = ph_dyn
    buf[dynstr_va:dynstr_va + 5] = b'\x00sym\x00'
    # DT_HASH: nbucket=1, nchain=1
    struct.pack_into('<II', buf, hash_va, 1, 1)
    # DT_DYNAMIC entries: STR(5), SYM(6), HASH(4), RELA(7), RELASZ(8), NULL(0)
    entries = [(5, dynstr_va), (6, dynsym_va), (4, hash_va), (7, dyn_va), (8, 0), (0, 0)]
    for i, (tag, val) in enumerate(entries):
        struct.pack_into('<qQ', buf, dyn_va + i * 16, tag, val)

    data = bytes(buf)
    if truncate_to is not None:
        data = data[:truncate_to]
    return data


def build_aot_lib(work_dir, module_name, abi_ver=3, extra_c='', include_tables=True, extra_obj=None):
    gen = work_dir / f'gen_{module_name}'
    out = work_dir / f'out_{module_name}'
    gen.mkdir(parents=True, exist_ok=True)
    chunks = [f'{module_name}_0000.c']
    (gen / 'manifest.json').write_text(json.dumps({'module': module_name, 'chunks': chunks}), encoding='utf-8')
    (gen / chunks[0]).write_text(
        '#include "cpu.h"\n' + extra_c +
        f'void f_{module_name}_0(Cpu *c) {{ FN_ENTER(0x7100000030ull); c->x[0] = rt_ticks(); }}\n',
        encoding='utf-8')
    if module_name == 'main':
        (gen / 'plt_stubs.c').write_text(
            '#include "cpu.h"\n__attribute__((weak)) void imp_memcpy(Cpu *c) { rt_import_call(c, 0); }\n',
            encoding='utf-8')
    (gen / 'unresolved.c').write_text('#include "cpu.h"\n', encoding='utf-8')
    if include_tables:
        if module_name == 'main':
            tbl = (
                '#include "cpu.h"\n'
                f'extern const int snap_runtime_abi_{abi_ver};\n'
                f'const int *const main_abi_ref = &snap_runtime_abi_{abi_ver};\n'
                'void f_main_0(Cpu *c);\n'
                'const uint64_t main_fn_addrs[] = { 0x7100000030ull };\n'
                'const GuestFn main_fn_ptrs[] = { f_main_0 };\n'
                'const uint64_t main_fn_count = 1;\n'
                'const char *const main_import_names[] = { "memcpy" };\n'
                'const uint64_t main_import_stub_addrs[] = { 0x7100001000ull };\n'
                'const uint64_t main_import_count = 1;\n'
            )
        else:
            tbl = (
                '#include "cpu.h"\n'
                f'extern const int snap_runtime_abi_{abi_ver};\n'
                f'const int *const sdk_abi_ref = &snap_runtime_abi_{abi_ver};\n'
                'void f_sdk_0(Cpu *c);\n'
                'const uint64_t sdk_fn_addrs[] = { 0x7110000040ull };\n'
                'const GuestFn sdk_fn_ptrs[] = { f_sdk_0 };\n'
                'const uint64_t sdk_fn_count = 1;\n'
            )
    else:
        tbl = (
            '#include "cpu.h"\n'
            f'extern const int snap_runtime_abi_{abi_ver};\n'
            f'const int *const dummy_abi_ref = &snap_runtime_abi_{abi_ver};\n'
        )
    (gen / f'{module_name}_table.c').write_text(tbl, encoding='utf-8')

    subprocess.check_call([
        sys.executable, '-I', str(BUILD_SCRIPT),
        '--gen', str(gen), '--out', str(out), '--lib', f'snap_{module_name}', '--jobs', '2',
    ], stdout=subprocess.DEVNULL)
    lib = out / 'lib' / f'libsnap_{module_name}.a'
    if extra_obj is not None:
        subprocess.check_call(['ar', 'r', str(lib), str(extra_obj)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return lib


class PreflightLinuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = os.environ.get('SNAP_BOOT_LINUX_BIN')
        if not cls.runner or not Path(cls.runner).is_file():
            raise RuntimeError('SNAP_BOOT_LINUX_BIN env var must point to snap_boot_linux')

    def run_validate(self, *args):
        return subprocess.run([self.runner, 'validate', *args], capture_output=True, text=True)

    def test_valid_synthetic_and_paths_with_spaces(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            spaced = td / 'dir with spaces'
            spaced.mkdir()
            main_elf = spaced / 'my main.elf'
            sdk_elf = spaced / 'my sdk.elf'
            main_elf.write_bytes(make_synthetic_elf())
            sdk_elf.write_bytes(make_synthetic_elf())
            main_lib = build_aot_lib(td, 'main', abi_ver=3)
            sdk_lib = build_aot_lib(td, 'sdk', abi_ver=3)
            main_lib_sp = spaced / 'libsnap main.a'
            sdk_lib_sp = spaced / 'libsnap sdk.a'
            shutil.copyfile(main_lib, main_lib_sp)
            shutil.copyfile(sdk_lib, sdk_lib_sp)

            r = self.run_validate('--main', str(main_elf), '--sdk', str(sdk_elf),
                                  '--main-lib', str(main_lib_sp), '--sdk-lib', str(sdk_lib_sp))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('preflight: PASS', r.stdout)

    def test_elf_failure_modes(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            sdk_elf = td / 'sdk.elf'
            sdk_elf.write_bytes(make_synthetic_elf())
            main_lib = build_aot_lib(td, 'main', abi_ver=3)
            sdk_lib = build_aot_lib(td, 'sdk', abi_ver=3)

            # x86-64 ELF rejected as guest ELF
            bad_arch = td / 'x86.elf'
            bad_arch.write_bytes(make_synthetic_elf(machine=0x3E))
            r = self.run_validate('--main', str(bad_arch), '--sdk', str(sdk_elf),
                                  '--main-lib', str(main_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn('[FAIL] main_elf', r.stdout)

            # Truncated ELF
            trunc = td / 'trunc.elf'
            trunc.write_bytes(make_synthetic_elf(truncate_to=80))
            r = self.run_validate('--main', str(trunc), '--sdk', str(sdk_elf),
                                  '--main-lib', str(main_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)

            # Segment exceeds main's 256 MiB window before sdk
            oversized = td / 'oversized.elf'
            oversized.write_bytes(make_synthetic_elf(memsz=0x11000000))
            r = self.run_validate('--main', str(oversized), '--sdk', str(sdk_elf),
                                  '--main-lib', str(main_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)

    def test_aot_failure_and_blocked_modes(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            main_elf = td / 'main.elf'
            sdk_elf = td / 'sdk.elf'
            main_elf.write_bytes(make_synthetic_elf())
            sdk_elf.write_bytes(make_synthetic_elf())
            sdk_lib = build_aot_lib(td, 'sdk', abi_ver=3)

            # ABI 1 rejected
            abi1_lib = build_aot_lib(td / 'abi1', 'main', abi_ver=1)
            r = self.run_validate('--main', str(main_elf), '--sdk', str(sdk_elf),
                                  '--main-lib', str(abi1_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn('ABI mismatch', r.stdout)

            # Missing required symbols
            notbl_lib = build_aot_lib(td / 'notbl', 'main', abi_ver=3, include_tables=False)
            r = self.run_validate('--main', str(main_elf), '--sdk', str(sdk_elf),
                                  '--main-lib', str(notbl_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn('missing required AOT symbol', r.stdout)

            # Unresolved external dependency -> BLOCKED (non-zero)
            unprov_lib = build_aot_lib(td / 'unprov', 'main', abi_ver=3,
                                       extra_c='extern void missing_provider_fn(void); void use_it(void) { missing_provider_fn(); }\n')
            r = self.run_validate('--main', str(main_elf), '--sdk', str(sdk_elf),
                                  '--main-lib', str(unprov_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 2, r.stdout)
            self.assertIn('missing_provider_fn', r.stdout)

            # Archive containing wrong architecture object (AArch64 ELF relocatable)
            bad_obj = td / 'wrong_arch.o'
            # Minimal ELF64 LE ET_REL EM_AARCH64 header
            bad_obj.write_bytes(struct.pack('<16sHHIQQQIHHHHHH',
                                            b'\x7fELF\x02\x01\x01' + b'\x00' * 9,
                                            1, 0xB7, 1, 0, 0, 0, 0, 64, 0, 0, 64, 0, 0))
            bad_arch_lib = build_aot_lib(td / 'badarch', 'main', abi_ver=3, extra_obj=bad_obj)
            r = self.run_validate('--main', str(main_elf), '--sdk', str(sdk_elf),
                                  '--main-lib', str(bad_arch_lib), '--sdk-lib', str(sdk_lib))
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn('incompatible object architecture', r.stdout)

    def test_missing_paths_and_incomplete_cli(self):
        r = self.run_validate('--main', '/nonexistent/main.elf', '--sdk', '/nonexistent/sdk.elf',
                              '--main-lib', '/nonexistent/libsnap_main.a', '--sdk-lib', '/nonexistent/libsnap_sdk.a')
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn('preflight: BLOCKED', r.stdout)

        r2 = subprocess.run([self.runner, 'validate', '--main-lib'], capture_output=True, text=True)
        self.assertEqual(r2.returncode, 2)
        self.assertIn('missing argument for --main-lib', r2.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
