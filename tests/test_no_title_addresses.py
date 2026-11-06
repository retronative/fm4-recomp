# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# M2: no title address in the runtime or the build: every guest address the runtime uses comes from
# the generated tables (scripts/gen_runtime_tables.py, build tree) or ppc/; config/ holds FM4's
# addresses with their evidence. Scanned: runtime/, recomp/, tests/runtime/, the CMake files and the
# build and launch scripts. Control: the same patterns must find a planted address and a sub_ name.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_no_title_addresses.py
import re, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATTERNS = [re.compile(r'0x8[0-9A-Fa-f]{7}'), re.compile(r'sub_8[0-9A-Fa-f]{7}')]
SCANNED = ['runtime', 'recomp', 'tests/runtime', 'CMakeLists.txt', 'scripts/build.sh', 'scripts/measure_tu.sh',
           'scripts/gen_runtime_tables.py', 'scripts/run_attempt.sh']


def hits(text):
    return [m for p in PATTERNS for m in p.findall(text)]


def files():
    for rel in SCANNED:
        path = ROOT / rel
        yield from (sorted(f for f in path.rglob('*') if f.is_file()) if path.is_dir() else [path])


class TitleAddresses(unittest.TestCase):
    def test_none_in_runtime_and_build(self):
        bad = [f'{f.relative_to(ROOT)}: {h}' for f in files() for h in hits(f.read_text(errors='replace'))]
        self.assertEqual(bad, [])

    def test_control_patterns_match(self):
        self.assertEqual(hits('lwz at 0x82ADA45C in sub_82ADA340'), ['0x82ADA45C', 'sub_82ADA340'])

    def test_every_scanned_path_exists(self):
        self.assertEqual([r for r in SCANNED if not (ROOT / r).exists()], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
