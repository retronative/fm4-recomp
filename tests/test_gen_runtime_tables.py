# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of scripts/gen_runtime_tables.py on a synthetic survey and ppc/ (no game data): the image
# check, the import-kind check, the thunk/declaration cross-check, the strict stubs and the import
# records. Expected text is written by hand from the inputs.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_gen_runtime_tables.py
import json, os, struct, subprocess, sys, tempfile, unittest, zlib
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
    s.mkdir(); p.mkdir()
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
    (p / 'ppc_config.h').write_text('#define PPC_IMAGE_BASE 0x40000000ull\n#define PPC_IMAGE_SIZE 0x200ull\n')
    (p / 'ppc_recomp_shared.h').write_text(''.join(f'PPC_EXTERN_FUNC({d});\n' for d in declared))
    return s, p, o


def run(s, p, o):
    r = subprocess.run([sys.executable, '-B', '-I', str(SCRIPT), str(s), str(p), str(s.parent / 'default.xex'), str(o)],
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
