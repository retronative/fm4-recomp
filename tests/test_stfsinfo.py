# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# survey/stfsinfo.py against synthetic XContent packages (no game bytes).
#
#   /opt/homebrew/bin/python3 -I tests/test_stfsinfo.py
#
# Packages are assembled here from Xenia canary 67f16c4e315f stfs_xbox.h (XContentHeader 0x344,
# XContentMetadata, StfsVolumeDescriptor with little-endian file-table count and 24-bit block
# numbers, StfsHashEntry info word, StfsDirectoryEntry) and the block layout of
# stfs_container_device.cc. Expected physical blocks are hand-computed:
#   one table per level (read-only): data 0 -> 1, 169 -> 170, 170 -> 173
#     (L0 table 0 at 0, data 0..169 at 1..170, L1 table at 171, L0 table 1 at 172);
#   two tables per level: data 0 -> 2, 170 -> 176 (L1 tables 172-173, L0 tables 174-175).
# FM4_SURVEY_DIR (os.environ, works under -I) points the test at a mutated copy.
import importlib.util
import io
import os
import struct
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SURVEY = os.environ.get("FM4_SURVEY_DIR", os.path.join(HERE, "..", "survey"))
spec = importlib.util.spec_from_file_location("stfsinfo", os.path.join(SURVEY, "stfsinfo.py"))
stfsinfo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stfsinfo)

HEADER_SIZE = 0x971A          # XContentContainerHeader size; rounds up to base 0xA000
BASE = 0xA000


def u24le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])


def dir_entry(name, directory=False, parent=0xFFFF, length=0, start=0, blocks=0, contiguous=False):
    e = bytearray(0x40)
    e[:len(name)] = name.encode()
    e[40] = len(name) | (0x80 if directory else 0) | (0x40 if contiguous else 0)
    e[41:44] = u24le(blocks)
    e[44:47] = u24le(blocks)
    e[47:50] = u24le(start)
    e[50:52] = struct.pack(">H", parent)
    e[52:56] = struct.pack(">I", length)
    return bytes(e)


def header(magic=b"PIRS", read_only=True, root_active=False, table_blocks=1, table_start=0,
           total_blocks=3, volume_type=0, descriptor_length=0x24):
    h = bytearray(BASE)
    h[0:4] = magic
    h[0x340:0x344] = struct.pack(">I", HEADER_SIZE)
    h[0x344:0x348] = struct.pack(">I", 0x00000002)            # MarketplaceContent
    h[0x348:0x34C] = struct.pack(">I", 2)
    h[0x34C:0x354] = struct.pack(">Q", 0x123000)
    h[0x354:0x358] = struct.pack(">I", 0x0A0B0C0D)            # media id
    h[0x360:0x364] = struct.pack(">I", 0x41420007)            # title id
    h[0x366], h[0x367] = 1, 1
    v = 0x379
    h[v] = descriptor_length
    h[v + 2] = (1 if read_only else 0) | (2 if root_active else 0)
    h[v + 3:v + 5] = struct.pack("<H", table_blocks)
    h[v + 5:v + 8] = u24le(table_start)
    h[v + 0x1C:v + 0x20] = struct.pack(">I", total_blocks)
    h[0x39D:0x3A1] = struct.pack(">I", 0)
    h[0x3A9:0x3AD] = struct.pack(">I", volume_type)
    name = "Test Pack".encode("utf-16-be")
    h[0x411:0x411 + len(name)] = name
    title = "Synthetic".encode("utf-16-be")
    h[0x1691:0x1691 + len(title)] = title
    return h


def put_block(buf, physical, data):
    off = BASE + physical * 0x1000
    if len(buf) < off + 0x1000:
        buf.extend(b"\0" * (off + 0x1000 - len(buf)))
    buf[off:off + len(data)] = data


def hash_entry(next_block, active=False):
    return b"\0" * 0x14 + struct.pack(">I", (0x80000000) | (0x40000000 if active else 0) | next_block)


