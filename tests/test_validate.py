#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Tests for scripts/validate.py on a synthetic translation (no game bytes, no ppc/ needed).

    /opt/homebrew/bin/python3 -B -I tests/test_validate.py [Case.test_name ...]

The world: a 0x330-byte image at 0x82000000 with a code section [0x82000100, 0x82000300), a data
section holding one code pointer, and .pdata for four functions, plus the ppc_functions.txt lines,
generated C++ text, recomp.log and switch tables a clean translation of it would have. Words are
hand-encoded from the PowerPC ISA (Book I) field layouts; every expected address and message is
worked out by hand from the layout below, never from validate.py's output.

  word  address     content
   0    82000100    F1 (.pdata, 13 words): cmplwi cr6,r4,1
   1    82000104      bgt cr6,0x82000130 (default)
   2-6  82000108      lis r12,0x8200; slwi r0,r4,2; addi r12,r12,0x120; lwzx r0,r12,r0; mtctr r0
   7    8200011C      bctr                     (proven switch, r4, 2 cases)
   8-9  82000120      table: 82000128, 8200012C (inline, data)
  10-12 82000128      blr (case 0); blr (case 1); blr (default)
  13    82000134    F2 (.pdata, 7 words): li r3,0
  14    82000138      addi r3,r3,1             (loop head)
  15    8200013C      cmplwi cr0,r3,4
  16    82000140      blt cr0,0x82000138       (backward branch)
  17    82000144      blr
  18    82000148      li r3,5                  (secondary entry E: after a blr, pointed to by .rdata)
  19    8200014C      blr
  20    82000150    F3 (.pdata, 1 word): blr    (runtime routine switch_fiber)
  21    82000154    F4 (.pdata, 10 words): cmplwi cr6,r5,1; bgt cr6,0x82000178
  23-27 8200015C      lis r12,0x8200; slwi r0,r5,2; addi r12,r12,0x1A0; lwzx r0,r12,r0; mtctr r0
  28    82000170      bctr                     (proven switch, r5, 2 cases, table outside F4)
  29-30 82000174      blr (case 0); blr (case 1 and default)
  31-34 8200017C    import thunk (4 non-zero words)
  35-39 8200018C    zero padding
  40-41 820001A0    table: 82000174, 82000178 (outside every function)
  .rdata 82000300: 82000148; .pdata 82000310: (82000100, 13), (82000134, 7), (82000150, 1),
  (82000154, 10), ExceptionFlag clear.
