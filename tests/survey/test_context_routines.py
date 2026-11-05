#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Tests for survey/context_routines.py on synthetic words (no game bytes).

    /opt/homebrew/bin/python3 -B -I tests/survey/test_context_routines.py [TestCase.test_name ...]

Encodings come from the PowerPC ISA (Book I): std/ld are DS-form (primary 62/58, low 2 bits 0;
1 = stdu/ldu), stw/lwz D-form (36/32), b I-form (18: LI, AA, LK), bc B-form (16: BO, BI, BD, AA,
LK). The encoders are anchored to two words written independently of this code: the compiler
helper patterns 'std r14,-0x98(r1)' = 0xF9C1FF68 and 'ld r14,-0x98(r1)' = 0xE9C1FF68 (XenonRecomp
README, survey/xexsurvey.cpp). Every expected shape is worked out by hand from the rule: one base
register other than r1, r1 plus at least 17 of r14-r31 (r1 not needed on the load side of a
switch), update forms not counted.
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "survey"))
import context_routines as C  # noqa: E402

BASE = 0x82000000


def ds(op, rs, ra, d, xo=0):
    return op << 26 | rs << 21 | ra << 16 | (d & 0xFFFC) | xo


def std(rs, ra, d):
    return ds(62, rs, ra, d)


def ld(rt, ra, d):
    return ds(58, rt, ra, d)


def stw(rs, ra, d):
    return 36 << 26 | rs << 21 | ra << 16 | (d & 0xFFFF)


def lwz(rt, ra, d):
    return 32 << 26 | rt << 21 | ra << 16 | (d & 0xFFFF)


NOP, BLR = 0x60000000, 0x4E800020


def words_at(address, seq):
    return {address + 4 * i: w for i, w in enumerate(seq)}


def word_of(*maps):
    merged = {}
    for m in maps:
        merged.update(m)
    return lambda a: merged.get(a, 0)


def save(base_reg, regs, r1=True, op=std):
    return [op(1, base_reg, 0)] * r1 + [op(r, base_reg, 8 * (i + 1)) for i, r in enumerate(regs)]


R14_31 = list(range(14, 32))  # 18 registers


class Encoders(unittest.TestCase):
    def test_encoders_match_independent_words(self):
        self.assertEqual(std(14, 1, -0x98), 0xF9C1FF68)
        self.assertEqual(ld(14, 1, -0x98), 0xE9C1FF68)

    def test_image_word(self):
        word = C.image_word(bytes(range(1, 9)), 0x100)
        self.assertEqual([word(0x100), word(0x104)], [0x01020304, 0x05060708])
        # Outside the image, including a partial word at its end, reads as 0.
        self.assertEqual([word(0xFC), word(0x106), word(0x108)], [0, 0, 0])


class Shape(unittest.TestCase):
    def shape(self, seq, kind, window=320):
        return C.context_shape(word_of(words_at(BASE, seq)), BASE, kind, window)

    def test_store_shape(self):
        seq = save(3, R14_31) + [BLR]
        self.assertTrue(self.shape(seq, 'store'))
        self.assertFalse(self.shape(seq, 'load'))
        self.assertFalse(self.shape(seq, 'switch'))

    def test_17_registers_suffice_16_do_not(self):
        self.assertTrue(self.shape(save(3, R14_31[:17]), 'store'))
        self.assertFalse(self.shape(save(3, R14_31[:16]), 'store'))

    def test_r1_required(self):
        self.assertFalse(self.shape(save(3, R14_31, r1=False), 'store'))

    def test_base_r1_not_counted(self):
        self.assertFalse(self.shape(save(1, R14_31), 'store'))

    def test_update_forms_not_counted(self):
        stdu = [ds(62, r, 3, 8 * i, xo=1) for i, r in enumerate([1] + R14_31)]
        self.assertFalse(self.shape(stdu, 'store'))
        ldu = [ds(58, r, 3, 8 * i, xo=1) for i, r in enumerate([1] + R14_31)]
        self.assertFalse(self.shape(ldu, 'load'))

    def test_one_base_register(self):
        seq = save(3, R14_31[:9]) + save(4, R14_31[9:])
        self.assertFalse(self.shape(seq, 'store'))

    def test_32_bit_forms(self):
        self.assertTrue(self.shape(save(3, R14_31, op=stw), 'store'))
        self.assertTrue(self.shape(save(3, R14_31, op=lwz), 'load'))

    def test_load_shape(self):
        self.assertTrue(self.shape(save(3, R14_31, op=ld), 'load'))
        self.assertFalse(self.shape(save(3, R14_31, op=ld), 'store'))

    def test_switch_shape(self):
        # stores r1 + 18 through r3, loads 18 (no r1) through r4.
        seq = save(3, R14_31) + save(4, R14_31, r1=False, op=ld)
        self.assertTrue(self.shape(seq, 'switch'))
        self.assertTrue(self.shape(seq, 'store'))
        self.assertFalse(self.shape(seq, 'load'))
        self.assertFalse(self.shape(save(3, R14_31), 'switch'))

    def test_window_boundary(self):
        # r1 + 17 stores = 18 words; with 302 nops first the last store is word 319: inside a
        # 320-word window; one more nop puts it at word 320, outside.
        seq = save(3, R14_31[:17])
        self.assertTrue(self.shape([NOP] * 302 + seq, 'store'))
        self.assertFalse(self.shape([NOP] * 303 + seq, 'store'))

    def test_function_extent_is_the_window(self):
        seq = save(3, R14_31)  # 19 words
        word = word_of(words_at(BASE, seq))
        self.assertEqual(C.context_routines(word, [(BASE, 4 * 19)]), {BASE})
        self.assertEqual(C.context_routines(word, [(BASE, 4 * 17)]), set())  # r1 + 16 registers
        self.assertEqual(C.context_routines(word, [(BASE, 0)]), set())  # a secondary entry line