def single_table_package():
    """Read-only, file table at data block 0: dir 'media' with two files, one root file."""
    buf = header()
    table = (dir_entry("media", directory=True) +
             dir_entry("a.bin", parent=0, length=5000, start=1, blocks=2) +
             dir_entry("b.zip", parent=0, length=10, start=3, blocks=1, contiguous=True) +
             dir_entry("readme.txt", length=1, start=4, blocks=1))
    put_block(buf, 0, hash_entry(0xFFFFFF))                  # L0 table: block 0 ends its chain
    put_block(buf, 1, table)                                  # data block 0 -> physical 1
    return bytes(buf)


def chained_table_package():
    """File table of 2 blocks, data blocks 5 -> 9 linked through the L0 hash table."""
    buf = header(table_blocks=2, table_start=5, total_blocks=10)
    l0 = bytearray(0x1000)
    l0[5 * 0x18:6 * 0x18] = hash_entry(9)
    l0[9 * 0x18:10 * 0x18] = hash_entry(0xFFFFFF)
    put_block(buf, 0, l0)
    put_block(buf, 1 + 5, dir_entry("one.bin", length=1, start=1, blocks=1))
    put_block(buf, 1 + 9, dir_entry("two.bin", length=2, start=2, blocks=1))
    return bytes(buf)


def inspect_bytes(data):
    with tempfile.NamedTemporaryFile(suffix=".pkg", delete=False) as t:
        t.write(data)
    try:
        return stfsinfo.inspect(t.name)
    finally:
        os.unlink(t.name)


class Offsets(unittest.TestCase):
    def stfs(self, read_only):
        info = {"header_size": HEADER_SIZE, "stfs": {"descriptor_length": 0x24,
                "read_only_format": read_only, "root_active_index": False, "total_block_count": 1,
                "file_table_block_count": 1, "file_table_block_number": 0}}
        return stfsinfo.Stfs(io.BytesIO(), info)

    def test_read_only_block_to_offset(self):
        s = self.stfs(True)
        self.assertEqual(s.block_to_offset(0), BASE + 1 * 0x1000)
        self.assertEqual(s.block_to_offset(169), BASE + 170 * 0x1000)
        self.assertEqual(s.block_to_offset(170), BASE + 173 * 0x1000)

    def test_read_only_hash_tables(self):
        s = self.stfs(True)
        self.assertEqual(s.hash_block_number(0, 0), 0)
        self.assertEqual(s.hash_block_number(170, 0), 172)
        self.assertEqual(s.hash_block_number(170, 1), 171)

    def test_two_tables_per_level(self):
        s = self.stfs(False)
        self.assertEqual(s.block_to_offset(0), BASE + 2 * 0x1000)
        self.assertEqual(s.block_to_offset(170), BASE + 176 * 0x1000)
        self.assertEqual(s.hash_block_number(170, 0), 174)
        self.assertEqual(s.hash_block_number(170, 1), 172)


