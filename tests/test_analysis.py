# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Function-analysis tests: synthetic XEX images (tests/analysis/xexgen.py) run through the real
# XenonRecomp binary with a small config. Checks: exit code, FATAL lines, the generated function
# mapping (address -> function), and, from patch 0016, ppc_functions.txt (address, size, source).
# Expected values are written by hand from the words each case lays out, never from the output.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_analysis.py [test_name ...]
#
# With names, only those tests run (the focused run per change); without, all of them.
import os
import re
import shutil
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "analysis"))
import xexgen as X  # noqa: E402

XR = os.path.join(ROOT, "tools", "XenonRecomp")
RECOMP = os.path.join(XR, "build", "XenonRecomp", "XenonRecomp")
CONTEXT_H = os.path.join(XR, "XenonUtils", "ppc_context.h")
WORK = os.path.join(ROOT, "build", "tests", "analysis")


class Result:
    def __init__(self, code, log, out):
        self.code, self.log, self.out = code, log, out
        self.fatal = [l[len("FATAL: "):] for l in log.splitlines() if l.startswith("FATAL: ")]
        mapping = ""
        path = os.path.join(out, "ppc_func_mapping.cpp")
        if os.path.exists(path):
            mapping = open(path).read()
        self.functions = sorted(int(a, 16) for a in re.findall(r"\{\s*0x([0-9A-Fa-f]+),", mapping))
        self.listed = {}
        listing = os.path.join(out, "ppc_functions.txt")
        if os.path.exists(listing):
            for line in open(listing):
                parts = line.split()
                if len(parts) == 3:
                    self.listed[int(parts[0], 16)] = (int(parts[1], 16), parts[2])

    def instruction_counts(self):
        """Instructions per generated function body, in emission (address) order: each translated
        instruction is preceded by a '// <mnemonic>' comment line."""
        counts = []
        for body in re.split(r"^PPC_FUNC_IMPL\(", self.code_text(), flags=re.M)[1:]:
            body = body.split("\n}\n", 1)[0]
            counts.append(len(re.findall(r"^\t// \S", body, re.M)))
        return counts

    def code_text(self):
        text = ""
        for name in sorted(os.listdir(self.out)):
            if name.startswith("ppc_recomp") and name.endswith(".cpp"):
                text += open(os.path.join(self.out, name)).read()
        return text


def recompile(case, sections, entry, extra_config="", export_table_address=0):
    d = os.path.join(WORK, case)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "ppc"))
    with open(os.path.join(d, "case.xex"), "wb") as f:
        f.write(X.build(sections, entry, export_table_address=export_table_address))
    with open(os.path.join(d, "case.toml"), "w") as f:
        f.write('[main]\nfile_path = "case.xex"\nout_directory_path = "ppc"\n' + extra_config)
    p = subprocess.run([RECOMP, os.path.join(d, "case.toml"), CONTEXT_H], cwd=d, capture_output=True,
                       text=True, timeout=120)
    with open(os.path.join(d, "recomp.log"), "w") as f:
        f.write(p.stdout + p.stderr)
    return Result(p.returncode, p.stdout + p.stderr, os.path.join(d, "ppc"))


T = 0x82001000  # .text at RVA 0x1000
P = 0x2000      # .pdata RVA


