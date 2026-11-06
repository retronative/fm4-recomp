# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# survey/fxobj.py and survey/shader_census.py against synthetic .fxobj bytes (no game bytes).
#
#   /opt/homebrew/bin/python3 -B -I tests/test_fxobj.py
#
# The bytes are assembled here from the layout documented in survey/fxobj.py and
# the shader census plan (outer 28-byte header; size word + 0x102A11xx container:
# 9-word header, D3DXSHADER_CONSTANTTABLE per d3dx9shader.h, optional 0x28-byte block, info
# block, literal block, microcode). Microcode is hand-assembled from ReCarbon
# runtime/gpu/xenos/ucode.h (control-flow bitfields, UnpackControlFlowInstructions) and every
# expected offset, size and count below is hand-computed (derivations in comments).
# Hash vectors: FNV-1a-64 published values (Fowler/Noll/Vo test suite):
#   "" -> 0xcbf29ce484222325, "a" -> 0xaf63dc4c8601ec8c, "foobar" -> 0x85944171f73967e8.
# The runtime hash steps one dword at a time; for dwords < 256 that is exactly the byte-wise
# FNV-1a of those bytes (XOR with a zero-extended byte), so the published vectors apply.
# FM4_SURVEY_DIR (os.environ, works under -I) points the test at a mutated copy.
import importlib.util
import json
import os
import struct
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SURVEY = os.environ.get("FM4_SURVEY_DIR", os.path.join(HERE, "..", "survey"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(SURVEY, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fxobj = load("fxobj")
shader_census = load("shader_census")


def words(*ws):
    return struct.pack(">%dI" % len(ws), *ws)


def ctab(target, version=None):
    """D3DXSHADER_CONSTANTTABLE, 0x44 bytes: header 0x1C, one CONSTANTINFO at 0x1C (sampler s0,
    register 0, count 1), "s0" at 0x30, target at 0x34, creator "test" at 0x3C."""
    version = version or {"vs": 0xFFFE0300, "ps": 0xFFFF0300}[target[:2]]
    t = words(0x1C, 0x3C, version, 1, 0x1C, 0, 0x34)
    t += struct.pack(">IHHHHII", 0x30, 3, 0, 1, 0, 0, 0)
    t += b"s0\0\0" + target.encode().ljust(8, b"\0") + b"test\0\0\0\0"
    assert len(t) == 0x44
    return t


# Control flow (RC ucode.h): exec word 0 = address | count << 12 | sequence << 16; word 1 bits
# 12-15 opcode; alloc word 0 bits 0-2 size, word 1 bits 9-10 type.
# Triple 0: A = alloc (type t, size 0): d0 0, d1 0xC000 | t << 9;
#           B = exec_end address 1, count 2, sequence 0b01 (slot 1 fetch, slot 2 ALU):
#               d0 = 0x00012001, d1 = 0x2000.
# Packed: w0 = A.d0 = 0; w1 = A.d1 | (B.d0 & 0xFFFF) << 16; w2 = B.d0 >> 16 | B.d1 << 16.
VS_CF = words(0x00000000, 0x2001C200, 0x20000001)   # alloc position + exec_end
PS_CF = words(0x00000000, 0x2001C400, 0x20000001)   # alloc colours + exec_end
VFETCH = words(0x05F81000, 0x00000688, 0x00000000)  # word 0 bits 0-4 = 0: vertex fetch
TFETCH = words(0x00011001, 0x00000000, 0x00000000)  # word 0 bits 0-4 = 1: texture fetch
ALU = words(0xC80F803E, 0x00000000, 0xE2010100)
VS_UCODE = VS_CF + VFETCH + ALU                     # 3 slots, 36 bytes
PS_UCODE = PS_CF + TFETCH + ALU + words(0, 0, 0)    # 4 slots, 48 bytes (last slot unexecuted)
LITERALS = words(0x3F800000, 0, 0, 0, 0x3F000000, 0, 0, 0)   # 2 vec4, 32 bytes


def container(magic, target, ucode, literals=b"", extra=False, info_tail=(0x1442,),
              blob_size_delta=0, size_word_delta=0, version=None):
    """Size word + container. Offsets: CTAB size word 0x24, CTAB 0x28..0x6C; extra block
    0x6C..0x94 if present; info (2 + len(info_tail) words); blob after it."""
    after_ctab = 0x28 + 0x44                            # 0x6C
    extra_off = after_ctab if extra else 0
    info_off = after_ctab + (0x28 if extra else 0)      # 0x6C or 0x94
    info = words(len(literals), len(ucode), *info_tail)
    blob_off = info_off + len(info)                     # 0x78 or 0xA0 with one tail word
    blob_size = len(literals) + len(ucode) + blob_size_delta
    head = words(magic, blob_off, blob_size, 0, 0x24, extra_off, info_off, 0, 0, 0x44)
    body = head + ctab(target, version) +(words(0, 1, 0, 0, 0x14, 0x00FC0010, 0, 0, 0, 0) if extra else b"")
    body += info + literals + ucode
    assert len(body) == blob_off + len(literals) + len(ucode)
    return words(blob_off + blob_size + size_word_delta) + body


def fxfile(*parts, tail=words(0), magic=0x101, size_delta=0, truncate=None):
    """28-byte outer header, a zero pad word (so the first size word sits at 0x1C + 4 = 0x20),
    the parts, a tail."""
    body = words(0) + b"".join(parts) + tail
    data = bytearray(words(magic, 0x12345678, 0, 1, 1, 1, 0) + body)
    if truncate is not None:
        del data[truncate:]
    struct.pack_into(">I", data, 0x18, len(data) - 28 + size_delta)
    return bytes(data)


def vs_part(**kw):
    return container(0x102A1101, "vs_3_0", VS_UCODE, **kw)


def ps_part(**kw):
    return container(0x102A1100, "ps_3_0", PS_UCODE, literals=LITERALS, extra=True, **kw)


# Hand layout of fxfile(vs_part(), ps_part()):
#   VS size word 0x20, VS magic 0x24, blob offset 0x78, blob 36 -> size 0x9C, ends 0x24+0x9C = 0xC0,
#     microcode at 0x24 + 0x78 = 0x9C.
#   PS size word 0xC0, PS magic 0xC4, blob offset 0xA0, blob 32 + 48 = 0x50 -> size 0xF0,
#     ends 0xC4 + 0xF0 = 0x1B4; literals at 0xC4 + 0xA0 = 0x164, microcode at 0x164 + 0x20 = 0x184.
#   Tail word 0x1B4..0x1B8; file size 0x1B8, size field 0x19C.
GOOD = fxfile(vs_part(), ps_part())


class Hash(unittest.TestCase):
    def test_published_vectors_bytewise(self):
        self.assertEqual(fxobj.fnv1a64_words([], fxobj.FNV_OFFSET), 0xCBF29CE484222325)
        self.assertEqual(fxobj.fnv1a64_words(b"a", fxobj.FNV_OFFSET), 0xAF63DC4C8601EC8C)
        self.assertEqual(fxobj.fnv1a64_words(b"foobar", fxobj.FNV_OFFSET), 0x85944171F73967E8)

    def test_stage_seed(self):
        self.assertEqual(fxobj.shader_hash("vs", b""), 0xCBF29CE484222325)
        self.assertEqual(fxobj.shader_hash("ps", b""), 0xCBF29CE484222324)   # basis ^ 1
        # ps seed ^ 0x60 = basis ^ 0x61: the same state as "a" under the vs seed.
        self.assertEqual(fxobj.shader_hash("ps", b"\x60\0\0\0"), 0xAF63DC4C8601EC8C)

    def test_dwords_load_little_endian(self):
        self.assertEqual(fxobj.shader_hash("vs", b"a\0\0\0"), 0xAF63DC4C8601EC8C)
        self.assertEqual(fxobj.shader_hash("vs", b"".join(bytes([c, 0, 0, 0]) for c in b"foobar")),
                         0x85944171F73967E8)
        self.assertNotEqual(fxobj.shader_hash("vs", b"\0\0\0a"), 0xAF63DC4C8601EC8C)

    def test_partial_dword_rejected(self):
        with self.assertRaises(fxobj.FormatError):
            fxobj.shader_hash("vs", b"abc")


class Parse(unittest.TestCase):
    def test_good_file(self):
        info = fxobj.parse_fxobj(GOOD)
        self.assertEqual(info["errors"], [])
        self.assertEqual(info["outer"][0], "00000101")
        self.assertEqual(info["outer"][6], "0000019C")
        self.assertEqual([(k["offset"], k["end"]) for k in info["containers"]], [(0x24, 0xC0), (0xC4, 0x1B4)])

    def test_stage_from_magic_and_version(self):
        vs, ps = fxobj.parse_fxobj(GOOD)["containers"]
        self.assertEqual((vs["stage"], ps["stage"]), ("vs", "ps"))
        self.assertEqual((vs["ctab"]["target"], ps["ctab"]["target"]), ("vs_3_0", "ps_3_0"))
        self.assertEqual(vs["ctab"]["creator"], "test")
        self.assertEqual(ps["ctab"]["constants"], [{"name": "s0", "set": "sampler", "index": 0, "count": 1}])

    def test_microcode_location(self):
        vs, ps = fxobj.parse_fxobj(GOOD)["containers"]
        self.assertEqual((vs["ucode_offset"], vs["ucode_bytes"], vs["ucode_dwords"], vs["literal_bytes"]),
                         (0x9C, 36, 9, 0))
        self.assertEqual((ps["ucode_offset"], ps["ucode_bytes"], ps["ucode_dwords"], ps["literal_bytes"]),
                         (0x184, 48, 12, 32))
        self.assertEqual(GOOD[0x184:0x184 + 48], PS_UCODE)

    def test_hash_covers_microcode_only_with_stage_seed(self):
        vs, ps = fxobj.parse_fxobj(GOOD)["containers"]
        self.assertEqual(vs["hash"], "%016x" % fxobj.shader_hash("vs", VS_UCODE))
        self.assertEqual(ps["hash"], "%016x" % fxobj.shader_hash("ps", PS_UCODE))

    def test_extra_and_info_blocks(self):
        vs, ps = fxobj.parse_fxobj(GOOD)["containers"]
        self.assertEqual(vs["extra"], [])
        self.assertEqual(len(ps["extra"]), 10)
        self.assertEqual(ps["info"], ["00000020", "00000030", "00001442"])

    def test_control_flow(self):
        vs, ps = fxobj.parse_fxobj(GOOD)["containers"]
        # VS: 2 CF instructions in 1 triple; slot 1 vfetch, slot 2 ALU; 3 slots, none unexecuted.
        self.assertEqual({k: vs["cf"][k] for k in ("cf_instructions", "slots", "alu", "vfetch", "tfetch",
                                                   "unexecuted_slots", "allocs", "loops", "conditional")},
                         {"cf_instructions": 2, "slots": 3, "alu": 1, "vfetch": 1, "tfetch": 0,
                          "unexecuted_slots": 0, "allocs": [[1, 0]], "loops": False, "conditional": False})
        # PS: slot 1 tfetch, slot 2 ALU, slot 3 padding.
        self.assertEqual((ps["cf"]["tfetch"], ps["cf"]["vfetch"], ps["cf"]["alu"], ps["cf"]["unexecuted_slots"],
                          ps["cf"]["allocs"]), (1, 0, 1, 1, [[2, 0]]))


class Rejects(unittest.TestCase):
    def errors(self, data):
        return fxobj.parse_fxobj(data)["errors"]

    def assertError(self, data, text):
        errs = self.errors(data)
        self.assertTrue(any(text in e for e in errs), "no error containing %r in %r" % (text, errs))

    def test_bad_magic(self):
        self.assertError(fxfile(vs_part(), magic=0x102), "file magic")

    def test_size_mismatch(self):
        self.assertError(fxfile(vs_part(), size_delta=4), "size field")

    def test_short_file(self):
        self.assertError(GOOD[:27], "shorter than the 28-byte header")

    def test_truncated_container(self):
        # Cut inside the PS container (0xC4..0x1B4); the size field is fixed up to the new length.
        errs = self.errors(fxfile(vs_part(), ps_part(), truncate=0x100))
        self.assertTrue(any(e.startswith("container at 0xC4:") for e in errs), errs)
        self.assertFalse(any(e.startswith("container at 0x24:") for e in errs), errs)

    def test_unknown_container(self):
        part = container(0x102A1102, "vs_3_0", VS_UCODE)
        self.assertError(fxfile(part), "unknown container magic 0x102A1102")

    def test_unaligned_magic(self):
        self.assertError(fxfile(vs_part(), tail=b"\0\x10\x2a\x11"), "unaligned offset 0xC1")

    def test_no_container(self):
        self.assertError(fxfile(), "no shader container")

    def test_stage_disagreement(self):
        part = container(0x102A1101, "ps_3_0", PS_UCODE)
        self.assertError(fxfile(part), "version token says ps")

    def test_blob_size_mismatch(self):
        # Blob 12 bytes longer than literals + microcode; a 16-byte tail keeps it inside the file.
        self.assertError(fxfile(vs_part(blob_size_delta=12), tail=words(0, 0, 0, 0)), "!= blob size")

    def test_size_word_mismatch(self):
        self.assertError(fxfile(vs_part(size_word_delta=4)), "size word before the magic")

    def test_microcode_not_whole_instructions(self):
        part = container(0x102A1101, "vs_3_0", VS_UCODE + words(0))
        self.assertError(fxfile(part), "not whole 3-dword instructions")

    def test_literals_not_whole_vec4(self):
        part = container(0x102A1101, "vs_3_0", VS_UCODE, literals=words(0, 0))
        self.assertError(fxfile(part), "not whole vec4s")

    def test_no_ending_exec(self):
        # B opcode 2 -> 1 (exec): w2 = 0x0001 | 0x1000 << 16 = 0x10000001.
        ucode = words(0, 0x2001C200, 0x10000001) + VFETCH + ALU
        self.assertError(fxfile(container(0x102A1101, "vs_3_0", ucode)), "no ending exec")

    def test_exec_past_microcode(self):
        # B count 2 -> 3 (address 1 + 3 > 3 slots): d0 = 0x00013001 -> w1 = 0x3001C200.
        ucode = words(0, 0x3001C200, 0x20000001) + VFETCH + ALU
        self.assertError(fxfile(container(0x102A1101, "vs_3_0", ucode)), "runs past")

    def test_position_alloc_in_pixel_shader(self):
        part = container(0x102A1100, "ps_3_0", VS_UCODE)
        self.assertError(fxfile(part), "position alloc in a ps")

    def test_bad_ctab_target(self):
        part = container(0x102A1101, "xs_3_0", VS_UCODE, version=0xFFFE0300)  # token vs, target not
        self.assertError(fxfile(part), "disagrees with version token")


class Census(unittest.TestCase):
    def test_inventory(self):
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as o:
            os.makedirs(os.path.join(d, "b"))
            with open(os.path.join(d, "b", "two.fxobj"), "wb") as f:
                f.write(GOOD)
            with open(os.path.join(d, "a.fxobj"), "wb") as f:
                f.write(fxfile(vs_part()))
            with open(os.path.join(d, "notes.xml"), "wb") as f:
                f.write(b"<x/>")
            result = shader_census.census(d)
            out1, out2 = os.path.join(o, "1.json"), os.path.join(o, "2.json")
            self.assertEqual(shader_census.main(["shader_census.py", d, out1]), 0)
            self.assertEqual(shader_census.main(["shader_census.py", d, out2]), 0)
            with open(out1, "rb") as f1, open(out2, "rb") as f2:
                self.assertEqual(f1.read(), f2.read())
        self.assertEqual([f["path"] for f in result["files"]], ["a.fxobj", "b/two.fxobj"])
        self.assertEqual(result["other_files"], ["notes.xml"])
        t = result["totals"]
        self.assertEqual((t["files"], t["files_with_errors"], t["errors"]), (2, 0, 0))
        self.assertEqual(t["containers"], {"vs": 2, "ps": 1})
        self.assertEqual(t["ucode_bytes"], {"vs": 72, "ps": 48})
        self.assertEqual(t["distinct"], {"vs": 1, "ps": 1})
        vs = result["distinct"]["vs"][0]
        self.assertEqual((vs["occurrences"], vs["first"], vs["ucode_dwords"]),
                         (2, {"path": "a.fxobj", "offset": 0x24}, 9))
        self.assertEqual(result["distinct"]["ps"][0]["samplers"], ["s0"])

    def test_errors_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "bad.fxobj"), "wb") as f:
                f.write(fxfile(vs_part(), magic=0x102))
            out = os.path.join(d, "out.json")
            self.assertEqual(shader_census.main(["shader_census.py", d, out]), 1)
            with open(out) as f:
                result = json.load(f)
        self.assertEqual(result["totals"]["files_with_errors"], 1)
        self.assertTrue(result["errors"][0].startswith("bad.fxobj: file magic"))

    def test_empty_directory_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(shader_census.main(["shader_census.py", d, os.path.join(d, "o.json")]), 1)


if __name__ == "__main__":
    unittest.main()
