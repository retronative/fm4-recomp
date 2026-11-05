#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Tests for survey/survey.py's own checks (no tools, no game files).

    /opt/homebrew/bin/python3 -B -I tests/test_survey.py [Case.test_name ...]

check_config: the [main] key assertion (a key written after a table header belongs to that table)
(ReCarbon: a key written after a [[table]] header is that table's key, and XenonRecomp
then ran without it); the fixed output paths; the *_as_local/skip_* flags (patch 0023 requires
them off while secondary entries exist). empty_ppc: deletes only XenonRecomp's own output files.
tree_hashes: byte-order paths, SHA-256 per file (expected digests of b'' and b'abc' are the
FIPS 180-2 published values).
"""
import os
import shutil
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "survey"))
import survey as S  # noqa: E402
from pathlib import Path  # noqa: E402

WORK = Path(ROOT) / "build" / "tests" / "survey"
EMPTY = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

HEADER = '[main]\nfile_path = "../game/files/default.xex"\nout_directory_path = "../ppc"\n' \
         'switch_table_file_path = "switch_tables.toml"\n'
FLAGS = ''.join(f'{k} = false\n' for k in S.MAIN_KEYS[3:11])
HELPERS = ''.join(f'{k} = 0x{0x82000000 + 4 * i:08X}\n' for i, k in enumerate(S.MAIN_KEYS[11:]))


class Config(unittest.TestCase):
    def test_complete_main_accepted(self):
        main = S.check_config(HEADER + FLAGS + HELPERS)
        self.assertEqual(main['restgprlr_14_address'], 0x82000000)

    def test_keys_after_table_are_rejected(self):
        # The last helper key written after a hand-written [[midasm_hook]] belongs to that table.
        text = HEADER + FLAGS + ''.join(HELPERS.splitlines(True)[:-1]) + '\n[[midasm_hook]]\nname = "x"\n' + HELPERS.splitlines(True)[-1]
        with self.assertRaises(SystemExit) as e:
            S.check_config(text)
        self.assertIn("'savevmx_64_address'", str(e.exception))

    def test_table_outside_main_rejected(self):
        # A complete [main] followed by a top-level table: XenonRecomp would ignore the table.
        text = HEADER + FLAGS + HELPERS + '\n[[runtime_routines]]\naddress = 0x82000000\nname = "x"\n'
        with self.assertRaises(SystemExit) as e:
            S.check_config(text)
        self.assertIn("['runtime_routines'] are outside [main]", str(e.exception))

    def test_wrong_output_directory(self):
        with self.assertRaises(SystemExit) as e:
            S.check_config(HEADER.replace('"../ppc"', '"../out"') + FLAGS + HELPERS)
        self.assertIn('out_directory_path', str(e.exception))

    def test_switch_tables_elsewhere(self):
        with self.assertRaises(SystemExit) as e:
            S.check_config(HEADER.replace('"switch_tables.toml"', '"../game/survey/switch_tables.toml"') + FLAGS + HELPERS)
        self.assertIn('switch_table_file_path', str(e.exception))

    def test_local_flag_true_rejected(self):
        with self.assertRaises(SystemExit) as e:
            S.check_config(HEADER + FLAGS.replace('non_volatile_as_local = false', 'non_volatile_as_local = true') + HELPERS)
        self.assertIn('must stay false', str(e.exception))


class Output(unittest.TestCase):
    def setUp(self):
        shutil.rmtree(WORK, ignore_errors=True)  # the test's own scratch directory under build/
        self.ppc = WORK / "ppc"
        self.ppc.mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(WORK, ignore_errors=True)

    def test_own_output_deleted(self):
        for name in ("ppc_recomp.0.cpp", "ppc_config.h", "ppc_functions.txt"):
            (self.ppc / name).write_text("x")
        S.empty_ppc(self.ppc)
        self.assertEqual(list(self.ppc.iterdir()), [])

    def test_foreign_file_refused(self):
        (self.ppc / "ppc_recomp.0.cpp").write_text("x")
        (self.ppc / "notes.md").write_text("keep")
        with self.assertRaises(SystemExit) as e:
            S.empty_ppc(self.ppc)
        self.assertIn("notes.md", str(e.exception))
        self.assertEqual(sorted(p.name for p in self.ppc.iterdir()), ["notes.md", "ppc_recomp.0.cpp"])

    def test_subdirectory_refused(self):
        (self.ppc / "sub.cpp").mkdir()
        with self.assertRaises(SystemExit):
            S.empty_ppc(self.ppc)
        self.assertTrue((self.ppc / "sub.cpp").is_dir())

    def test_hashes_in_byte_order(self):
        (self.ppc / "b.cpp").write_bytes(b"abc")
        (self.ppc / "B.h").write_bytes(b"")
        switch = WORK / "switch_tables.toml"
        switch.write_bytes(b"")
        hashes = S.tree_hashes(self.ppc, switch)
        self.assertEqual(list(hashes.items()), [("config/switch_tables.toml", EMPTY), ("ppc/B.h", EMPTY), ("ppc/b.cpp", ABC)])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:], verbosity=2)
