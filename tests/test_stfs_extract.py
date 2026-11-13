# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# survey/stfs_extract.py (and, when built, tools/survey/stfs_extract from survey/stfs_extract.cpp) against
# synthetic read-only STFS packages assembled here (no game bytes).
#
#   /opt/homebrew/bin/python3 -B -I tests/test_stfs_extract.py
#   FM4_STFS_EXTRACT_BIN=<binary>   the C++ tool to compare (default tools/survey/stfs_extract; its tests are
#                                   skipped, and say so, when it is not built)
#
# The package builder below is independent of survey/: it places blocks by the read-only layout
# (one table per level), restated from the hand-computed positions:
#   data i is preceded by (i // 170 + 1) level-0 tables, (0 if i < 170 else i // 28900 + 1) level-1 tables and
#   (0 if i < 28900 else 1) level-2 tables, so data 0 -> 1, 169 -> 170, 170 -> 173, 28900 -> 29074;
#   L0 table k sits just before data k*170, L1 table 0 at 171 and table j >= 1 just before L0 table 170*j,
#   L2 at 29071 (L2, L1 table 1, L0 table 170, then data 28900).
# Hash tree (Velocity 1690428a XboxInternals/Stfs/StfsPackage.cpp HashBlock/Rehash, facts only): each L0 entry is
# SHA-1 of its full 0x1000-byte data block (info word: 0x80000000 | next block, 0xFFFFFF ends a chain), each L1
# entry SHA-1 of an L0 table block, each L2 entry SHA-1 of an L1 table block, the top table's SHA-1 at volume
# descriptor +0x08; top level 0 for <= 170 blocks, 1 for <= 28900, else 2. Expected file bytes are the bytes placed;
# expected mtimes are hand-computed from the FAT values (2011-10-11 13:45:30 UTC = 1318340730,
# 2010-01-02 03:04:06 UTC = 1262401446).
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
SURVEY = os.environ.get("FM4_SURVEY_DIR", os.path.join(REPO, "survey"))
CXX = os.environ.get("FM4_STFS_EXTRACT_BIN", os.path.join(REPO, "tools", "survey", "stfs_extract"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


stfs_extract = load("stfs_extract", os.path.join(SURVEY, "stfs_extract.py"))
stfsinfo = stfs_extract.stfsinfo
builders = load("test_stfsinfo", os.path.join(HERE, "test_stfsinfo.py"))   # header(), u24le()

BASE = builders.BASE
B = 0x1000
END = 0xFFFFFF
FAT_2011 = 0x3F4B6DAF      # date (31 << 9 | 10 << 5 | 11), time (13 << 11 | 45 << 5 | 15)
FAT_2010 = 0x3C221883      # date (30 << 9 | 1 << 5 | 2), time (3 << 11 | 4 << 5 | 3)
EPOCH_2011 = 1318340730
EPOCH_2010 = 1262401446


def physical(i):
    return (i + i // 170 + 1 + (0 if i < 170 else i // 28900 + 1) + (0 if i < 28900 else 1))


def l0_physical(k):
    return physical(k * 170) - 1


def l1_physical(j):
    return 171 if j == 0 else physical(j * 28900) - 2


L2_PHYSICAL = 29071


class Layout(unittest.TestCase):
    """The builder's placement against the hand-computed positions (and the parser's own functions)."""

    def test_hand_positions(self):
        self.assertEqual([physical(0), physical(169), physical(170), physical(28900)], [1, 170, 173, 29074])
        self.assertEqual([l0_physical(0), l0_physical(1), l0_physical(170)], [0, 172, 29073])
        self.assertEqual([l1_physical(0), l1_physical(1)], [171, 29072])

    def test_parser_agrees(self):
        info = {"header_size": builders.HEADER_SIZE, "stfs": {"descriptor_length": 0x24,
                "read_only_format": True, "root_active_index": False, "total_block_count": 30000,
                "file_table_block_count": 1, "file_table_block_number": 0, "top_hash_table_hash": "00" * 20}}
        s = stfs_extract.VerifiedStfs(io.BytesIO(), info)
        self.assertEqual(s.block_to_offset(28900), BASE + 29074 * B)
        self.assertEqual(s.table_physical(0, 170), 29073)
        self.assertEqual(s.table_physical(1, 1), 29072)
        self.assertEqual(s.table_physical(2, 0), L2_PHYSICAL)
        self.assertEqual(s.top, 2)

    def test_top_levels(self):
        self.assertEqual([stfs_extract.top_level(n) for n in (1, 170, 171, 28900, 28901)], [0, 0, 1, 1, 2])


def entry(name, directory=False, parent=0xFFFF, length=0, start=0, blocks=0, created=0, modified=0):
    e = bytearray(0x40)
    raw = name.encode("latin-1")
    e[:len(raw)] = raw
    e[40] = len(raw) | (0x80 if directory else 0)
    e[41:44] = builders.u24le(blocks)
    e[44:47] = builders.u24le(blocks)
    e[47:50] = builders.u24le(start)
    e[50:52] = struct.pack(">H", parent)
    e[52:56] = struct.pack(">I", length)
    e[56:60] = struct.pack(">I", created)
    e[60:64] = struct.pack(">I", modified)
    return bytes(e)


class Pkg:
    """A read-only package: data blocks by index, next pointers, then the hash tree on build()."""

    def __init__(self, total_blocks):
        self.total = total_blocks
        self.data = {}
        self.next = {}

    def chain(self, blocks, content):
        for n, i in enumerate(blocks):
            self.data[i] = content[n * B:(n + 1) * B]
            self.next[i] = blocks[n + 1] if n + 1 < len(blocks) else END

    def build(self, table_entries, table_block=0, read_only=True, corrupt=None):
        self.chain([table_block], b"".join(table_entries))
        top = 0 if self.total <= 170 else 1 if self.total <= 28900 else 2
        buf = bytearray(builders.header(read_only=read_only, table_blocks=1, table_start=table_block,
                                        total_blocks=self.total))

        def put(phys, block):
            off = BASE + phys * B
            if len(buf) < off + B:
                buf.extend(b"\0" * (off + B - len(buf)))
            buf[off:off + len(block)] = block

        def padded(b):
            return b + b"\0" * (B - len(b))

        l0 = []
        for k in range((self.total + 169) // 170):
            t = bytearray(B)
            for i in range(k * 170, min((k + 1) * 170, self.total)):
                if i in self.data:
                    r = (i % 170) * 0x18
                    t[r:r + 0x14] = hashlib.sha1(padded(self.data[i])).digest()
                    t[r + 0x14:r + 0x18] = struct.pack(">I", 0x80000000 | self.next[i])
                    put(physical(i), self.data[i])
            l0.append(bytes(t))
        tables = [l0]
        if top >= 1:
            l1 = []
            for j in range((len(l0) + 169) // 170):
                t = bytearray(B)
                group = l0[j * 170:(j + 1) * 170]
                for k, table in enumerate(group):
                    t[k * 0x18:k * 0x18 + 0x14] = hashlib.sha1(table).digest()
                t[0xFF0:0xFF4] = struct.pack(">I", min(28900, self.total - j * 28900))
                l1.append(bytes(t))
            tables.append(l1)
        if top == 2:
            t = bytearray(B)
            for j, table in enumerate(tables[1]):
                t[j * 0x18:j * 0x18 + 0x14] = hashlib.sha1(table).digest()
            t[0xFF0:0xFF4] = struct.pack(">I", self.total)
            tables.append([bytes(t)])
        if corrupt is not None:
            corrupt(tables, self)
        for k, t in enumerate(tables[0]):
            put(l0_physical(k), t)
        if top >= 1:
            for j, t in enumerate(tables[1]):
                put(l1_physical(j), t)
        if top == 2:
            put(L2_PHYSICAL, tables[2][0])
        buf[0x379 + 8:0x379 + 0x1C] = hashlib.sha1(tables[top][0]).digest()
        for i, d in self.data.items():           # data written after tables (corrupt() may change self.data)
            put(physical(i), d)
        return bytes(buf)


A_BIN = bytes((n * 7 + 3) & 0xFF for n in range(5000))
README = b"FM4 test readme\n"
SPLIT = bytes((n * 13) & 0xFF for n in range(3 * B - 100))


def simple_package(total=10):
    """media/ (dir) holding a.bin (5000 bytes, blocks 1 -> 2) and empty.dat; readme.txt (block 3) at the root."""
    p = Pkg(total)
    p.chain([1, 2], A_BIN)
    p.chain([3], README)
    table = [entry("media", directory=True, created=FAT_2010, modified=FAT_2011),
             entry("a.bin", parent=0, length=len(A_BIN), start=1, blocks=2, modified=FAT_2011),
             entry("empty.dat", parent=0, modified=FAT_2010),
             entry("readme.txt", length=len(README), start=3, blocks=1, created=FAT_2011, modified=FAT_2010)]
    return p, table


def write(tmp, data, name="pkg"):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


class Extract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="stfs_extract_test.")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_py(self, data):
        pkg = write(self.tmp, data)
        out, js = os.path.join(self.tmp, "out"), os.path.join(self.tmp, "out.header.json")
        stfs_extract.extract(pkg, out, js)
        return out, js

    def read(self, out, rel):
        with open(os.path.join(out, rel), "rb") as f:
            return f.read()

    def test_tree_bytes_and_times(self):
        p, table = simple_package()
        out, js = self.run_py(p.build(table))
        self.assertEqual(self.read(out, "media/a.bin"), A_BIN)
        self.assertEqual(self.read(out, "media/empty.dat"), b"")
        self.assertEqual(self.read(out, "readme.txt"), README)
        self.assertEqual(sorted(os.listdir(out)), ["media", "readme.txt"])
        self.assertEqual(int(os.stat(os.path.join(out, "media/a.bin")).st_mtime), EPOCH_2011)
        self.assertEqual(int(os.stat(os.path.join(out, "readme.txt")).st_mtime), EPOCH_2010)
        self.assertEqual(int(os.stat(os.path.join(out, "media")).st_mtime), EPOCH_2011)

    def test_header_json_is_stfsinfo_json(self):
        p, table = simple_package()
        data = p.build(table)
        out, js = self.run_py(data)
        ref = os.path.join(self.tmp, "ref.json")
        stfsinfo.write_json(stfsinfo.inspect(os.path.join(self.tmp, "pkg")), ref)
        with open(js, "rb") as a, open(ref, "rb") as b:
            self.assertEqual(a.read(), b.read())
        with open(js) as f:
            info = json.load(f)
        self.assertEqual([f["path"] for f in info["files"]], ["media/a.bin", "media/empty.dat", "readme.txt"])
        self.assertEqual(info["files"][0]["modified_fat"], FAT_2011)
        self.assertEqual(info["directories"], [{"path": "media", "created_fat": FAT_2010, "modified_fat": FAT_2011}])
        self.assertEqual(info["stfs"]["top_hash_table_hash"], data[0x379 + 8:0x379 + 0x1C].hex())

    def test_out_of_order_chain(self):
        p = Pkg(12)
        p.chain([9, 4, 7], SPLIT)
        out, _ = self.run_py(p.build([entry("split.bin", length=len(SPLIT), start=9, blocks=3)]))
        self.assertEqual(self.read(out, "split.bin"), SPLIT)

    def test_two_levels(self):
        """400 blocks (top level 1): a file over data 168..172 crosses L0 tables 0 and 1."""
        p = Pkg(400)
        content = bytes((n * 5 + 1) & 0xFF for n in range(5 * B))
        p.chain(list(range(168, 173)), content)
        out, _ = self.run_py(p.build([entry("cross.bin", length=len(content), start=168, blocks=5)]))
        self.assertEqual(self.read(out, "cross.bin"), content)

    def test_three_levels(self):
        """28902 blocks (top level 2, as each of Disc 2's packages): data 28899..28901 crosses L1 tables 0 and 1."""
        p = Pkg(28902)
        p.chain([28899, 28900, 28901], SPLIT)
        out, _ = self.run_py(p.build([entry("deep.bin", length=len(SPLIT), start=28899, blocks=3, modified=FAT_2011)]))
        self.assertEqual(self.read(out, "deep.bin"), SPLIT)

    # Negative controls: each must stop with its named cause.

    def assert_fails(self, data, pattern):
        with self.assertRaisesRegex(ValueError, pattern):
            self.run_py(data)

    def test_corrupt_data_block(self):
        p, table = simple_package()
        def flip(tables, pkg):
            pkg.data[2] = b"\xFF" + pkg.data[2][1:]
        self.assert_fails(p.build(table, corrupt=flip), "data block 2 does not match")

    def test_corrupt_level0_table(self):
        p = Pkg(400)
        p.chain(list(range(168, 173)), bytes(5 * B))
        def flip(tables, pkg):
            t = bytearray(tables[0][1]); t[0x14 + 3] ^= 1; tables[0][1] = bytes(t)   # a next pointer, not rehashed
        self.assert_fails(p.build([entry("x", length=5 * B, start=168, blocks=5)], corrupt=flip),
                          "level-0 hash table 1")

    def test_corrupt_level1_table(self):
        p = Pkg(28902)
        p.chain([28899, 28900, 28901], SPLIT)
        def flip(tables, pkg):
            t = bytearray(tables[1][1]); t[0xFF0] ^= 1; tables[1][1] = bytes(t)
        self.assert_fails(p.build([entry("x", length=len(SPLIT), start=28899, blocks=3)], corrupt=flip),
                          "level-1 hash table 1")

    def test_bad_top_hash(self):
        p, table = simple_package()
        data = bytearray(p.build(table))
        data[0x379 + 8] ^= 0x80
        self.assert_fails(bytes(data), "level-0 hash table 0")

    def test_truncated_package(self):
        p, table = simple_package()
        data = p.build(table)
        self.assert_fails(data[:BASE + physical(3) * B + 100], "past end of package")

    def test_chain_ends_early(self):
        p = Pkg(12)
        p.chain([5, 6], bytes(2 * B))                         # 6 ends the chain; the entry claims 3 blocks
        self.assert_fails(p.build([entry("short.bin", length=3 * B, start=5, blocks=3)]), "chain ends after 2 of 3")

    def test_length_past_allocation(self):
        p = Pkg(12)
        p.chain([5, 6, 7], bytes(3 * B))
        self.assert_fails(p.build([entry("long.bin", length=3 * B + 1, start=5, blocks=3)]),
                          "need 4 blocks, the entry allocates 3")

    def test_block_past_volume(self):
        p = Pkg(12)
        p.chain([5], bytes(B))
        self.assert_fails(p.build([entry("far.bin", length=B, start=12, blocks=1)]), "past the volume")

    def test_unsafe_names(self):
        for name in ("..", ".", "a/b", "a\\b", "c:d", "caf\xe9"):
            with self.subTest(name=name):
                p = Pkg(4)
                self.assert_fails(p.build([entry(name)]), "unsafe entry name")

    def test_case_insensitive_duplicate(self):
        p = Pkg(4)
        self.assert_fails(p.build([entry("Data.bin"), entry("data.BIN")]), "duplicate path")

    def test_directory_cycle(self):
        p = Pkg(4)
        self.assert_fails(p.build([entry("a", directory=True, parent=1), entry("b", directory=True, parent=0)]),
                          "cycle")

    def test_not_read_only(self):
        p, table = simple_package()
        self.assert_fails(p.build(table, read_only=False), "not a read-only STFS package")

    def test_refuses_existing_outputs(self):
        p, table = simple_package()
        pkg = write(self.tmp, p.build(table))
        os.mkdir(os.path.join(self.tmp, "out"))
        with self.assertRaisesRegex(ValueError, "never overwritten"):
            stfs_extract.extract(pkg, os.path.join(self.tmp, "out"), os.path.join(self.tmp, "j.json"))


@unittest.skipUnless(os.access(CXX, os.X_OK), "C++ stfs_extract not built (%s): run scripts/setup_tools.sh" % CXX)
class CxxParity(unittest.TestCase):
    """The C++ tool's tree, mtimes and header.json are byte-identical to the Python tool's; its failures exit 1."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="stfs_extract_cxx.")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def both(self, data):
        pkg = write(self.tmp, data)
        py = (os.path.join(self.tmp, "py"), os.path.join(self.tmp, "py.json"))
        cx = (os.path.join(self.tmp, "cx"), os.path.join(self.tmp, "cx.json"))
        stfs_extract.extract(pkg, *py)
        r = subprocess.run([CXX, pkg, *cx], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return py, cx

    def snapshot(self, root):
        rows = []
        for d, dirs, files in os.walk(root):
            dirs.sort()
            for name in sorted(dirs + files):
                path = os.path.join(d, name)
                st = os.stat(path)
                body = None if os.path.isdir(path) else open(path, "rb").read()
                rows.append((os.path.relpath(path, root), body, int(st.st_mtime)))
        return rows

    def compare(self, data):
        (py, pyj), (cx, cxj) = self.both(data)
        self.assertEqual(self.snapshot(py), self.snapshot(cx))
        with open(pyj, "rb") as a, open(cxj, "rb") as b:
            self.assertEqual(a.read(), b.read())

    def test_simple(self):
        p, table = simple_package()
        self.compare(p.build(table))

    def test_three_levels(self):
        p = Pkg(28902)
        p.chain([28899, 28900, 28901], SPLIT)
        self.compare(p.build([entry("deep.bin", length=len(SPLIT), start=28899, blocks=3, modified=FAT_2011)]))

    def test_header_strings(self):
        """Non-ASCII UTF-16 display names must be escaped as Python's json.dump does (\\uXXXX, surrogate pairs)."""
        p, table = simple_package()
        data = bytearray(p.build(table))
        name = "Forza é中\U0001F3CE\"\\\t".encode("utf-16-be")
        data[0x411:0x411 + 256] = (name + b"\0" * 256)[:256]
        data[0xD11:0xD11 + 4] = b"\xD8\x00\x00A"           # a lone high surrogate, then 'A'
        self.compare(bytes(data))

    def test_failures_exit_1(self):
        p, table = simple_package()
        good = p.build(table)
        bad = bytearray(good); bad[BASE + physical(2) * B] ^= 1
        top = bytearray(good); top[0x379 + 8] ^= 1
        for label, data in (("data", bytes(bad)), ("top", bytes(top)), ("truncated", good[:BASE + physical(3) * B + 1])):
            with self.subTest(label):
                pkg = write(self.tmp, data, label)
                r = subprocess.run([CXX, pkg, os.path.join(self.tmp, label + ".out"),
                                    os.path.join(self.tmp, label + ".json")], capture_output=True, text=True)
                self.assertEqual(r.returncode, 1, r.stderr)

    def test_usage_exit_2(self):
        self.assertEqual(subprocess.run([CXX], capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
