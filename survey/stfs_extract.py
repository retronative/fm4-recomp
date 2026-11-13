# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Unpack a read-only STFS package (Disc 2's PIRS content packages) into a directory, every block checked
# against the package's SHA-1 hash tree, and write the package's header.json (survey/stfsinfo.py --json).
#
#   /opt/homebrew/bin/python3 -I survey/stfs_extract.py <package> <out dir> <out header.json>
#
# Neither output may exist (never overwritten). Fail-closed (exit 1, a message naming the block or entry): a
# package that is not a read-only STFS volume, any hash mismatch at any level, a chain that ends or leaves the
# volume before a file's last block, a file whose block count is not its allocated_data_blocks, an unsafe or
# duplicate (case-insensitive) name, a directory cycle. A failed run leaves its partial outputs for inspection.
# survey/stfs_extract.cpp is the same tool in C++ for the player's setup (no Python there); its outputs must be
# byte-identical (tests/test_stfs_extract.py compares them).
#
# Layout (block numbering, hash-table placement, chain) from Xenia via survey/stfsinfo.py (Xenia canary
# 67f16c4e315f stfs_container_device.cc; ShiftGlue 72b7dd6f src/filesystem/devices/stfs_container_device.cpp:
# 539-788). What each SHA-1 covers is not checked by Xenia (StfsHashEntry.sha1 is only declared); it is taken from
# Velocity 1690428a XboxInternals/Stfs/StfsPackage.cpp (facts only): HashBlock is SHA-1 of a full 0x1000-byte
# block; Rehash hashes each data block into its level-0 entry, each level-0 table block into its level-1 entry,
# each level-1 table into its level-2 entry, and the top table into the volume descriptor's top_hash_table_hash
# (+0x08); CalcualateTopLevel: top level 0 when total blocks <= 0xAA, 1 when <= 0x70E4, 2 when <= 0x4AF768.
# Timestamps: the entry's modified FAT date/time (+0x3C) decoded as ShiftGlue stfs_xbox.h:32-54
# (decode_fat_timestamp, UTC) becomes the host mtime and atime; an invalid date/time leaves the host time alone
# (header.json keeps the raw values either way).
import calendar
import hashlib
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("stfsinfo", os.path.join(HERE, "stfsinfo.py"))
stfsinfo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stfsinfo)

BLOCK = stfsinfo.BLOCK
PER_LEVEL = stfsinfo.PER_LEVEL
END_OF_CHAIN = stfsinfo.END_OF_CHAIN
TOP_LEVEL_LIMITS = (0xAA, 0x70E4, 0x4AF768)


class StfsError(ValueError):
    pass


def top_level(total_blocks):
    for level, limit in enumerate(TOP_LEVEL_LIMITS):
        if total_blocks <= limit:
            return level
    raise StfsError("total block count %d exceeds a level-2 hash tree" % total_blocks)