"""
import os
import re
import struct
import sys
import unittest
from contextlib import redirect_stdout
from io import StringIO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import validate as V  # noqa: E402

BASE, T = 0x82000000, 0x82000100
BLR, BCTR = 0x4E800020, 0x4E800420


def cmplwi(crf, ra, uimm): return 10 << 26 | crf << 23 | ra << 16 | uimm
def bc(bo, bi, disp): return 16 << 26 | bo << 21 | bi << 16 | (disp & 0xFFFC)
def lis(rd, imm): return 15 << 26 | rd << 21 | imm
def addi(rd, ra, imm): return 14 << 26 | rd << 21 | ra << 16 | (imm & 0xFFFF)
def slwi(ra, rs, n): return 21 << 26 | rs << 21 | ra << 16 | n << 11 | (31 - n) << 1
def lwzx(rt, ra, rb): return 31 << 26 | rt << 21 | ra << 16 | rb << 11 | 23 << 1
def mtctr(rs): return 0x7C0903A6 | rs << 21


TEXT = [
    cmplwi(6, 4, 1), bc(12, 25, 44), lis(12, 0x8200), slwi(0, 4, 2), addi(12, 12, 0x120), lwzx(0, 12, 0),
    mtctr(0), BCTR, 0x82000128, 0x8200012C, BLR, BLR, BLR,
    addi(3, 0, 0), addi(3, 3, 1), cmplwi(0, 3, 4), bc(12, 0, -8), BLR, addi(3, 0, 5), BLR,
    BLR,
    cmplwi(6, 5, 1), bc(12, 25, 32), lis(12, 0x8200), slwi(0, 5, 2), addi(12, 12, 0x1A0), lwzx(0, 12, 0),
    mtctr(0), BCTR, BLR, BLR,
    0x00000060, 0x00000060, 0x00000060, 0x2000804E,
    0, 0, 0, 0, 0,
    0x82000174, 0x82000178,
]
FUNCTIONS = [(0x82000100, 0x34, 'pdata'), (0x82000134, 0x1C, 'pdata'), (0x82000150, 0x4, 'pdata'), (0x82000154, 0x28, 'pdata')]
ENTRIES = [(0x82000148, 0x82000134)]
CONFIG = {'runtime_routines': [{'address': 0x82000150, 'name': 'switch_fiber'}]}
TABLES = [
    {'base': 0x8200011C, 'r': 4, 'table': 0x82000120, 'table_size': 8, 'labels': [0x82000128, 0x8200012C]},
    {'base': 0x82000170, 'r': 5, 'table': 0x820001A0, 'table_size': 8, 'labels': [0x82000174, 0x82000178]},
]
SOURCES = {'ppc_recomp.0.cpp': '''#include "ppc_recomp_shared.h"

PPC_FUNC_IMPL(__imp__sub_82000100) {
\tPPC_FUNC_PROLOGUE();
\tswitch (ctx.r4.u32) {
\tcase 0:
\t\tgoto loc_82000128;
\tcase 1:
\t\tgoto loc_8200012C;
\tdefault:
\t\t__builtin_trap();
\t}
loc_82000128:
\treturn;
loc_8200012C:
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_82000134) {
\tPPC_FUNC_PROLOGUE();
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_82000148) {
\tPPC_FUNC_PROLOGUE();
\treturn;
}

PPC_FUNC_IMPL(__imp__sub_82000150) {
\tPPC_FUNC_PROLOGUE();
\tPpcRuntime_switch_fiber(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82000154) {
\tPPC_FUNC_PROLOGUE();
\treturn;
}
'''}
LOG = 'NOTE: function 82000150 is handled by the runtime (PpcRuntime_switch_fiber)\n'


def world(text=TEXT):
    image = bytearray(0x330)
    for i, w in enumerate(text):
        struct.pack_into('>I', image, 0x100 + 4 * i, w)
    struct.pack_into('>I', image, 0x300, 0x82000148)
    for i, (start, size, _) in enumerate(FUNCTIONS):
        struct.pack_into('>II', image, 0x310 + 8 * i, start, (size // 4) << 8)
    report = {'base': BASE, 'size': 0x330, 'entry': T, 'export_table': 0, 'helpers': {},
              'sections': [{'name': '.text', 'start': T, 'end': 0x82000300, 'code': True},
                           {'name': '.rdata', 'start': 0x82000300, 'end': 0x82000310, 'code': False},
                           {'name': '.pdata', 'start': 0x82000310, 'end': 0x82000330, 'code': False}],
              'imports': [{'name': '__imp__Mutant', 'address': 0x8200017C}], 'import_slots': []}
    return report, bytes(image), (list(FUNCTIONS), list(ENTRIES)), dict(SOURCES), LOG


def check(report, image, analysis, sources=SOURCES, log=LOG, config=CONFIG, tables=TABLES):
    return V.check(report, image, analysis, sources, log, config, tables)[0]


class Encoding(unittest.TestCase):
    def test_words_match_independent_encodings(self):
        # bctr/blr/mtctr r0 as listed in the ISA's extended mnemonics; lis r12,0x8200 = addis.
        self.assertEqual(mtctr(0), 0x7C0903A6)
        self.assertEqual(lis(12, 0x8200), 0x3D808200)
        self.assertEqual(bc(12, 25, 44), 0x4199002C)  # bgt cr6,+0x2C


class Clean(unittest.TestCase):
    def test_clean_translation_passes_every_check(self):
        report, image, analysis, _, _ = world()
        self.assertEqual({k: v for k, v in check(report, image, analysis).items() if v}, {})


class Faults(unittest.TestCase):
    """One fault each, with the failure written out by hand."""

    def setUp(self):
        self.report, self.image, self.analysis, _, _ = world()

    def test_pdata_size_mismatch(self):
        functions = [(0x82000100, 0x30, 'pdata')] + FUNCTIONS[1:]
        self.assertEqual(check(self.report, self.image, (functions, ENTRIES))['pdata_functions'], ['82000100'])

    def test_overlap(self):
        functions = FUNCTIONS + [(0x82000104, 4, 'sweep')]
        self.assertEqual(check(self.report, self.image, (functions, ENTRIES))['no_overlap'], ['82000100+34 overlaps 82000104'])

    def test_address_taken_entry_missing(self):
        self.assertEqual(check(self.report, self.image, (FUNCTIONS, []))['address_taken'], ['82000148'])

    def test_padding_word_made_code(self):
        image = self.image[:0x18C] + struct.pack('>I', 0x60000000) + self.image[0x190:]
        self.assertEqual(check(self.report, image, self.analysis)['coverage'], ['8200018C'])

    def test_function_over_thunk(self):
        functions = FUNCTIONS + [(0x8200017C, 16, 'sweep')]
        self.assertEqual(check(self.report, self.image, (functions, ENTRIES))['no_code_over_data'],
                         ['8200017C covers import thunk at 8200017C'])

    def test_switch_table_words_differ_from_labels(self):
        tables = [dict(TABLES[0], labels=TABLES[0]['labels'][::-1]), TABLES[1]]
        self.assertEqual(check(self.report, self.image, self.analysis, tables=tables)['switch_tables'],
                         ['8200011C: table words at 82000120 differ from its labels'])

    def test_unproven_entry_with_data_range(self):
        tables = [dict(TABLES[0], labels=[], table_size=8), TABLES[1]]
        self.assertEqual(check(self.report, self.image, self.analysis, tables=tables)['switch_tables'],
                         ['8200011C: unproven, but its table is a 8-byte data range'])

    def test_unaligned_label(self):
        tables = [TABLES[0], dict(TABLES[1], table_size=0, labels=[0x82000176])]
        self.assertEqual(check(self.report, self.image, self.analysis, tables=tables)['switch_tables'],
                         ['82000170: case 82000176 is not aligned code'])

    def test_inline_table_as_entry(self):
        entries = ENTRIES + [(0x82000120, 0x82000100)]
        sources = {**SOURCES, 'm.cpp': '\nPPC_FUNC_IMPL(__imp__sub_82000120) {\n}\n'}
        self.assertEqual(check(self.report, self.image, (FUNCTIONS, entries), sources)['secondary_entries'],
                         ['82000120 is the switch table after 8200011C'])

    def test_loop_split_is_a_tail_cycle(self):
        functions = [FUNCTIONS[0], (0x82000134, 8, 'pdata'), (0x8200013C, 0x14, 'pdata')] + FUNCTIONS[2:]
        self.assertEqual(check(self.report, self.image, (functions, ENTRIES))['no_tail_cycles'], ['82000134 -> 8200013C'])

    def test_runtime_routine_body(self):
        sources = {n: t.replace('PpcRuntime_switch_fiber(ctx, base);', 'return;') for n, t in SOURCES.items()}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['runtime_routines'],
                         ['82000150: body is not only its runtime handling'])

    def test_upstream_drop_comment(self):
        sources = {**SOURCES, 'm.cpp': '\t// ERROR 82000000\n'}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['no_error_markers'], ['m.cpp: // ERROR 82000000'])

    def test_every_fatal_fails(self):
        log = LOG + 'FATAL: unrecognized instruction at 0x82000104: frsqrte\nUnable to decode instruction 0 at 82000108\n'
        self.assertEqual(check(self.report, self.image, self.analysis, log=log)['no_fatal'],
                         ['FATAL: unrecognized instruction at 0x82000104: frsqrte', 'Unable to decode instruction 0 at 82000108'])

    def test_undefined_label(self):
        sources = {n: t.replace('loc_8200012C:\n', '') for n, t in SOURCES.items()}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['defined_labels'],
                         ['__imp__sub_82000100: loc_8200012C'])

    def test_function_over_foreign_switch_table(self):
        functions = FUNCTIONS + [(0x820001A0, 8, 'sweep')]
        self.assertEqual(check(self.report, self.image, (functions, ENTRIES))['no_code_over_data'],
                         ['820001A0 covers switch table of 82000170 at 820001A0'])

    def test_entry_outside_owner(self):
        self.assertEqual(check(self.report, self.image, (FUNCTIONS, [(0x82000148, 0x82000100)]))['secondary_entries'],
                         ['82000148 not inside 82000100'])

    def test_entry_not_emitted(self):
        sources = {n: t.replace('PPC_FUNC_IMPL(__imp__sub_82000148)', 'PPC_FUNC_IMPL(__imp__sub_8200014C)') for n, t in SOURCES.items()}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['secondary_entries'], ['82000148 not emitted'])

    def test_runtime_routine_with_entry(self):
        entries = ENTRIES + [(0x82000154, 0x82000150)]
        self.assertEqual(check(self.report, self.image, (FUNCTIONS, entries))['runtime_routines'],
                         ['82000150: has a secondary entry'])

    def test_context_header_guard_allowed(self):
        sources = {**SOURCES, 'ppc_context.h': '#error "unsupported compiler"\n'}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['no_error_markers'], [])

    def test_recompiler_error_marker(self):
        sources = {**SOURCES, 'm.cpp': '#error "XenonRecomp: x"\n'}
        self.assertEqual(check(self.report, self.image, self.analysis, sources)['no_error_markers'], ['m.cpp:  "XenonRecomp: x"'])


# An export table (Xenia xex2_export_table layout: magic 0x48000000, "HVE", 0x48000000, two module
# words, three version words, image base >> 16, count, ordinal base, one offset per export) in
# the padding at 0x820001A8: 12 non-zero words.
EXPORT = [0x48000000, 0x00485645, 0x48000000, 1, 2, 3, 4, 5, 0x8200, 1, 1, 0x154]


class ResidualStops(unittest.TestCase):
    """Patch 0035: residual stops equal the configured list (each failure written out by hand)."""
    STOP = '\tPpcResidualStop(0x82000200u, "stop_a", ctx.r3.u32, uint32_t(ctx.lr));\n'
    UNPROVEN = dict(base=0x82000200, r=3, labels=[], table=0x82000204, table_size=0)
    LISTED = dict(CONFIG, residual_stops=[{'address': 0x82000200, 'name': 'stop_a'}])

    def setUp(self):
        self.report, self.image, self.analysis, _, _ = world()

    def residual(self, sources=None, config=None, tables=None):
        return check(self.report, self.image, self.analysis, sources if sources is not None else {**SOURCES, 'stop.cpp': self.STOP},
                     LOG, config if config is not None else self.LISTED,
                     tables if tables is not None else TABLES + [self.UNPROVEN])['residual_stops']

    def test_listed_stop_passes(self):
        self.assertEqual(self.residual(), [])

    def test_stop_not_listed(self):
        self.assertEqual(self.residual(config=CONFIG), ['82000200: unproven switch table not listed',
                                                       '82000200: stop emitted, but not listed'])

    def test_listed_but_not_emitted(self):
        self.assertEqual(self.residual(sources=SOURCES), ['82000200: listed, but no stop emitted'])

    def test_name_differs(self):
        self.assertEqual(self.residual(sources={**SOURCES, 'stop.cpp': self.STOP.replace('stop_a', 'stop_b')}),
                         ['82000200: stop named stop_b, listed as stop_a'])

    def test_listed_proven_table(self):
        self.assertEqual(self.residual(tables=TABLES), ['82000200: listed, but not an unproven switch table'])


class ExportTable(unittest.TestCase):
    def world(self, words):
        report, image, analysis, _, _ = world(TEXT + words)
        return dict(report, export_table=0x820001A8), image, analysis

    def test_export_table_explained(self):
        self.assertEqual(check(*self.world(EXPORT))['coverage'], [])

    def test_wrong_image_base_not_explained(self):
        coverage = check(*self.world(EXPORT[:8] + [0x8201] + EXPORT[9:]))['coverage']
        self.assertEqual(coverage, [f'{0x820001A8 + 4 * i:08X}' for i in range(12)])

    def test_wrong_magic_not_explained(self):
        coverage = check(*self.world([0x48000001] + EXPORT[1:]))['coverage']
        self.assertEqual(len(coverage), 12)


class SelfTest(unittest.TestCase):
    def run_self_test(self, *data):
        out = StringIO()
        with redirect_stdout(out):
            ok = V.self_test(*data, CONFIG, TABLES)
        return ok, out.getvalue()

    def test_every_injection_detected(self):
        ok, out = self.run_self_test(*world())
        self.assertTrue(ok, out)
        self.assertNotIn('FAIL', out)
        names = re.findall(r'^self-test (\S+(?: \S+)?): detected', out, re.M)
        self.assertEqual(len(names), 19, out)

    def test_missing_fixture_is_a_failure(self):
        # The loop's blt (word 16) made a nop: no backward branch, no loop to split.
        text = list(TEXT)
        text[16] = 0x60000000
        ok, out = self.run_self_test(*world(text))
        self.assertFalse(ok)
        self.assertIn('FAIL self-test no_tail_cycles: fixture missing', out)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:], verbosity=2)