class Analysis(unittest.TestCase):
    def test_baseline_discovery(self):
        """pdata functions A, B; C found only as a bl target; D found only by the sweep.
        A @T: bl C; blr (pdata 2)   B @T+8: li r3,2; blr (pdata 2)
        C @T+0x10: li r3,3; blr     D @T+0x18: li r3,4; blr"""
        A, B, C, D = T, T + 8, T + 0x10, T + 0x18
        text = X.words(X.bl(A, C), X.BLR, X.li(3, 2), X.BLR, X.li(3, 3), X.BLR, X.li(3, 4), X.BLR)
        r = recompile("baseline_discovery",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((A, 2), (B, 2)), X.DATA)], A)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.fatal, [])
        self.assertEqual(r.functions, [A, B, C, D])
        self.assertEqual(r.instruction_counts(), [2, 2, 2, 2])

    def test_function_sources(self):
        """Patch 0016: ppc_functions.txt lists each function's address, size and the analysis step
        that found it. Same layout as test_baseline_discovery: A, B from .pdata (2 instructions =
        8 bytes each), C only as a bl target, D only by the sweep."""
        A, B, C, D = T, T + 8, T + 0x10, T + 0x18
        text = X.words(X.bl(A, C), X.BLR, X.li(3, 2), X.BLR, X.li(3, 3), X.BLR, X.li(3, 4), X.BLR)
        r = recompile("function_sources",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((A, 2), (B, 2)), X.DATA)], A)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed, {A: (8, "pdata"), B: (8, "pdata"), C: (8, "bl_target"), D: (8, "sweep")})

    def test_helper_sources(self):
        """Patch 0016: register-restore helper entries are tagged "helper". restgprlr_14 at
        T+0x20 (after A..D); entry i (14..31) at +(i-14)*4, restoring r_i..r31 plus lwz, mtlr, blr:
        (32-i)*4+12 bytes."""
        A, B, C, D, H = T, T + 8, T + 0x10, T + 0x18, T + 0x20
        self.assertEqual(X.restgprlr_14()[0], 0xE9C1FF68)  # XenonRecomp's own search pattern
        text = X.words(X.bl(A, C), X.BLR, X.li(3, 2), X.BLR, X.li(3, 3), X.BLR, X.li(3, 4), X.BLR,
                       *X.restgprlr_14())
        r = recompile("helper_sources",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((A, 2), (B, 2)), X.DATA)], A,
                      "restgprlr_14_address = 0x%X\n" % H)
        self.assertEqual(r.code, 0, r.log[-2000:])
        for i in range(14, 32):
            self.assertEqual(r.listed.get(H + (i - 14) * 4), ((32 - i) * 4 + 12, "helper"), "restgprlr_%d" % i)

    def test_exception_metadata_is_data(self):
        """Patch 0017: a .pdata function with ExceptionFlag is preceded by two data words, the
        handler's address and its data. H @T: li r3,9; blr (pdata, the handler); metadata @T+8:
        H, 0x12345678; E @T+0x10: li r3,1; blr (pdata, ExceptionFlag). The metadata is no
        function: functions H and E only, 2 instructions each."""
        H, E = T, T + 0x10
        text = X.words(X.li(3, 9), X.BLR, H, 0x12345678, X.li(3, 1), X.BLR)
        r = recompile("exception_metadata_is_data",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((H, 2), (E, 2, 0, 1)), X.DATA)], H)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.functions, [H, E])
        self.assertEqual(r.instruction_counts(), [2, 2])

    def test_exception_handler_not_in_code(self):
        """Patch 0017: a handler word that points outside code (here .rdata at RVA 0x3000) is
        fatal."""
        H, E, R = T, T + 0x10, 0x82003000
        text = X.words(X.li(3, 9), X.BLR, R, 0x12345678, X.li(3, 1), X.BLR)
        r = recompile("exception_handler_not_in_code",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((H, 2), (E, 2, 0, 1)), X.DATA),
                       (".rdata", 0x3000, X.words(0, 0), X.DATA)], H)
        self.assertNotEqual(r.code, 0)
        self.assertTrue(any("exception handler 82003000 before function 82001010 is not in code" in f for f in r.fatal),
                        r.fatal)

    def test_flagged_helper_keeps_preceding_code(self):
        """Patch 0017: the register helpers set ExceptionFlag without metadata, so the words before
        them are code. A @T: li r3,1; blr (found only by the sweep); restgprlr_14 @T+8 with a
        flagged .pdata entry (21 words). A must stay a 2-instruction function."""
        A, Hlp = T, T + 8
        text = X.words(X.li(3, 1), X.BLR, *X.restgprlr_14())
        r = recompile("flagged_helper_keeps_preceding_code",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((Hlp, 21, 0, 1)), X.DATA)], A,
                      "restgprlr_14_address = 0x%X\n" % Hlp)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(A), (8, "sweep"))

    def export_case(self, name, table, table_at, extra_words=()):
        """A @T: li r3,1; blr (pdata); export table at T+8; then extra words (the export E)."""
        A = T
        text = X.words(X.li(3, 1), X.BLR) + table + X.words(*extra_words)
        return recompile(name, [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((A, 2)), X.DATA)], A,
                         export_table_address=table_at)

    def test_export_table_is_data_and_entry(self):
        """Patch 0018: the export table (44-byte header + 1 offset = 48 bytes at T+8) is data, and its
        export E (T+0x38: li r3,5; blr; found nowhere else) is a function tagged "export"."""
        A, TBL, E = T, T + 8, T + 0x38
        r = self.export_case("export_table_is_data_and_entry", X.export_table([E - X.BASE]), TBL, (X.li(3, 5), X.BLR))
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.functions, [A, E])
        self.assertEqual(r.instruction_counts(), [2, 2])
        self.assertEqual(r.listed.get(E), (8, "export"))

    def test_export_table_bad_magic(self):
        """Patch 0018: any other layout is fatal (here the second magic word is wrong)."""
        E = T + 0x38
        r = self.export_case("export_table_bad_magic", X.export_table([E - X.BASE], magic=(0x48000000, 0x00485646, 0x48000000)),
                             T + 8, (X.li(3, 5), X.BLR))
        self.assertNotEqual(r.code, 0)
        self.assertIn("export table at 82001008 has an unsupported layout", r.fatal)

    def test_export_table_truncated(self):
        """Patch 0018: a count whose offsets would run past the code section (0x100 offsets, the
        section ends 8 bytes after the header + 1 offset) is an unsupported layout."""
        E = T + 0x38
        r = self.export_case("export_table_truncated", X.export_table([E - X.BASE], count=0x100), T + 8, (X.li(3, 5), X.BLR))
        self.assertNotEqual(r.code, 0)
        self.assertIn("export table at 82001008 has an unsupported layout", r.fatal)

    def test_export_inside_function(self):
        """Patch 0018: an export pointing into a function (A+4, A's blr) is no function entry."""
        r = self.export_case("export_inside_function", X.export_table([T + 4 - X.BASE]), T + 8)
        self.assertNotEqual(r.code, 0)
        self.assertIn("export 82001004 is not a function entry", r.fatal)

    def function_body(self, r, address):
        text = r.code_text()
        name = "_xstart" if address == T else "sub_%X" % address
        m = re.search(r"^PPC_FUNC_IMPL\(__imp__%s\) \{(.*?)^\}" % name, text, re.M | re.S)
        return m.group(1) if m else ""

    def test_final_call_before_data_stops(self):
        """Patch 0019: a .pdata function whose last word is a call (bl) followed by data (the next
        function's exception metadata) stops after the call instead of falling through.
        H @T: li r3,9; blr (pdata, handler); F @T+8: li r3,1; bl H (pdata, 2); metadata @T+0x10: H,
        0x12345678; G @T+0x18: li r3,2; blr (pdata, ExceptionFlag)."""
        H, F, G = T, T + 8, T + 0x18
        text = X.words(X.li(3, 9), X.BLR, X.li(3, 1), X.bl(F + 4, H), H, 0x12345678, X.li(3, 2), X.BLR)
        r = recompile("final_call_before_data_stops",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((H, 2), (F, 2), (G, 2, 0, 1)), X.DATA)], H)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.functions, [H, F, G])
        self.assertIn("__builtin_trap();", self.function_body(r, F))

    def test_final_call_then_padding_in_extent_stops(self):
        """Patch 0019, padding case: a .pdata extent of 3 words (li r3,1; bl H; 0) ends in padding
        after a call. Before 0019 this was "falls through past its end"; now it translates and the
        function stops (the zero word itself traps, 0003). H @T: li r3,9; blr (pdata);
        F @T+8: li r3,1; bl H; 0 (pdata, 3); G @T+0x14: li r3,2; blr (pdata)."""
        H, F, G = T, T + 8, T + 0x14
        text = X.words(X.li(3, 9), X.BLR, X.li(3, 1), X.bl(F + 4, H), 0, X.li(3, 2), X.BLR)
        r = recompile("final_call_then_padding_in_extent_stops",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((H, 2), (F, 3), (G, 2)), X.DATA)], H)
        self.assertEqual(r.fatal, [])
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertIn("__builtin_trap();", self.function_body(r, F))

    def test_final_call_before_unknown_words_is_fatal(self):
        """Patch 0019 keeps the fallthrough FATAL when what follows the call is neither padding nor
        a recorded data range: here a word the config declares invalid (skipped, not data).
        F @T: li r3,1; bl G (pdata, 2); T+8: 0x12345678 (invalid_instructions, 4 bytes);
        G @T+0xC: li r3,2; blr."""
        F, G = T, T + 0xC
        text = X.words(X.li(3, 1), X.bl(F + 4, G), 0x12345678, X.li(3, 2), X.BLR)
        r = recompile("final_call_before_unknown_words_is_fatal",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((F, 2)), X.DATA)], F,
                      "invalid_instructions = [ { data = 0x12345678, size = 4 } ]\n")
        self.assertNotEqual(r.code, 0)
        self.assertIn("function 82001000 falls through past its end at 82001008", r.fatal)

    def test_analyze_stops_at_size_limit(self):
        """Patch 0020: Function::Analyze reads no word past the size it is given
        (tests/analysis/function_analyze_test.cpp under AddressSanitizer)."""
        out = os.path.join(WORK, "function_analyze")
        os.makedirs(out, exist_ok=True)
        exe = os.path.join(out, "function_analyze_test")
        b = os.path.join(XR, "build")
        cmd = ["/opt/homebrew/opt/llvm/bin/clang++", "-std=c++20", "-O1", "-g", "-include", "cstdlib",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I" + os.path.join(XR, "XenonAnalyse"), "-I" + os.path.join(XR, "XenonUtils"),
               "-I" + os.path.join(XR, "thirdparty"), "-I" + os.path.join(XR, "thirdparty", "disasm"),
               "-I" + os.path.join(XR, "thirdparty", "fmt", "include"),
               os.path.join(HERE, "analysis", "function_analyze_test.cpp"),
               os.path.join(b, "XenonAnalyse", "libLibXenonAnalyse.a"), os.path.join(b, "XenonUtils", "libXenonUtils.a"),
               os.path.join(b, "thirdparty", "disasm", "libdisasm.a"), os.path.join(b, "thirdparty", "fmt", "libfmt.a"),
               "-o", exe]
        c = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        self.assertEqual(c.returncode, 0, c.stderr[-3000:])
        p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, (p.stdout + p.stderr)[-3000:])

    def test_sweep_stops_at_next_function(self):
        """Patch 0021: the sweep never runs past the next function. A @T: li r3,1 (no return; found
        by the sweep) is followed by B @T+4: li r3,2; blr (pdata). A is one word and falls into B."""
        A, B = T, T + 4
        text = X.words(X.li(3, 1), X.li(3, 2), X.BLR)
        r = recompile("sweep_stops_at_next_function",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((B, 2)), X.DATA)], B)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(A), (4, "sweep"))
        self.assertEqual(r.listed.get(B), (8, "pdata"))

    def test_sweep_limit_keeps_later_gap_function(self):
        """Patch 0021: an unbounded sweep from A would run through B's conditional branch, whose
        fall-through and target blocks are contiguous, and so jump over gap function E (Analyze
        keeps contiguous blocks only). A @T: li r3,1 (sweep); B @T+4: li r3,2; beq F (pdata, 2);
        E @T+0xC: li r3,5; blr (gap, sweep only); F @T+0x14: li r3,6; blr (pdata). E must be found."""
        A, B, E, F = T, T + 4, T + 0xC, T + 0x14
        text = X.words(X.li(3, 1), X.li(3, 2), X.beq(B + 4, F), X.li(3, 5), X.BLR, X.li(3, 6), X.BLR)
        r = recompile("sweep_limit_keeps_later_gap_function",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((B, 2), (F, 2)), X.DATA)], B)
        self.assertEqual(r.listed.get(E), (8, "sweep"))
        self.assertEqual(r.listed.get(A), (4, "sweep"))

    def test_analysed_function_cut_at_next(self):
        """Patch 0021: an analysed function (here a bl target) is cut at the next function's base.
        A @T: bl C; blr (pdata); C @T+8: li r3,3 (no return); D @T+0xC: li r3,4; blr (pdata).
        Analysis of C alone would run through D's blr (12 bytes); it is cut to 4."""
        A, C, D = T, T + 8, T + 0xC
        text = X.words(X.bl(A, C), X.BLR, X.li(3, 3), X.li(3, 4), X.BLR)
        r = recompile("analysed_function_cut_at_next",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((A, 2), (D, 2)), X.DATA)], A)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(C), (4, "bl_target"))

    def test_loop_pieces_merged(self):
        """Patch 0022: two analysed pieces of one routine are merged when the second branches back
        to the first's exact start, even though the first ends in an unconditional branch.
        C @T: bl L1; blr (pdata); L1 @T+8: li r3,0; b L3 (bl target; ends in b);
        L2 @T+0x10: li r3,1; b L1 (sweep; branches back to L1's start); L3 @T+0x18: blr (sweep).
        L2 joins L1 (it branches back to L1's start); then L3 joins too (L1 branches to it): one
        routine of 0x14 bytes at L1."""
        C, L1, L2, L3 = T, T + 8, T + 0x10, T + 0x18
        text = X.words(X.bl(C, L1), X.BLR, X.li(3, 0), X.b(L1 + 4, L3), X.li(3, 1), X.b(L2 + 4, L1), X.BLR)
        r = recompile("loop_pieces_merged",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((C, 2)), X.DATA)], C)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(L1), (0x14, "bl_target"))
        self.assertNotIn(L2, r.listed)
        self.assertEqual(r.functions, [C, L1])

    def test_call_does_not_join_pieces(self):
        """Patch 0022: a call (bl) is not a branch that joins pieces. C @T: bl L1; blr (pdata);
        L1 @T+8: bl L2; blr (bl target); L2 @T+0x10: li r3,2; blr (bl target). All stay functions."""
        C, L1, L2 = T, T + 8, T + 0x10
        text = X.words(X.bl(C, L1), X.BLR, X.bl(L1, L2), X.BLR, X.li(3, 2), X.BLR)
        r = recompile("call_does_not_join_pieces",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((C, 2)), X.DATA)], C)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.functions, [C, L1, L2])

    def test_export_piece_keeps_its_entry(self):
        """Patch 0022 must not drop an export's function-table entry: A @T: li r3,1 (sweep, no
        return) falls into export E @T+4: li r3,5; blr. Whether or not they merge, E stays an entry
        in the function mapping (the runtime resolves exports there). Export table at T+0xC."""
        A, E, TBL = T, T + 4, T + 0xC
        text = X.words(X.li(3, 1), X.li(3, 5), X.BLR) + X.export_table([E - X.BASE])
        r = recompile("export_piece_keeps_its_entry",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata(), X.DATA)], A,
                      export_table_address=TBL)
        self.assertIn(E, r.functions, r.log[-1500:])


def main(argv):
    names = argv[1:]
    loader = unittest.TestLoader()
    if names:
        suite = unittest.TestSuite(loader.loadTestsFromName(n, Analysis) for n in names)
    else:
        suite = loader.loadTestsFromTestCase(Analysis)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
