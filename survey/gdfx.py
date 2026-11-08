#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""The GDFX (Xbox 360 disc) directory of a disc image: every entry's path, first sector, size and attribute byte.

    /opt/homebrew/bin/python3 -I survey/gdfx.py <disc image (.iso)> <out .tsv>

Reads the image read-only and writes a manifest the runtime serves the extracted disc with (the extraction keeps
neither sectors nor attribute bytes): one line per entry, `path<TAB>sector<TAB>size<TAB>attributes` (hex), paths
with '/' and in GDFX tree order (in-order, depth first: Xenia's children order). Layout, re-derived from Xenia xenia-canary 67f16c4e315f
vfs/devices/disc_image_device.cc:70-185 (all fields little-endian):
  - the game partition starts at one of 0, 0xFB20, 0x20600, 0x2080000, 0xFD90000 bytes; its sector 32 (2048-byte
    sectors) begins "MICROSOFT*XBOX*MEDIA" and holds the root directory's sector (+20) and size (+24);
  - a directory is a binary tree of entries at 4-byte offsets: left (u16, in 4-byte units), right (u16), sector
    (u32), size (u32), attributes (u8, 0x10 = directory), name length (u8), name.
Fail-closed: a missing volume, a root size outside 13..32 MB, an entry or directory past the image, a tree cycle or
an entry past its directory's data stops the survey (exit 1); nothing is guessed.
"""
import struct, sys
from pathlib import Path

SECTOR = 2048
OFFSETS = (0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000)
MAGIC = b'MICROSOFT*XBOX*MEDIA'


class GdfxError(Exception):
    pass


def read(f, offset, size, image_size):
    if offset < 0 or offset + size > image_size:
        raise GdfxError(f'read of {size} bytes at {offset:#x} is past the image ({image_size:#x} bytes)')
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise GdfxError(f'short read at {offset:#x}')
    return data


def volume(f, image_size):
    """(game partition offset, root sector, root size)."""
    for offset in OFFSETS:
        at = offset + 32 * SECTOR
        if at + 28 <= image_size and read(f, at, 20, image_size) == MAGIC:
            root_sector, root_size = struct.unpack('<II', read(f, at + 20, 8, image_size))
            if not 13 <= root_size <= 32 << 20:
                raise GdfxError(f'root directory size {root_size} is outside 13..32 MB')
            return offset, root_sector, root_size
    raise GdfxError('no GDFX volume ("MICROSOFT*XBOX*MEDIA" at sector 32 of any known partition offset)')


def entries(f, image_size):
    """[(path, sector, size, attributes)] of every entry, depth first."""
    game, root_sector, root_size = volume(f, image_size)
    out = []

    def directory(sector, size, prefix):
        if size > 32 << 20:
            raise GdfxError(f'directory {prefix or "/"} is {size} bytes')
        data = read(f, game + sector * SECTOR, size, image_size)
        seen = set()

        def node(ordinal):
            if ordinal in seen:
                raise GdfxError(f'a cycle in directory {prefix or "/"} at entry {ordinal}')
            seen.add(ordinal)
            at = ordinal * 4
            if at + 14 > len(data):
                raise GdfxError(f'entry {ordinal} of {prefix or "/"} is past its directory data')
            left, right, esector, esize, attributes, length = struct.unpack_from('<HHIIBB', data, at)
            if at + 14 + length > len(data):
                raise GdfxError(f'the name of entry {ordinal} of {prefix or "/"} is past its directory data')
            if left:
                node(left)
            name = data[at + 14:at + 14 + length].decode('ascii')
            path = prefix + name
            out.append((path, esector, esize, attributes))
            if attributes & 0x10 and esize:
                directory(esector, esize, path + '/')
            elif not attributes & 0x10 and game + esector * SECTOR + esize > image_size:
                raise GdfxError(f'{path} ({esize} bytes at sector {esector}) is past the image')
            if right:
                node(right)

        node(0)

    directory(root_sector, root_size, '')
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: gdfx.py <disc image> <out .tsv>')
    image, out = Path(sys.argv[1]), Path(sys.argv[2])
    try:
        with open(image, 'rb') as f:
            rows = entries(f, image.stat().st_size)
    except (GdfxError, UnicodeDecodeError) as e:
        sys.exit(f'gdfx: {image}: {e}')
    # Kept in tree order (in-order, depth first): the runtime lists a directory's children in this order, as Xenia
    # (vfs/devices/disc_image_device.cc:126-187).
    out.write_text(''.join(f'{p}\t{s:x}\t{n:x}\t{a:02x}\n' for p, s, n, a in rows))
    print(f'gdfx: {len(rows)} entries ({sum(1 for r in rows if r[3] & 0x10)} directories) -> {out}')


if __name__ == '__main__':
    main()
