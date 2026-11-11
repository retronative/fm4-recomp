# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of scripts/gen_runtime_tables.py on a synthetic survey and ppc/ (no game data): the image
# check, the import-kind check, the thunk/declaration cross-check, the strict stubs and the import
# records. Expected text is written by hand from the inputs.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_gen_runtime_tables.py
import json, os, shutil, struct, subprocess, sys, tempfile, unittest, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'gen_runtime_tables.py'
IMAGE = bytes(range(256)) * 2  # 512 bytes


def xex(tls=(64, 0x40000180, 0x1820, 0x1800), stack=0x10000, keys=(0x00020104, 0x00020200)):
    """A synthetic XEX2 header (Xenia xex2_info.h xex2_header): magic, header size at +8, optional-header
    count at +0x14, then {key, value} pairs from +0x18; the TLS info (4 words) at +0x40 by offset, the
    default stack size inline. 0x60 bytes, then 0x20 bytes that are not header."""
    h = bytearray(0x60)
    h[0:4] = b'XEX2'
    struct.pack_into('>I', h, 8, 0x60)
    struct.pack_into('>I', h, 0x14, 2)
    struct.pack_into('>II', h, 0x18, keys[0], 0x40)
    struct.pack_into('>II', h, 0x20, keys[1], stack)
    struct.pack_into('>4I', h, 0x40, *tls)
    return bytes(h) + b'\xAA' * 0x20


def survey(tmp, slots=None, size=len(IMAGE), declared=('__imp__KeA', '__imp__XamB'), xex_bytes=None):
    s, p, o = tmp / 'survey', tmp / 'ppc', tmp / 'out'
    tmp.mkdir(exist_ok=True); s.mkdir(); p.mkdir()
    (tmp / 'default.xex').write_bytes(xex_bytes if xex_bytes is not None else xex())
    (s / 'image.bin').write_bytes(IMAGE)
    slots = slots if slots is not None else [
        {'library': 'xboxkrnl.exe', 'name': '__imp__KeA', 'ordinal': 1, 'address': 0x40000010, 'kind': 'function_slot'},
        {'library': 'xboxkrnl.exe', 'name': '__imp__KeA', 'ordinal': 0, 'address': 0x40000100, 'kind': 'thunk'},
        {'library': 'xam.xex', 'name': '__imp__XamB', 'ordinal': 2, 'address': 0x40000014, 'kind': 'function_slot'},
        {'library': 'xam.xex', 'name': '__imp__XamB', 'ordinal': 0, 'address': 0x40000110, 'kind': 'thunk'},
        {'library': 'xboxkrnl.exe', 'name': '__imp__KeData', 'ordinal': 3, 'address': 0x40000018, 'kind': 'variable'}]
    (s / 'report.json').write_text(json.dumps({'base': 0x40000000, 'size': size, 'entry': 0x40000100, 'import_slots': slots,
                                               'sections': [{'name': '.text', 'start': 0x40000100, 'end': 0x40000200, 'code': True}]}))
    (p / 'ppc_config.h').write_text('#define PPC_IMAGE_BASE 0x40000000ull\n#define PPC_IMAGE_SIZE 0x200ull\n'
                                    '#define PPC_CODE_BASE 0x40000100ull\n#define PPC_CODE_SIZE 0x100ull\n')
    (p / 'ppc_recomp_shared.h').write_text(''.join(f'PPC_EXTERN_FUNC({d});\n' for d in declared))
    return s, p, o


