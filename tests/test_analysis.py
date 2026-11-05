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


def recompile(case, sections, entry, extra_config="", export_table_address=0, files=None, image_size=0x4000):
    d = os.path.join(WORK, case)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "ppc"))
    with open(os.path.join(d, "case.xex"), "wb") as f:
        f.write(X.build(sections, entry, image_size=image_size, export_table_address=export_table_address))
    for name, text in (files or {}).items():
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
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

    def build_unit(self, name, libs):
        out = os.path.join(WORK, name)
        os.makedirs(out, exist_ok=True)
        exe = os.path.join(out, name)
        b = os.path.join(XR, "build")
        cmd = ["/opt/homebrew/opt/llvm/bin/clang++", "-std=c++20", "-O1", "-g", "-include", "cstdlib",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I" + os.path.join(XR, "XenonAnalyse"), "-I" + os.path.join(XR, "XenonUtils"),
               "-I" + os.path.join(XR, "thirdparty"), "-I" + os.path.join(XR, "thirdparty", "disasm"),
               "-I" + os.path.join(XR, "thirdparty", "fmt", "include"),
               os.path.join(HERE, "analysis", name + ".cpp")] + [os.path.join(b, l) for l in libs] + ["-o", exe]
        c = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        self.assertEqual(c.returncode, 0, c.stderr[-3000:])
        p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, (p.stdout + p.stderr)[-3000:])

    def test_image_find_bounds(self):
        """Patch 0023a: Image::Find is null outside every section (tests/analysis/image_find_test.cpp
        under AddressSanitizer)."""
        self.build_unit("image_find_test", [os.path.join("XenonUtils", "libXenonUtils.a"),
                                            os.path.join("thirdparty", "fmt", "libfmt.a")])

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

    def test_exception_handler_must_be_function(self):
        """Patch 0023: a handler word that points into code but not at a function is fatal.
        H @T: li r3,9; blr (pdata); metadata @T+8: H+4 (inside H), 0x12345678;
        E @T+0x10: li r3,1; blr (pdata, ExceptionFlag)."""
        H, E = T, T + 0x10
        text = X.words(X.li(3, 9), X.BLR, H + 4, 0x12345678, X.li(3, 1), X.BLR)
        r = recompile("exception_handler_must_be_function",
                      [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((H, 2), (E, 2, 0, 1)), X.DATA)], H)
        self.assertNotEqual(r.code, 0)
        self.assertIn("exception handler 82001004 before function 82001010 is not a function", r.fatal)

    def test_exception_handler_word_outside_image(self):
        """Patch 0023: a flagged function at the start of .text has no handler word in any section
        (T-8 is header space): fatal, not a null dereference. E @T: li r3,1; blr (pdata, flag)."""
        E = T
        r = recompile("exception_handler_word_outside_image",
                      [(".text", 0x1000, X.words(X.li(3, 1), X.BLR), X.CODE), (".pdata", P, X.pdata((E, 2, 0, 1)), X.DATA)], E)
        self.assertNotEqual(r.code, 0)
        self.assertIn("no exception handler word before function 82001000", r.fatal)

    def entry_with_option(self, name, option):
        """C @T: bl O+4; blr (pdata); O @T+8: li r3,1; li r3,2; blr (pdata): O+4 is a secondary
        entry. With a *_as_local option set it must be fatal (patch 0023)."""
        C, O = T, T + 8
        text = X.words(X.bl(C, O + 4), X.BLR, X.li(3, 1), X.li(3, 2), X.BLR)
        return recompile(name, [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((C, 2), (O, 3)), X.DATA)], C,
                         "%s = true\n" % option)

    def test_secondary_entry_rejects_ctr_as_local(self):
        r = self.entry_with_option("secondary_entry_rejects_ctr_as_local", "ctr_as_local")
        self.assertNotEqual(r.code, 0)
        self.assertIn("secondary entries need registers in the context, but a *_as_local option is set", r.fatal)

    def test_secondary_entry_rejects_non_volatile_as_local(self):
        r = self.entry_with_option("secondary_entry_rejects_non_volatile_as_local", "non_volatile_as_local")
        self.assertNotEqual(r.code, 0)
        self.assertIn("secondary entries need registers in the context, but a *_as_local option is set", r.fatal)

    def runtime_case(self, name, text, pdata, config):
        return recompile(name, [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata(*pdata), X.DATA)], T, config)

    def test_runtime_routine_body(self):
        """Patch 0024: a runtime routine's body is a call to PpcRuntime_<name>, followed by a stop
        when it does not return. C @T: bl R; blr (pdata); R @T+8: li r3,7; blr (routine)."""
        C, R = T, T + 8
        r = self.runtime_case("runtime_routine_body", X.words(X.bl(C, R), X.BLR, X.li(3, 7), X.BLR), [(C, 2)],
                              'runtime_routines = [ { address = 0x%X, name = "restore_context", noreturn = true } ]\n' % R)
        body = self.function_body(r, R)
        self.assertIn("PpcRuntime_restore_context(ctx, base);", body)
        self.assertIn("__builtin_trap();", body)
        self.assertNotIn("ctx.r3.s64 = 7;", body)  # the guest body is not translated

    def test_runtime_routine_entry_is_fatal(self):
        """Patch 0024: entering a runtime routine in its middle is fatal. C @T: bl R; blr (pdata);
        R @T+8: li r3,7; blr (routine); D @T+0x10: b R+4 (pdata)."""
        C, R, D = T, T + 8, T + 0x10
        r = self.runtime_case("runtime_routine_entry_is_fatal",
                              X.words(X.bl(C, R), X.BLR, X.li(3, 7), X.BLR, X.b(D, R + 4)), [(C, 2), (D, 1)],
                              'runtime_routines = [ { address = 0x%X, name = "switch_fiber" } ]\n' % R)
        self.assertNotEqual(r.code, 0)
        self.assertIn("secondary entry 8200100C enters runtime-handled routine 82001008", r.fatal)

    def test_setjmp_call(self):
        """Patch 0024: a call to setjmp snapshots ctx, calls host setjmp on the guest buffer in r3
        (PpcJumpBuffer), and restores ctx only on the second (non-zero) return.
        C @T: bl S; blr (pdata); S @T+8: li r3,0; blr (setjmp)."""
        C, S = T, T + 8
        r = self.runtime_case("setjmp_call", X.words(X.bl(C, S), X.BLR, X.li(3, 0), X.BLR), [(C, 2)],
                              "setjmp_address = 0x%X\n" % S)
        body = self.function_body(r, C)
        self.assertRegex(body, r"\t(\w+) = ctx;\n\t(\w+)\.s64 = setjmp\(PpcJumpBuffer\(ctx\.r3\.u32\)\);\n\tif \(\2\.s64 != 0\) ctx = \1;")

    def test_branch_to_setjmp_is_fatal(self):
        """Patch 0024: setjmp must return into a caller, so a branch (not a call) to it is fatal.
        C @T: b S (pdata, 1); S @T+4: li r3,0; blr (setjmp)."""
        C, S = T, T + 4
        r = self.runtime_case("branch_to_setjmp_is_fatal", X.words(X.b(C, S), X.li(3, 0), X.BLR), [(C, 1)],
                              "setjmp_address = 0x%X\n" % S)
        self.assertNotEqual(r.code, 0)
        self.assertIn("branch (not a call) to setjmp at 82001000: setjmp must return into a caller", r.fatal)

    def test_longjmp_call(self):
        """Patch 0024: a call to longjmp is host longjmp on the guest buffer in r3 with the value in r4.
        C @T: bl L; blr (pdata); L @T+8: li r3,0; blr (longjmp)."""
        C, L = T, T + 8
        r = self.runtime_case("longjmp_call", X.words(X.bl(C, L), X.BLR, X.li(3, 0), X.BLR), [(C, 2)],
                              "longjmp_address = 0x%X\n" % L)
        self.assertIn("longjmp(PpcJumpBuffer(ctx.r3.u32), ctx.r4.s32);", self.function_body(r, C))

    def switch_case(self, name, labels, extra_words=(), pdata_extra=(), table=None, table_size=8, rdata=None):
        """F @T (pdata, 8 words): mtctr r4; bctr; table (T+0x10, T+0x18); case 0 @T+0x10: li r3,1;
        blr; case 1 @T+0x18: li r3,2; blr; then extra words. Switch at the bctr (T+4) on r3."""
        F = T
        text = X.words(X.MTCTR_R4, X.BCTR, T + 0x10, T + 0x18, X.li(3, 1), X.BLR, X.li(3, 2), X.BLR, *extra_words)
        table = "[[switch]]\nbase = 0x%X\nr = 3\nlabels = [ %s ]\ntable = 0x%X\ntable_size = %d\n" % (
            T + 4, ", ".join("0x%X" % l for l in labels), T + 8 if table is None else table, table_size)
        sections = [(".text", 0x1000, text, X.CODE), (".pdata", P, X.pdata((F, 8), *pdata_extra), X.DATA)]
        if rdata is not None:
            sections.append((".rdata", 0x3000, rdata, X.DATA))
        return recompile(name, sections, F,
                         'switch_table_file_path = "switch.toml"\n', files={"switch.toml": table})

    def test_switch_proven(self):
        """Patch 0025: a proven switch dispatches on the index register's low word, jumps to each
        case, traps on any other index, and its inline table is data, not instructions."""
        r = self.switch_case("switch_proven", [T + 0x10, T + 0x18])
        self.assertEqual(r.code, 0, r.log[-2000:])
        body = self.function_body(r, T)
        self.assertIn("switch (ctx.r3.u32) {", body)
        self.assertIn("\tcase 0:\n\t\tgoto loc_82001010;\n\tcase 1:\n\t\tgoto loc_82001018;\n\tdefault:\n\t\t__builtin_trap();", body)
        self.assertIn("// data: 8 bytes", body)

    def test_switch_without_bound_is_fatal(self):
        """Patch 0025: a switch entry with no labels (no proven bound) is fatal."""
        r = self.switch_case("switch_without_bound_is_fatal", [])
        self.assertNotEqual(r.code, 0)
        self.assertIn("switch table at 82001004 has no proven bound", r.fatal)

    def test_switch_case_in_another_function_is_entry(self):
        """Patch 0025: a case label inside another function becomes a secondary entry of it, and
        the switch tail-calls it. G @T+0x20 (pdata, 2): li r3,3; blr; case 1 = G+4 (G's blr)."""
        G = T + 0x20
        r = self.switch_case("switch_case_in_another_function_is_entry", [T + 0x10, G + 4],
                             extra_words=(X.li(3, 3), X.BLR), pdata_extra=((G, 2),))
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertIn(G + 4, r.functions)
        self.assertIn("sub_82001024(ctx, base);", self.function_body(r, T))

    T8 = 0x82008000  # .text at RVA 0x8000: addresses whose low half is >= 0x8000

    def pointer_case(self, name, load_address, rdata=None):
        """C @T8 (pdata, 7): lis r4,0x8201; li r5,0; li r6,0; li r7,0; addi r4,r4,-0x7FDC (= H when
        load_address, else li r4,0); bl X; blr. X @T8+0x1C: li r3,1; b H+4 (bl target).
        H @T8+0x24: li r3,5; blr. X branches into H's piece, so 0022 merges H away; H follows X's
        unconditional b, so a pointer to H makes it an entry again (patch 0026)."""
        C, Xf, H = self.T8, self.T8 + 0x1C, self.T8 + 0x24
        load = (X.lis(4, 0x8201), X.li(5, 0), X.li(6, 0), X.li(7, 0), X.addi(4, 4, H - 0x82010000)) if load_address \
            else (X.li(4, 0), X.li(5, 0), X.li(6, 0), X.li(7, 0), X.li(4, 0))
        text = X.words(*load, X.bl(C + 0x14, Xf), X.BLR, X.li(3, 1), X.b(Xf + 4, H + 4), X.li(3, 5), X.BLR)
        sections = [(".text", 0x8000, text, X.CODE), (".pdata", P, X.pdata((C, 7)), X.DATA)]
        if rdata is not None:
            sections.append((".rdata", 0x9000, rdata, X.DATA))
        return recompile(name, sections, C, image_size=0x10000), H

    def test_pointer_reached_piece_is_entry(self):
        """Patch 0026: lis/addi (3 instructions apart, low half >= 0x8000) reaching a merged piece's
        start after an unconditional terminator make it an entry."""
        r, H = self.pointer_case("pointer_reached_piece_is_entry", True)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(H), (0, "secondary@8200801C"))  # merged into X, back as X's entry
        self.assertIn(H, r.functions)

    def test_data_pointer_reached_piece_is_entry(self):
        """Patch 0026: a code address stored in a data section (a vtable slot) makes an entry."""
        r, H = self.pointer_case("data_pointer_reached_piece_is_entry", False, rdata=X.words(self.T8 + 0x24))
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.listed.get(H), (0, "secondary@8200801C"))
        self.assertIn(H, r.functions)

    def test_unreached_merged_piece_is_not_entry(self):
        """Control: without any pointer the merged piece is not an entry."""
        r, H = self.pointer_case("unreached_merged_piece_is_not_entry", False)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertNotIn(H, r.functions)
        self.assertNotIn(H, r.listed)

    def test_fatal_reported_once_for_reemitted_body(self):
        """Patch 0026: a FATAL inside a body that a secondary entry re-emits is reported once.
        O @T (pdata, 2): frsqrte f1,f2; blr; C @T+8 (pdata, 2): bl O+4; blr (O+4 is an entry)."""
        O, C = T, T + 8
        r = recompile("fatal_reported_once_for_reemitted_body",
                      [(".text", 0x1000, X.words(X.frsqrte(1, 2), X.BLR, X.bl(C, O + 4), X.BLR), X.CODE),
                       (".pdata", P, X.pdata((O, 2), (C, 2)), X.DATA)], O)
        self.assertNotEqual(r.code, 0)
        self.assertEqual(r.fatal, ["unrecognized instruction at 0x82001000: frsqrte"])

    def test_switch_table_unaligned_is_fatal(self):
        """Patch 0027: an in-code switch table that is not word-aligned is fatal."""
        r = self.switch_case("switch_table_unaligned_is_fatal", [T + 0x10, T + 0x18], table=T + 0xA)
        self.assertNotEqual(r.code, 0)
        self.assertIn("switch table 8200100A of the bctr at 82001004 is in code but not word-aligned", r.fatal)

    def test_switch_table_overrun_is_fatal(self):
        """Patch 0027: a data range running past its function's end is fatal: the table at T+0x1C
        (8 bytes) ends 4 bytes past the 0x20-byte function; two nops after it keep the table inside
        .text (a table not wholly in code is no data range)."""
        r = self.switch_case("switch_table_overrun_is_fatal", [T + 0x10, T + 0x18], table=T + 0x1C,
                             extra_words=(X.NOP, X.NOP))
        self.assertNotEqual(r.code, 0)
        self.assertIn("data at 8200101C runs past the end of function 82001000", r.fatal)

    def test_second_setjmp_call_is_fatal(self):
        """Patch 0027: one setjmp snapshot per function: a second call in it is fatal.
        C @T: bl S; bl S; blr (pdata, 3); S @T+0xC: li r3,0; blr (setjmp)."""
        C, S = T, T + 0xC
        r = self.runtime_case("second_setjmp_call_is_fatal", X.words(X.bl(C, S), X.bl(C + 4, S), X.BLR, X.li(3, 0), X.BLR),
                              [(C, 3)], "setjmp_address = 0x%X\n" % S)
        self.assertNotEqual(r.code, 0)
        self.assertIn("second setjmp call in function 82001000 at 82001004", r.fatal)

    def test_setjmp_once_in_each_of_two_functions(self):
        """Patch 0027: the per-function count restarts in each function.
        C1 @T: bl S; blr (pdata); C2 @T+8: bl S; blr (pdata); S @T+0x10: li r3,0; blr (setjmp)."""
        C1, C2, S = T, T + 8, T + 0x10
        r = self.runtime_case("setjmp_once_in_each_of_two_functions",
                              X.words(X.bl(C1, S), X.BLR, X.bl(C2, S), X.BLR, X.li(3, 0), X.BLR),
                              [(C1, 2), (C2, 2)], "setjmp_address = 0x%X\n" % S)
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertEqual(r.fatal, [])

    def test_switch_table_pointer_is_not_entry(self):
        """Patch 0028: a pointer to a switch's table (a data word holding T+8, the table right after
        the bctr, an unconditional terminator) does not make the table an entry. table_size 0: the
        table is no data range (the case 0028 is for: an unproven table's extent is unknown)."""
        r = self.switch_case("switch_table_pointer_is_not_entry", [T + 0x10, T + 0x18], rdata=X.words(T + 8),
                             table_size=0)
        self.assertNotIn(T + 8, r.listed)
        self.assertNotIn(T + 8, r.functions)

    def test_switch_case_pointer_is_not_entry(self):
        """Patch 0028: a pointer to a case label (T+0x18, right after case 0's blr) does not make it
        an extra entry: its switch reaches it."""
        r = self.switch_case("switch_case_pointer_is_not_entry", [T + 0x10, T + 0x18], rdata=X.words(T + 0x18))
        self.assertEqual(r.code, 0, r.log[-2000:])
        self.assertNotIn(T + 0x18, r.listed)


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