class VerifiedStfs(stfsinfo.Stfs):
    """stfsinfo.Stfs with every table and data block checked against its parent hash, top-down."""

    def __init__(self, f, info):
        super().__init__(f, info)
        if not self.read_only:
            raise StfsError("not a read-only STFS package (two tables per level are not supported)")
        self.top = top_level(self.total_blocks)
        self.top_hash = bytes.fromhex(info["stfs"]["top_hash_table_hash"])
        self.verified = {}

    def table_physical(self, level, number):        # table <number> of <level> (one table per level)
        return self.hash_block_number(number * PER_LEVEL[level], level)

    def table(self, level, number):
        key = (level, number)
        if key not in self.verified:
            if level == self.top:
                if number != 0:
                    raise StfsError("level-%d table %d past the top table" % (level, number))
                want = self.top_hash
            else:
                parent = self.table(level + 1, number // PER_LEVEL[0])
                record = number % PER_LEVEL[0]
                want = parent[record * 0x18:record * 0x18 + 0x14]
            data = self._read(self.base + (self.table_physical(level, number) << 12), BLOCK)
            if hashlib.sha1(data).digest() != want:
                raise StfsError("level-%d hash table %d (physical block %d) does not match its hash"
                                % (level, number, self.table_physical(level, number)))
            self.verified[key] = data
        return self.verified[key]

    def _entry(self, index):
        if index >= self.total_blocks:
            raise StfsError("data block %d is past the volume (%d blocks)" % (index, self.total_blocks))
        record = index % PER_LEVEL[0]
        table = self.table(0, index // PER_LEVEL[0])
        return table[record * 0x18:record * 0x18 + 0x18]

    def data_block(self, index):
        entry = self._entry(index)
        data = self._read(self.block_to_offset(index), BLOCK)
        if hashlib.sha1(data).digest() != entry[:0x14]:
            raise StfsError("data block %d does not match its level-0 hash" % index)
        return data

    def next_block(self, index):
        return struct.unpack_from(">I", self._entry(index), 0x14)[0] & 0xFFFFFF


def safe_name(name):
    return (name not in ("", ".", "..") and len(name) <= 40 and all(0x20 <= ord(c) < 0x7F for c in name)
            and "/" not in name and "\\" not in name and ":" not in name)


def fat_time(value):
    """decode_fat_timestamp's fields; None unless a real date and time (month 1-12, day 1-31, h<24, m<60, s<60)."""
    date, time = value >> 16, value & 0xFFFF
    year, month, day = ((date & 0xFE00) >> 9) + 1980, (date & 0x01E0) >> 5, date & 0x001F
    hour, minute, second = (time & 0xF800) >> 11, (time & 0x07E0) >> 5, (time & 0x001F) << 1
    if not (1 <= month <= 12 and 1 <= day <= 31 and hour < 24 and minute < 60 and second < 60):
        return None
    return calendar.timegm((year, month, day, hour, minute, second, 0, 0, 0))


def plan(entries):
    """Entry index -> relative path, after the name, cycle and duplicate checks."""
    paths, seen = [], {}
    for e in entries:
        if not safe_name(e["name"]):
            raise StfsError("unsafe entry name %r" % e["name"])
    for e in entries:
        path = stfsinfo.path_of(entries, e)
        folded = path.lower()
        if folded in seen:
            raise StfsError("duplicate path %r (case-insensitive) after %r" % (path, seen[folded]))
        seen[folded] = path
        paths.append(path)
    return paths


def extract(package, out_dir, out_json):
    for out in (out_dir, out_json):
        if os.path.lexists(out):
            raise StfsError("%s exists (never overwritten)" % out)
    with open(package, "rb") as f:
        info = stfsinfo.Package(f).header()
        if info["volume_type"] != "STFS":
            raise StfsError("volume type %s, not STFS" % info["volume_type"])
        stfs = VerifiedStfs(f, info)
        entries = stfs.entries()
        paths = plan(entries)
        os.mkdir(out_dir)
        for e, path in zip(entries, paths):     # parents may follow their children in the table
            if e["directory"]:
                os.makedirs(os.path.join(out_dir, path), exist_ok=True)
        for e, path in zip(entries, paths):
            if e["directory"]:
                continue
            blocks = (e["length"] + BLOCK - 1) // BLOCK
            if blocks != e["allocated_blocks"]:
                raise StfsError("%s: %d bytes need %d blocks, the entry allocates %d"
                                % (path, e["length"], blocks, e["allocated_blocks"]))
            target = os.path.join(out_dir, path)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            index, left = e["start_block"], e["length"]
            with open(target, "xb") as out:
                for n in range(blocks):
                    if index == END_OF_CHAIN:
                        raise StfsError("%s: chain ends after %d of %d blocks" % (path, n, blocks))
                    data = stfs.data_block(index)
                    out.write(data[:min(BLOCK, left)])
                    left -= BLOCK
                    if n + 1 < blocks:
                        index = stfs.next_block(index)
        for e, path in sorted(zip(entries, paths), key=lambda p: -p[1].count("/")):   # deepest first
            when = fat_time(e["modified_fat"])
            if when is not None:
                os.utime(os.path.join(out_dir, path), (when, when))
        stfsinfo.describe(info, entries)
    stfsinfo.write_json(info, out_json)
    return info


def main(argv):
    if len(argv) != 4:
        print("usage: stfs_extract.py <package> <out dir> <out header.json>", file=sys.stderr)
        return 2
    try:
        info = extract(argv[1], argv[2], argv[3])
    except (ValueError, OSError) as error:
        print("stfs_extract: %s: %s" % (argv[1], error), file=sys.stderr)
        return 1
    print("stfs_extract: %s: %d files (%d bytes) in %d directories, every block verified"
          % (argv[1], info["file_count"], info["files_total_bytes"], info["directory_count"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