class Branches(unittest.TestCase):
    def test_targets_and_calls(self):
        seq = [
            18 << 26 | 8,                           # BASE+0x0: b +8          -> BASE+0x8
            18 << 26 | (-4 & 0x3FFFFFC) | 1,        # BASE+0x4: bl -4         -> BASE+0x0, call
            16 << 26 | 12 << 21 | 2 << 16 | 0x10,   # BASE+0x8: beq +0x10     -> BASE+0x18
            18 << 26 | 0x100 | 2,                   # BASE+0xC: ba 0x100      -> 0x100 (absolute)
            BLR,
        ]
        targets = C.branch_targets(word_of(words_at(BASE, seq)), [(BASE, BASE + 4 * len(seq))])
        self.assertEqual(targets, {BASE + 8: [(BASE, False)], BASE: [(BASE + 4, True)],
                                   BASE + 0x18: [(BASE + 8, False)], 0x100: [(BASE + 0xC, False)]})


class Configured(unittest.TestCase):
    def setUp(self):
        self.store_at, self.caller = BASE, BASE + 0x1000
        routine = words_at(self.store_at, save(3, R14_31) + [BLR])
        call = words_at(self.caller, [18 << 26 | (-0x1000 & 0x3FFFFFC) | 1])  # bl BASE
        self.word = word_of(routine, call)
        self.targets = C.branch_targets(self.word, [(BASE, BASE + 0x1004)])

    def test_kinds_from_config(self):
        config = {'setjmp_address': 1, 'longjmp_address': 2,
                  'runtime_routines': [{'address': 3, 'name': 'switch_fiber'},
                                       {'address': 4, 'name': 'restore_context', 'noreturn': True}]}
        self.assertEqual(C.configured_routines(config), [(1, 'store', 'setjmp_address'), (2, 'load', 'longjmp_address'),
                                                         (3, 'switch', 'switch_fiber'), (4, 'load', 'restore_context')])

    def test_setjmp_shaped_and_called(self):
        self.assertEqual(C.check_configured(self.word, self.targets, [(self.store_at, 'store', 'setjmp_address')]), [])

    def test_wrong_kind(self):
        problems = C.check_configured(self.word, self.targets, [(self.store_at, 'load', 'longjmp_address')])
        self.assertEqual(problems, ['longjmp_address at 0x82000000 does not have the context load shape'])

    def test_not_a_branch_target(self):
        problems = C.check_configured(self.word, {}, [(self.store_at, 'store', 'setjmp_address')])
        self.assertEqual(problems, ['setjmp_address at 0x82000000 is not the target of any direct branch or call'])

    def test_evidence_row(self):
        rows = C.evidence(self.word, [(self.store_at, 4 * 20)], self.targets, {self.store_at: 2})
        self.assertEqual(rows, [{'address': '82000000', 'size': 80, 'kinds': ['store'], 'calls': 1, 'branches': 0,
                                 'data_refs': 2, 'stores': {'r3': (True, 18)}, 'loads': {}}])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:], verbosity=2)