def run(s, p, o, extra=()):
    r = subprocess.run([sys.executable, '-B', '-I', str(SCRIPT), str(s), str(p), str(s.parent / 'default.xex'), str(o), *map(str, extra)],
                       capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout + r.stderr


class Generate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_stubs_and_records(self):
        code, log = run(*survey(self.t))
        self.assertEqual(code, 0, log)
        stubs = (self.t / 'out' / 'fm4_imports.cpp').read_text()
        self.assertIn('__attribute__((weak)) PPC_FUNC(__imp__KeA) { (void)base; PpcImportStop("KeA", ctx); }\n', stubs)
        self.assertIn('__attribute__((weak)) PPC_FUNC(__imp__XamB) { (void)base; PpcImportStop("XamB", ctx); }', stubs)
        self.assertIn('#include "ppc_recomp_shared.h"', stubs)
        self.assertEqual(stubs.count('PPC_FUNC('), 2)
        header = (self.t / 'out' / 'fm4_image.h').read_text()
        self.assertIn('constexpr uint32_t kImageCrc32 = 0x%08Xu;' % zlib.crc32(IMAGE), header)
        self.assertIn('{0x40000010u, ImportKind::FunctionSlot, 0x40000100u, "xboxkrnl.exe", "__imp__KeA"},', header)
        self.assertIn('{0x40000018u, ImportKind::Variable, 0x00000000u, "xboxkrnl.exe", "__imp__KeData"},', header)
        # The XEX header: its first 0x60 bytes (the size at +8), their CRC-32, the TLS info and stack.
        self.assertIn('constexpr uint32_t kHeaderSize = 0x00000060u;', header)
        self.assertIn('constexpr uint32_t kHeaderCrc32 = 0x%08Xu;' % zlib.crc32(xex()[:0x60]), header)
        self.assertIn('constexpr runtime::TlsInfo kTls = {64u, 0x40000180u, 0x1820u, 0x1800u};', header)
        self.assertIn('constexpr uint32_t kDefaultStackSize = 0x10000u;', header)

    def test_xex_without_tls_or_stack_refused(self):
        code, log = run(*survey(self.t, xex_bytes=xex(keys=(0x00020105, 0x00020200))))
        self.assertNotEqual(code, 0)
        self.assertIn('no TLS info or default stack size', log)

    def test_not_an_xex_refused(self):
        code, log = run(*survey(self.t, xex_bytes=b'XEX1' + xex()[4:]))
        self.assertNotEqual(code, 0)
        self.assertIn('not an XEX2 file', log)

    def test_image_mismatch_refused(self):
        code, log = run(*survey(self.t, size=0x100))
        self.assertNotEqual(code, 0)
        self.assertIn('describe different images', log)

    def test_unexpected_kind_refused(self):
        slots = [{'library': 'x', 'name': '__imp__KeA', 'ordinal': 1, 'address': 0x40000010, 'kind': 'unexpected'}]
        code, log = run(*survey(self.t, slots=slots, declared=()))
        self.assertNotEqual(code, 0)
        self.assertIn("unexpected import record kinds ['unexpected']", log)

    def test_thunk_missing_from_ppc_refused(self):
        code, log = run(*survey(self.t, declared=('__imp__KeA',)))
        self.assertNotEqual(code, 0)
        self.assertIn("only in the survey ['__imp__XamB']", log)

    def test_declaration_missing_from_survey_refused(self):
        code, log = run(*survey(self.t, declared=('__imp__KeA', '__imp__XamB', '__imp__Extra')))
        self.assertNotEqual(code, 0)
        self.assertIn("only in ppc/ ['__imp__Extra']", log)


    def module(self, declared):
        # A second module: its own survey and ppc/ (thunks KeA and NtOnly), declaring _xstart, two helpers
        # and a generated function besides its import thunks.
        slots = [{'library': 'xboxkrnl.exe', 'name': n, 'ordinal': 0, 'address': 0x40000100 + 0x10 * i, 'kind': 'thunk'}
                 for i, n in enumerate(('__imp__KeA', '__imp__NtOnly'))]
        s, p, _ = survey(self.t / 'm', slots=slots, declared=declared)
        return ['--module', 'facade', s, p, s.parent / 'default.xex']

    def test_module_symbols_and_stub_union(self):
        extra = self.module(('__imp__KeA', '__imp__NtOnly', '_xstart', '__restgprlr_14', '__savefpr_14', 'sub_40000120'))
        code, log = run(*survey(self.t), extra)
        self.assertEqual(code, 0, log)
        self.assertEqual((self.t / 'out' / 'facade_symbols.h').read_text(),
                         "// Generated by scripts/gen_runtime_tables.py from the player's own files; do not edit.\n"
                         "// Force-included into the facade library: the names every translation defines alike, renamed.\n"
                         "#pragma once\n"
                         "#define __restgprlr_14 facade___restgprlr_14\n"
                         "#define __imp____restgprlr_14 facade___imp____restgprlr_14\n"
                         "#define __savefpr_14 facade___savefpr_14\n"
                         "#define __imp____savefpr_14 facade___imp____savefpr_14\n"
                         "#define _xstart facade__xstart\n"
                         "#define __imp___xstart facade___imp___xstart\n"
                         "#define PPCFuncMappings facade_PPCFuncMappings\n")
        stubs = (self.t / 'out' / 'fm4_imports.cpp').read_text()
        self.assertEqual(stubs.count('PPC_FUNC('), 3)  # KeA once, XamB, NtOnly
        self.assertIn('PPC_FUNC(__imp__NtOnly) { (void)base; PpcImportStop("NtOnly", ctx); }', stubs)

    def test_module_image(self):
        # The module's ModuleImage: name, file name, base, size, entry, code range (ppc_config.h), CRC-32 of
        # its image, header (the first 0x60 bytes) and whole file, its sections, its non-thunk import records
        # (a function slot with its thunk, a variable), and its renamed function table.
        slots = [{'library': 'xboxkrnl.exe', 'name': '__imp__KeA', 'ordinal': 1, 'address': 0x40000010, 'kind': 'function_slot'},
                 {'library': 'xboxkrnl.exe', 'name': '__imp__KeA', 'ordinal': 0, 'address': 0x40000100, 'kind': 'thunk'},
                 {'library': 'xboxkrnl.exe', 'name': '__imp__KeData', 'ordinal': 3, 'address': 0x40000018, 'kind': 'variable'}]
        s, p, _ = survey(self.t / 'm', slots=slots, declared=('__imp__KeA',))
        code, log = run(*survey(self.t), ['--module', 'facade', s, p, s.parent / 'default.xex'])
        self.assertEqual(code, 0, log)
        text = (self.t / 'out' / 'fm4_modules.h').read_text()
        self.assertIn('extern PPCFuncMapping facade_PPCFuncMappings[];', text)
        self.assertIn('    {0x40000100u, 0x40000200u, true},', text)
        self.assertIn('    {0x40000010u, false, 0x40000100u, "__imp__KeA"},\n    {0x40000018u, true, 0x00000000u, "__imp__KeData"},', text)
        whole = xex()
        self.assertIn('    {"facade", "default.xex", 0x40000000u, 0x00000200u, 0x40000100u,\n'
                      '     0x40000100u, 0x00000100u, 0x%08Xu, 0x00000060u,\n'
                      '     0x%08Xu, 0x%08Xu, 0x%08Xu, 0x00000000u,\n'
                      '     kfacadeSections, std::size(kfacadeSections),\n'
                      '     kfacadeImports, std::size(kfacadeImports), facade_PPCFuncMappings},'
                      % (zlib.crc32(IMAGE), zlib.crc32(whole[:0x60]), len(whole), zlib.crc32(whole)), text)

    def test_module_thunks_checked(self):
        extra = self.module(('__imp__KeA',))
        code, log = run(*survey(self.t), extra)
        self.assertNotEqual(code, 0)
        self.assertIn("only in the survey ['__imp__NtOnly']", log)

    def test_module_names_checked(self):
        for name in ('Facade', 'fm4', '9x'):
            extra = self.module(('__imp__KeA', '__imp__NtOnly'))
            extra[1] = name
            code, log = run(*survey(self.t), extra)
            self.assertNotEqual(code, 0, name)
            self.assertIn('module names must be distinct', log, name)
            for d in ('survey', 'ppc', 'out', 'm'):
                shutil.rmtree(self.t / d, ignore_errors=True)  # inside the test's own temporary directory

    def test_arguments_checked(self):
        s, p, o = survey(self.t)
        code, log = run(s, p, o, ['--modul', 'facade', s, p, s.parent / 'default.xex'])
        self.assertNotEqual(code, 0)
        self.assertIn('usage:', log)


class TitleHooks(unittest.TestCase):
    """FM4 Recomp: config/title_hooks.toml -> fm4_hooks.cpp, every hook checked by the SHA-256 of its function's bytes."""
    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location('gen', Path(__file__).resolve().parent.parent / 'scripts' / 'gen_runtime_tables.py')
        cls.gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.gen)

    def config(self, function=0x10008, size=8, sha=None, flag='FM4_TEST_HOOK', value=1):
        import hashlib
        image = bytes(range(64))
        sha = sha or hashlib.sha256(image[8:8 + size]).hexdigest()  # the 8 bytes at base + 8 (hand: 08 09 .. 0F)
        return image, f'[[hook]]\nname = "test"\nfunction = 0x{function:X}\nflag = "{flag}"\nreturns = {value}\nbytes = {size}\nsha256 = "{sha}"\n'

    def test_override_generated(self):
        image, text = self.config()
        out = self.gen.title_hooks(text, image, 0x10000)
        self.assertIn('PPC_FUNC(sub_00010008)', out)
        self.assertIn('ctx.r3.u64 = 0x1u;', out)
        self.assertIn('__imp__sub_00010008(ctx, base);', out)
        self.assertIn('HookFlag("FM4_TEST_HOOK")', out)
        self.assertIn('int TitleHookCount() { return 1; }', out)

    def test_no_hooks(self):
        self.assertIn('int TitleHookCount() { return 0; }', self.gen.title_hooks('', bytes(16), 0x10000))

    def test_wrong_hash_refused(self):
        image, text = self.config(sha='0' * 64)
        with self.assertRaises(SystemExit) as e:
            self.gen.title_hooks(text, image, 0x10000)
        self.assertIn('hash to', str(e.exception))

    def test_outside_image_refused(self):
        image, text = self.config(function=0x10040)
        with self.assertRaises(SystemExit) as e:
            self.gen.title_hooks(text, image, 0x10000)
        self.assertIn('outside the image', str(e.exception))

    def test_bad_flag_or_size_refused(self):
        for kwargs in ({'flag': 'NOT_FM4'}, {'size': 6}):
            image, text = self.config(**kwargs)
            with self.assertRaises(SystemExit):
                self.gen.title_hooks(text, image, 0x10000)

    def press_config(self, stop_sha=None):
        import hashlib
        image = bytes(range(64))
        sha = hashlib.sha256(image[8:16]).hexdigest()
        stop_sha = stop_sha or hashlib.sha256(image[32:40]).hexdigest()  # the 8 bytes at base + 32 (hand: 20 21 .. 27)
        return image, (f'[[hook]]\nname = "press"\nkind = "stop_on_press"\nfunction = 0x10008\nflag = "FM4_TEST_HOOK"\nbytes = 8\n'
                       f'sha256 = "{sha}"\ncaller = 0x10104\nstop = 0x10020\nstop_bytes = 8\nstop_sha256 = "{stop_sha}"\n')

    def test_stop_on_press_generated(self):
        image, text = self.press_config()
        out = self.gen.title_hooks(text, image, 0x10000)
        self.assertIn('uint32_t(ctx.lr) == 0x00010104u', out)
        self.assertIn('runtime::input::APresses()', out)
        self.assertIn('sub_00010020(ctx, base);', out)
        self.assertIn('ctx.r3.u64 = 0;', out)
        self.assertIn('__imp__sub_00010008(ctx, base);', out)

    def test_stop_on_press_checks_the_stop_function(self):
        image, text = self.press_config(stop_sha='0' * 64)
        with self.assertRaises(SystemExit) as e:
            self.gen.title_hooks(text, image, 0x10000)
        self.assertIn('(stop)', str(e.exception))

    def test_unknown_kind_refused(self):
        image, text = self.config()
        with self.assertRaises(SystemExit):
            self.gen.title_hooks(text.replace('returns = 1', 'kind = "other"\nreturns = 1'), image, 0x10000)


if __name__ == '__main__':
    unittest.main(verbosity=2)
