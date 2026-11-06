# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of scripts/check_module_symbols.py on hand-written nm output and headers. Expected names are
# written by hand from the Itanium C++ ABI mangling (_Z <length> <identifier> <parameters>) and Mach-O's
# leading underscore: `void sub_88050000(PPCContext&, unsigned char*)` is __Z12sub_88050000R10PPCContextPh.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_check_module_symbols.py
import sys, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import check_module_symbols as C  # noqa: E402

EXE = 'PPC_EXTERN_FUNC(sub_82000000);\nPPC_EXTERN_FUNC(_xstart);\nPPC_EXTERN_FUNC(__imp__KeA);\n'
MOD = 'PPC_EXTERN_FUNC(sub_88050000);\nPPC_EXTERN_FUNC(_xstart);\nPPC_EXTERN_FUNC(__imp__KeA);\n'
INLINE = '__ZN5simde3addEii'  # an inline helper from a shared header (not a translated name)


class Names(unittest.TestCase):
    def test_identifier(self):
        self.assertEqual(C.identifier('__Z12sub_88050000R10PPCContextPh'), 'sub_88050000')
        self.assertEqual(C.identifier('__Z7_xstartR10PPCContextPh'), '_xstart')
        self.assertEqual(C.identifier('___imp___xstart'), '__imp___xstart')  # extern "C": underscore only
        self.assertEqual(C.identifier('_PPCFuncMappings'), 'PPCFuncMappings')

    def test_defined_skips_headers(self):
        self.assertEqual(C.defined('\nlib.a(ppc_recomp.0.cpp.o):\n___imp__sub_1\n__Z5sub_1R10PPCContextPh\n\n'),
                         {'___imp__sub_1', '__Z5sub_1R10PPCContextPh'})

    def test_translated(self):
        self.assertEqual(C.translated(EXE), {'sub_82000000', '_xstart', '__imp__KeA', '__imp__sub_82000000',
                                             '__imp___xstart', '__imp____imp__KeA', 'PPCFuncMappings'})


class Check(unittest.TestCase):
    def libs(self, exe_symbols, mod_symbols, contexts=(b'ctx', b'ctx')):
        return [('fm4_ppc', exe_symbols, C.translated(EXE), contexts[0]),
                ('m_ppc', mod_symbols, C.translated(MOD), contexts[1])]

    def test_renamed_modules_pass(self):
        exe = {'__Z7_xstartR10PPCContextPh', '___imp___xstart', '_PPCFuncMappings', INLINE}
        mod = {'__Z9m__xstartR10PPCContextPh', '_m___imp___xstart', '_m_PPCFuncMappings', INLINE}
        self.assertEqual(C.check(self.libs(exe, mod)), [])

    def test_duplicate_translated_function_fails(self):
        both = {'__Z7_xstartR10PPCContextPh'}
        problems = C.check(self.libs(both | {INLINE}, both | {INLINE}))
        self.assertEqual(problems, ['__Z7_xstartR10PPCContextPh (_xstart) is defined by fm4_ppc, m_ppc'])

    def test_duplicate_body_and_table_fail(self):
        both = {'___imp___xstart', '_PPCFuncMappings'}
        self.assertEqual(len(C.check(self.libs(both, both))), 2)

    def test_inline_duplicate_needs_identical_context(self):
        self.assertEqual(C.check(self.libs({INLINE}, {INLINE})), [])
        problems = C.check(self.libs({INLINE}, {INLINE}, contexts=(b'ctx', b'other')))
        self.assertEqual(problems, ["1 shared definitions, but the modules' ppc_context.h differ"])


if __name__ == '__main__':
    unittest.main(verbosity=2)
