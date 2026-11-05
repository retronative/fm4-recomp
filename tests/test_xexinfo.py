# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# survey/xexinfo.py against a synthetic XEX2 header (no game bytes).
#
#   /opt/homebrew/bin/python3 -I tests/test_xexinfo.py
#
# The header is assembled here from the layout in Xenia canary 67f16c4e315f
# src/xenia/kernel/util/xex2_info.h (xex2_header, xex2_security_info, xex2_page_descriptor,
# xex2_opt_execution_info, xex2_opt_tls_info, xex2_opt_static_library, xex2_opt_import_libraries,
# xex2_import_library) and xex_module.cc GetOptHeader (key low byte 0x00/0x01 = inline value).
# Expected values are written by hand from the bytes placed below, never from the parser's output.
# FM4_SURVEY_DIR (read from os.environ, so it works under -I) points the test at a mutated copy.
import importlib.util
import os
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SURVEY = os.environ.get("FM4_SURVEY_DIR", os.path.join(HERE, "..", "survey"))
spec = importlib.util.spec_from_file_location("xexinfo", os.path.join(SURVEY, "xexinfo.py"))
xexinfo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xexinfo)


def be32(v):
    return struct.pack(">I", v)


def be16(v):
    return struct.pack(">H", v)


def build(page_words, image_flags=0, magic=b"XEX2"):
    """A minimal XEX2: header, 10 optional headers, their data blocks, security info."""
    blocks = {}  # offset -> bytes, laid out from 0x200
    cursor = [0x200]

    def place(data):
        off = cursor[0]
        blocks[off] = data
        cursor[0] += (len(data) + 15) & ~15
        return off

    exec_info = (be32(0x11223344) + be32(0x20449700) + be32(0x10000001) + be32(0x41420007)
                 + bytes([2, 1, 1, 2]) + be32(0))
    file_format = be32(8) + be16(1) + be16(1)  # info size, normal encryption, basic compression
    pe_name = b"game.exe\0\0\0\0"
    pe = be32(4 + len(pe_name)) + pe_name
    tls = be32(64) + be32(0x82E9E400) + be32(0x1820) + be32(0x1000)
    static = b"XAPILIB\0" + be16(2) + be16(0) + be16(17559) + bytes([0x40, 3])
    static2 = b"D3D9\0\0\0\0" + be16(2) + be16(0) + be16(17559) + bytes([0x40, 0])
    statics = be32(4 + 32) + static + static2
    strings = b"xboxkrnl.exe\0\0\0\0" + b"xam.xex\0"  # 16 + 8 = 24 bytes, entries 4-aligned
    lib_xam = (be32(0x28 + 3 * 4) + b"\0" * 0x14 + be32(0) + be32(0x20449700) + be32(0x20440000)
               + be16(1) + be16(3) + be32(0x82000100) + be32(0x82000104) + be32(0x82000108))
    lib_krnl = (be32(0x28 + 2 * 4) + b"\0" * 0x14 + be32(0) + be32(0x20449700) + be32(0x20440000)
                + be16(0) + be16(2) + be32(0x82000200) + be32(0x82000204))
    imports_body = be32(len(strings)) + be32(2) + strings + lib_xam + lib_krnl
    imports = be32(4 + len(imports_body)) + imports_body

    opt = [
        (0x00010100, 0x82123450),  # entry point, inline
        (0x00010201, 0x82000000),  # image base, inline (low byte 01)
        (0x00020200, 0x00040000),  # stack size, inline
        (0x00040006, place(exec_info)),
        (0x000003FF, place(file_format)),
        (0x000183FF, place(pe)),
        (0x00020104, place(tls)),
        (0x000200FF, place(statics)),
        (0x000103FF, place(imports)),
        (0x00030000, 0x00000400),  # system flags, inline
    ]
    sec = bytearray(0x184 + 24 * len(page_words))
    sec[0x4:0x8] = be32(0x00070000)          # image_size
    sec[0x10C:0x110] = be32(image_flags)
    sec[0x110:0x114] = be32(0x82000000)      # load_address
    sec[0x178:0x17C] = be32(0xFFFFFFFF)      # region
    sec[0x17C:0x180] = be32(0x00000003)      # allowed media
    sec[0x180:0x184] = be32(len(page_words))
    for i, w in enumerate(page_words):
        sec[0x184 + i * 24:0x188 + i * 24] = be32(w)
    sec_off = place(bytes(sec))

    head = magic + be32(0x00000001) + be32(cursor[0]) + be32(0) + be32(sec_off) + be32(len(opt))
    for k, v in opt:
        head += be32(k) + be32(v)
    out = bytearray(cursor[0])
    out[:len(head)] = head
    for off, data in blocks.items():
        out[off:off + len(data)] = data
    return bytes(out)


