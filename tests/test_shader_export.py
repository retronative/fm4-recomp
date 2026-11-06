# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# survey/shader_export.py against synthetic .fxobj bytes (no game bytes).
#
#   /opt/homebrew/bin/python3 -B -I tests/test_shader_export.py
#
# The synthetic files come from tests/test_fxobj.py's builders (layout and hand-computed offsets
# documented there): fxfile(vs_part(), ps_part()) holds the VS container at 0x24 and the PS at
# 0xC4 with the 32-byte literal block LITERALS; the VS microcode is VS_UCODE (36 bytes, 9 dwords),
# the PS microcode PS_UCODE (48 bytes, 12 dwords). Expected bytes are those builders' inputs.
# FM4_SURVEY_DIR (os.environ, works under -I) points the test at a mutated copy.
import importlib.util
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SURVEY = os.environ.get("FM4_SURVEY_DIR", os.path.join(HERE, "..", "survey"))


def load(name, directory):
    spec = importlib.util.spec_from_file_location(name, os.path.join(directory, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fx = load("test_fxobj", HERE)
shader_export = load("shader_export", SURVEY)
words, fxfile, vs_part, ps_part = fx.words, fx.fxfile, fx.vs_part, fx.ps_part


def write(root, rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def tree(root):
    out = {}
    for d, _, files in os.walk(root):
        for name in files:
            with open(os.path.join(d, name), "rb") as f:
                out[os.path.relpath(os.path.join(d, name), root)] = f.read()
    return out


class Export(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.src = os.path.join(self.tmp.name, "src")
        # a.fxobj: the VS alone, with a literal block (same microcode, so the same hash, as the
        # VS in b/two.fxobj, which has none): the occurrences disagree on literals only.
        write(self.src, "a.fxobj", fxfile(vs_part(literals=fx.LITERALS)))
        write(self.src, "b/two.fxobj", fx.GOOD)
        write(self.src, "notes.xml", b"<x/>")

    def tearDown(self):
        self.tmp.cleanup()

    def out(self, name):
        return os.path.join(self.tmp.name, name)

    def test_files_and_index(self):
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("o")]), 0)
        files = tree(self.out("o"))
        vs_hash = "%016x" % shader_export.fxobj.shader_hash("vs", fx.VS_UCODE)
        ps_hash = "%016x" % shader_export.fxobj.shader_hash("ps", fx.PS_UCODE)
        self.assertEqual(sorted(files), sorted(["index.json", "vs_%s.bin" % vs_hash, "ps_%s.bin" % ps_hash]))
        self.assertEqual(files["vs_%s.bin" % vs_hash], fx.VS_UCODE)     # as stored, 36 bytes
        self.assertEqual(files["ps_%s.bin" % ps_hash], fx.PS_UCODE)     # 48 bytes
        index = json.loads(files["index.json"])
        self.assertEqual(index["counts"], {"vs": 1, "ps": 1})
        # Sorted by (stage, hash): "ps" < "vs".
        ps, vs = index["shaders"]
        self.assertEqual((ps["stage"], ps["type"], ps["hash"], ps["dwords"]), ("ps", 1, ps_hash, 12))
        self.assertEqual((vs["stage"], vs["type"], vs["hash"], vs["dwords"]), ("vs", 0, vs_hash, 9))
        self.assertEqual([(o["path"], o["offset"]) for o in vs["occurrences"]],
                         [("a.fxobj", 0x24), ("b/two.fxobj", 0x24)])
        self.assertEqual([(o["path"], o["offset"]) for o in ps["occurrences"]], [("b/two.fxobj", 0xC4)])
        self.assertEqual(ps["occurrences"][0]["literals"], fx.LITERALS.hex())
        self.assertEqual([o["literals"] for o in vs["occurrences"]], [fx.LITERALS.hex(), ""])
        self.assertEqual((vs["literals_agree"], vs["constants_agree"]), (False, True))
        self.assertEqual((ps["literals_agree"], ps["constants_agree"]), (True, True))
        self.assertEqual(vs["occurrences"][0]["constants"],
                         [{"name": "s0", "set": "sampler", "index": 0, "count": 1}])
        self.assertEqual((vs["occurrences"][0]["target"], ps["occurrences"][0]["creator"]), ("vs_3_0", "test"))

    def test_deterministic(self):
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("1")]), 0)
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("2")]), 0)
        self.assertEqual(tree(self.out("1")), tree(self.out("2")))

    def test_parse_error_writes_nothing(self):
        write(self.src, "bad.fxobj", fxfile(vs_part(), magic=0x102))
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("o")]), 1)
        self.assertFalse(os.path.exists(self.out("o")))

    def test_non_empty_output_refused(self):
        write(self.out("o"), "keep", b"x")
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("o")]), 1)
        self.assertEqual(tree(self.out("o")), {"keep": b"x"})

    def test_empty_output_dir_accepted(self):
        os.makedirs(self.out("o"))
        self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("o")]), 0)

    def test_no_fxobj_is_an_error(self):
        empty = self.out("empty")
        os.makedirs(empty)
        self.assertEqual(shader_export.main(["shader_export.py", empty, self.out("o")]), 1)
        self.assertFalse(os.path.exists(self.out("o")))

    def test_same_hash_different_microcode_refused(self):
        # A second VS with one extra (unexecuted) slot; a constant hash forces the collision.
        other = fx.container(0x102A1101, "vs_3_0", fx.VS_UCODE + words(0, 0, 0))
        write(self.src, "c.fxobj", fxfile(other))
        saved = shader_export.fxobj.shader_hash
        shader_export.fxobj.shader_hash = lambda stage, ucode: 0
        try:
            with self.assertRaisesRegex(shader_export.ExportError, "different microcode"):
                shader_export.collect(self.src)
            self.assertEqual(shader_export.main(["shader_export.py", self.src, self.out("o")]), 1)
        finally:
            shader_export.fxobj.shader_hash = saved
        self.assertFalse(os.path.exists(self.out("o")))


if __name__ == "__main__":
    unittest.main()
