# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Tests of survey/gdfx.py on synthetic disc images built here from the layout in its docstring (Xenia
# disc_image_device.cc:70-185). Expected rows are written by hand from the bytes placed.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_gdfx.py
import io, struct, sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'survey'))
import gdfx  # noqa: E402

S = gdfx.SECTOR


def entry(left, right, sector, size, attributes, name):
    e = struct.pack('<HHIIBB', left, right, sector, size, attributes, len(name)) + name
    return e + b'\xff' * (-len(e) % 4)


def image(game=0, root=None, sub=None, root_size=None, magic=gdfx.MAGIC):
    """Root (sector 34): "b.bin" at ordinal 0 (14 + 5 = 19 bytes, padded to 20), left -> "a" at byte 20 = ordinal 5
    (14 + 1 = 15, padded to 16), right -> "c" at byte 36 = ordinal 9; "a" is a directory (sector 35, 0x18 bytes) holding
    "x.dat"; files at sectors 40 and 41."""
    root = root if root is not None else (entry(5, 9, 40, 0x10, 0x00, b'b.bin') + entry(0, 0, 35, 0x18, 0x10, b'a')
                                          + entry(0, 0, 41, 0x20, 0x01, b'c'))
    sub = sub if sub is not None else entry(0, 0, 42, 0x30, 0x80, b'x.dat')
    data = bytearray(game + 43 * S)
    struct.pack_into('<20sII', data, game + 32 * S, magic, 34, root_size if root_size is not None else len(root))
    data[game + 34 * S:game + 34 * S + len(root)] = root
    data[game + 35 * S:game + 35 * S + len(sub)] = sub
    return bytes(data)


def rows(data):
    return gdfx.entries(io.BytesIO(data), len(data))


class Gdfx(unittest.TestCase):
    def test_tree_in_order(self):
        # In-order: left (a), then the node (b.bin), then right (c); a's child x.dat right after a.
        self.assertEqual(rows(image()), [('a', 35, 0x18, 0x10), ('a/x.dat', 42, 0x30, 0x80), ('b.bin', 40, 0x10, 0x00),
                                         ('c', 41, 0x20, 0x01)])

    def test_partition_offset(self):
        self.assertEqual(rows(image(game=0xFB20))[0], ('a', 35, 0x18, 0x10))

    def test_no_volume(self):
        with self.assertRaisesRegex(gdfx.GdfxError, 'no GDFX volume'):
            rows(image(magic=b'MICROSOFT*XBOX*MEDIX'))

    def test_root_size_bounds(self):
        with self.assertRaisesRegex(gdfx.GdfxError, 'outside 13..32 MB'):
            rows(image(root_size=12))

    def test_cycle(self):
        # "q" at ordinal 0 (16 bytes) links left to "r" at ordinal 4, whose left links back to ordinal 4 itself.
        with self.assertRaisesRegex(gdfx.GdfxError, 'a cycle'):
            rows(image(root=entry(4, 0, 40, 1, 0, b'q') + entry(4, 0, 40, 1, 0, b'r')))

    def test_entry_past_directory(self):
        with self.assertRaisesRegex(gdfx.GdfxError, 'past its directory data'):
            rows(image(root=entry(0, 9, 40, 1, 0, b'q')))

    def test_file_past_image(self):
        with self.assertRaisesRegex(gdfx.GdfxError, 'past the image'):
            rows(image(root=entry(0, 0, 40, 10 * S, 0, b'big')))


if __name__ == '__main__':
    unittest.main(verbosity=2)