class Packages(unittest.TestCase):
    def test_header_fields(self):
        info = inspect_bytes(single_table_package())
        self.assertEqual(info["magic"], "PIRS")
        self.assertEqual(info["content_type"], 2)
        self.assertEqual(info["content_type_name"], "MarketplaceContent")
        self.assertEqual(info["title_id"], 0x41420007)
        self.assertEqual(info["media_id"], 0x0A0B0C0D)
        self.assertEqual(info["display_name"], "Test Pack")
        self.assertEqual(info["title_name"], "Synthetic")
        self.assertEqual(info["volume_type"], "STFS")
        self.assertEqual(info["stfs"]["file_table_block_count"], 1)

    def test_listing(self):
        info = inspect_bytes(single_table_package())
        self.assertEqual(info["file_count"], 3)
        self.assertEqual(info["directory_count"], 1)
        self.assertEqual(info["files_total_bytes"], 5011)
        self.assertEqual([f["path"] for f in info["files"]],
                         ["media/a.bin", "media/b.zip", "readme.txt"])

    def test_chained_file_table(self):
        info = inspect_bytes(chained_table_package())
        self.assertEqual([f["path"] for f in info["files"]], ["one.bin", "two.bin"])

    def test_little_endian_table_count(self):
        data = bytearray(single_table_package())
        info = stfsinfo.Package(io.BytesIO(bytes(data))).header()
        self.assertEqual(info["stfs"]["file_table_block_count"], 1)   # bytes 01 00
        data[0x379 + 3:0x379 + 5] = b"\x00\x01"                         # 256 as LE
        info = stfsinfo.Package(io.BytesIO(bytes(data))).header()
        self.assertEqual(info["stfs"]["file_table_block_count"], 256)

    def test_rejects_bad_magic(self):
        data = bytearray(single_table_package())
        data[0:4] = b"XEX2"
        with self.assertRaises(ValueError):
            inspect_bytes(bytes(data))

    def test_rejects_short_chain(self):
        buf = bytearray(single_table_package())
        buf[0x379 + 3:0x379 + 5] = struct.pack("<H", 2)   # claims 2 table blocks, chain ends at 1
        with self.assertRaises(ValueError):
            inspect_bytes(bytes(buf))

    def test_rejects_bad_descriptor(self):
        data = bytearray(single_table_package())
        data[0x379] = 0x20
        with self.assertRaises(ValueError):
            inspect_bytes(bytes(data))

    def test_root_active_index_selects_secondary_table(self):
        """Two tables per level, < 170 blocks (no upper levels): root_active_index picks the L0
        table at physical 1 over the one at 0. Data 0 -> physical 2, data 1 -> physical 3."""
        buf = header(read_only=False, root_active=True, table_blocks=2, table_start=0, total_blocks=5)
        primary, secondary = bytearray(0x1000), bytearray(0x1000)
        primary[0:0x18] = hash_entry(0xFFFFFF)          # stale chain: would end after 1 block
        secondary[0:0x18] = hash_entry(1)
        put_block(buf, 0, primary)
        put_block(buf, 1, secondary)
        put_block(buf, 2, dir_entry("first.bin", length=1, start=3, blocks=1))
        put_block(buf, 3, dir_entry("second.bin", length=1, start=4, blocks=1))
        info = inspect_bytes(bytes(buf))
        self.assertEqual([f["path"] for f in info["files"]], ["first.bin", "second.bin"])

    def test_level1_active_bit_selects_level0_table(self):
        """Two tables per level, 200 blocks (one upper level): the L1 table (physical 172, root
        inactive) record 0 has the active bit, so block 0's L0 table is read at physical 1."""
        buf = header(read_only=False, root_active=False, table_blocks=2, table_start=0, total_blocks=200)
        l1, primary, secondary = bytearray(0x1000), bytearray(0x1000), bytearray(0x1000)
        l1[0:0x18] = hash_entry(0, active=True)
        primary[0:0x18] = hash_entry(0xFFFFFF)
        secondary[0:0x18] = hash_entry(1)
        put_block(buf, 172, l1)
        put_block(buf, 0, primary)
        put_block(buf, 1, secondary)
        put_block(buf, 2, dir_entry("first.bin", length=1, start=3, blocks=1))
        put_block(buf, 3, dir_entry("second.bin", length=1, start=4, blocks=1))
        info = inspect_bytes(bytes(buf))
        self.assertEqual([f["path"] for f in info["files"]], ["first.bin", "second.bin"])

    def test_svod_header_only(self):
        buf = header(volume_type=1)
        info = inspect_bytes(bytes(buf))
        self.assertEqual(info["volume_type"], "SVOD")
        self.assertNotIn("file_count", info)


if __name__ == "__main__":
    unittest.main(verbosity=2)