# page_count << 4 | kind: 3 read-only pages, 2 + 1 code pages (merge), 1 data page.
PAGES = [(3 << 4) | 3, (2 << 4) | 1, (1 << 4) | 1, (1 << 4) | 2]


class XexInfoTest(unittest.TestCase):
    def setUp(self):
        self.info = xexinfo.parse(build(PAGES))

    def test_inline_optional_headers(self):
        self.assertEqual(self.info["entry_point"], 0x82123450)
        self.assertEqual(self.info["image_base"], 0x82000000)
        self.assertEqual(self.info["default_stack_size"], 0x40000)
        self.assertEqual(self.info["system_flags"], 0x400)
        inline = {h["key"]: h["inline"] for h in self.info["optional_headers"]}
        self.assertTrue(inline[0x00010201])
        self.assertFalse(inline[0x000103FF])

    def test_execution_info(self):
        e = self.info["execution"]
        self.assertEqual(e["title_id"], 0x41420007)
        self.assertEqual(e["media_id"], 0x11223344)
        self.assertEqual(e["version_raw"], 0x20449700)
        self.assertEqual(e["version"], "2.0.17559.0")     # 0x2 | 0x0 | 0x4497 | 0x00
        self.assertEqual(e["base_version"], "1.0.0.1")    # 0x10000001
        self.assertEqual((e["disc_number"], e["disc_count"]), (1, 2))

    def test_file_format_name_tls(self):
        self.assertEqual(self.info["file_format"], {"encryption": "normal", "compression": "basic"})
        self.assertEqual(self.info["pe_name"], "game.exe")
        self.assertEqual(self.info["tls"], {"slot_count": 64, "raw_data_address": 0x82E9E400,
                                            "data_size": 0x1820, "raw_data_size": 0x1000})

    def test_security_and_page_runs_64k(self):
        s = self.info["security"]
        self.assertEqual(s["image_size"], 0x70000)
        self.assertEqual(s["load_address"], 0x82000000)
        self.assertEqual(s["page_size"], 0x10000)
        self.assertEqual(s["page_total"], 0x70000)        # 3 + 3 + 1 pages of 64 KB
        runs = [(r["start"], r["end"], r["kind"]) for r in s["page_runs"]]
        self.assertEqual(runs, [(0x82000000, 0x82030000, "read-only"),
                                (0x82030000, 0x82060000, "code"),
                                (0x82060000, 0x82070000, "data")])

    def test_page_runs_4k_flag(self):
        s = xexinfo.parse(build(PAGES, image_flags=0x10000000))["security"]
        self.assertEqual(s["page_size"], 0x1000)
        self.assertEqual(s["page_total"], 0x7000)
        self.assertEqual(s["page_runs"][1]["start"], 0x82003000)

    def test_static_libraries(self):
        libs = self.info["static_libraries"]
        self.assertEqual([(l["name"], l["major"], l["minor"], l["build"], l["qfe"]) for l in libs],
                         [("XAPILIB", 2, 0, 17559, 3), ("D3D9", 2, 0, 17559, 0)])

    def test_import_libraries(self):
        libs = self.info["import_libraries"]
        self.assertEqual([(l["name"], l["records"]) for l in libs],
                         [("xam.xex", 3), ("xboxkrnl.exe", 2)])
        self.assertEqual(libs[0]["version"], "2.0.17559.0")
        self.assertEqual(libs[0]["version_min"], "2.0.17408.0")  # 0x4400 = 17408
        self.assertEqual(self.info["import_records_total"], 5)

    def test_report_mentions_key_fields(self):
        text = xexinfo.report(self.info)
        self.assertIn("title id     41420007 (AB-7)", text)
        self.assertIn("82030000-82060000 code", text)

    def test_rejects_bad_magic(self):
        with self.assertRaises(ValueError):
            xexinfo.parse(build(PAGES, magic=b"XEX1"))

    def test_rejects_truncated_file(self):
        data = build(PAGES)
        with self.assertRaises(ValueError):
            xexinfo.parse(data[:0x100])  # optional-header data and security info cut off


if __name__ == "__main__":
    unittest.main(verbosity=2)
