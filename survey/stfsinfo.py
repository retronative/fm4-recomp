# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Summarize an Xbox 360 XContent package (CON/LIVE/PIRS): header fields and, for STFS volumes,
# the file table. Reads only the header and the file-table blocks; never writes.
#
#   /opt/homebrew/bin/python3 -I survey/stfsinfo.py <package> [--json <out.json>] [--list]
#
# Layout and algorithms from Xenia canary 67f16c4e315f (BSD-3), cited per function:
#   src/xenia/vfs/devices/stfs_xbox.h: XContentHeader (0x344 bytes), XContentMetadata,
#     StfsVolumeDescriptor, StfsHashEntry, StfsHashTable, StfsDirectoryEntry, XContentPackageType
#   src/xenia/xbox.h: XContentType
#   src/xenia/vfs/devices/xcontent_devices/stfs_container_device.{h,cc}: SetupContainer,
#     Read, ReadEntry, BlockToOffset, BlockToHashBlockNumber, GetBlockHash
# Written for FM4 Recomp from those definitions; no Xenia code is copied.
import json
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (170, 28900, 4913000)   # kBlocksPerHashLevel
END_OF_CHAIN = 0xFFFFFF
MAGICS = {b"CON ": "CON", b"LIVE": "LIVE", b"PIRS": "PIRS"}
CONTENT_TYPES = {
    0x00000001: "SavedGame", 0x00000002: "MarketplaceContent", 0x00000003: "Publisher",
    0x00001000: "Xbox360Title", 0x00002000: "IptvPauseBuffer", 0x00003000: "XNACommunity",
    0x00004000: "InstalledGame", 0x00005000: "XboxTitle", 0x00006000: "SocialTitle",
    0x00007000: "GamesOnDemand", 0x00008000: "SUStoragePack", 0x00009000: "AvatarItem",
    0x00010000: "Profile", 0x00020000: "GamerPicture", 0x00030000: "Theme",
    0x00040000: "CacheFile", 0x00050000: "StorageDownload", 0x00060000: "XboxSavedGame",
    0x00070000: "XboxDownload", 0x00080000: "GameDemo", 0x00090000: "Video",
    0x000A0000: "GameTitle", 0x000B0000: "Installer", 0x000C0000: "GameTrailer",
    0x000D0000: "ArcadeTitle", 0x000E0000: "XNA", 0x000F0000: "LicenseStore",
    0x00100000: "Movie", 0x00200000: "TV", 0x00300000: "MusicVideo", 0x00400000: "GameVideo",
    0x00500000: "PodcastVideo", 0x00600000: "ViralVideo", 0x02000000: "CommunityGame",
}
# XContentHeader / XContentMetadata offsets (packed; header 0x344, metadata follows).
OFF_HEADER_SIZE = 0x340
OFF_CONTENT_TYPE = 0x344
OFF_METADATA_VERSION = 0x348
OFF_CONTENT_SIZE = 0x34C
OFF_EXEC = 0x354            # xex2_opt_execution_info (0x18)
OFF_VOLUME = 0x379          # StfsVolumeDescriptor / SvodDeviceDescriptor (0x24)
OFF_DATA_FILE_COUNT = 0x39D
OFF_DATA_FILE_SIZE = 0x3A1
OFF_VOLUME_TYPE = 0x3A9
OFF_DISPLAY_NAME = 0x411    # 9 languages x 128 UTF-16BE chars; English first
OFF_DESCRIPTION = 0xD11
OFF_PUBLISHER = 0x1611      # 64 chars
OFF_TITLE_NAME = 0x1691     # 64 chars
HEADER_MIN = 0x1712


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


def utf16(raw):
    text = raw.decode("utf-16-be", errors="replace")
    return text.split("\0")[0]


class Package:
    def __init__(self, f):
        self.f = f
        f.seek(0)
        self.h = f.read(HEADER_MIN)
        if len(self.h) < HEADER_MIN:
            raise ValueError("file shorter than an XContent header (0x%X bytes)" % HEADER_MIN)
        if self.h[:4] not in MAGICS:
            raise ValueError("not an XContent package (magic %r)" % self.h[:4])

    def be32(self, off):
        return struct.unpack_from(">I", self.h, off)[0]

    def be64(self, off):
        return struct.unpack_from(">Q", self.h, off)[0]

    def header(self):
        h, e = self.h, OFF_EXEC
        v = OFF_VOLUME
        volume_type = self.be32(OFF_VOLUME_TYPE)
        info = {
            "magic": MAGICS[h[:4]],
            "header_size": self.be32(OFF_HEADER_SIZE),
            "content_type": self.be32(OFF_CONTENT_TYPE),
            "content_type_name": CONTENT_TYPES.get(self.be32(OFF_CONTENT_TYPE), "unknown"),
            "metadata_version": self.be32(OFF_METADATA_VERSION),
            "content_size": self.be64(OFF_CONTENT_SIZE),
            "media_id": self.be32(e), "title_id": self.be32(e + 0xC),
            "disc_number": h[e + 0x12], "disc_count": h[e + 0x13],
            "data_file_count": self.be32(OFF_DATA_FILE_COUNT),
            "data_file_size": self.be64(OFF_DATA_FILE_SIZE),
            "volume_type": {0: "STFS", 1: "SVOD"}.get(volume_type, "unknown(%d)" % volume_type),
            "display_name": utf16(h[OFF_DISPLAY_NAME:OFF_DISPLAY_NAME + 256]),
            "description": utf16(h[OFF_DESCRIPTION:OFF_DESCRIPTION + 256]),
            "publisher": utf16(h[OFF_PUBLISHER:OFF_PUBLISHER + 128]),
            "title_name": utf16(h[OFF_TITLE_NAME:OFF_TITLE_NAME + 128]),
        }
        if volume_type == 0:
            flags = h[v + 2]
            info["stfs"] = {
                "descriptor_length": h[v], "version": h[v + 1],
                "read_only_format": bool(flags & 1), "root_active_index": bool(flags & 2),
                "file_table_block_count": h[v + 3] | (h[v + 4] << 8),   # host (LE) uint16
                "file_table_block_number": u24le(h[v + 5:v + 8]),
                "total_block_count": struct.unpack_from(">I", h, v + 0x1C)[0],
                "free_block_count": struct.unpack_from(">I", h, v + 0x20)[0],
            }
        return info


class Stfs:
    """Read-only walk of an STFS volume (Xenia StfsContainerDevice)."""

    def __init__(self, f, info):
        self.f = f
        s = info["stfs"]
        if s["descriptor_length"] != 0x24:
            raise ValueError("STFS volume descriptor length 0x%X, want 0x24" % s["descriptor_length"])
        self.read_only = s["read_only_format"]
        self.root_active = s["root_active_index"]
        self.total_blocks = s["total_block_count"]
        self.per_table = 1 if self.read_only else 2            # SetupContainer
        self.step = (PER_LEVEL[0] + self.per_table,
                     PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.per_table)
        self.base = (info["header_size"] + BLOCK - 1) & ~(BLOCK - 1)
        self.s = s
        self.tables = {}

    def block_to_offset(self, index):                           # BlockToOffset
        block = index
        for level_base in PER_LEVEL:
            block += ((index + level_base) // level_base) * self.per_table
            if index < level_base:
                break
        return self.base + (block << 12)

    def hash_block_number(self, index, level):                  # BlockToHashBlockNumber
        if level == 2:
            return self.step[1]
        if index < PER_LEVEL[level]:
            return 0 if level == 0 else self.step[level - 1]
        block = (index // PER_LEVEL[level]) * self.step[level]
        if level == 0:
            block += ((index // PER_LEVEL[1]) + 1) * self.per_table
            if index < PER_LEVEL[1]:
                return block
        return block + self.per_table

    def _read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise ValueError("read of 0x%X bytes at 0x%X past end of package" % (size, offset))
        return data

    def _table(self, offset):
        if offset not in self.tables:
            self.tables[offset] = self._read(offset, BLOCK)
        return self.tables[offset]

    def next_block(self, index):                                # GetBlockHash().level0_next_block
        secondary = BLOCK if self.root_active else 0
        levels = 0
        for level, per in enumerate(PER_LEVEL):                 # GetAmountOfHashLevelsToCheck
            if self.total_blocks < per:
                levels = level
                break
        if self.read_only:
            secondary, levels = 0, 0
        for level in range(levels, -1, -1):                     # UpdateCachedHashTables
            table = self._table(self.base + (self.hash_block_number(index, level) << 12) + secondary)
            record = index % PER_LEVEL[0] if level == 0 else (index // PER_LEVEL[level - 1]) % PER_LEVEL[0]
            info_raw = struct.unpack_from(">I", table, record * 0x18 + 0x14)[0]
            if level == 0:
                return info_raw & 0xFFFFFF
            secondary = BLOCK if info_raw & 0x40000000 else 0

    def entries(self):                                          # Read / ReadEntry
        out, index = [], self.s["file_table_block_number"]
        for n in range(self.s["file_table_block_count"]):
            block = self._read(self.block_to_offset(index), BLOCK)
            for m in range(BLOCK // 0x40):
                e = block[m * 0x40:(m + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[40]
                parent = struct.unpack_from(">H", e, 50)[0]
                name = e[:flags & 0x3F].decode("ascii", errors="replace")
                out.append({"name": name, "directory": bool(flags & 0x80),
                            "parent": None if parent == 0xFFFF else parent,
                            "length": struct.unpack_from(">I", e, 52)[0],
                            "allocated_blocks": u24le(e[44:47]),
                            "start_block": u24le(e[47:50])})
            if n + 1 == self.s["file_table_block_count"]:
                break
            index = self.next_block(index)
            if index == END_OF_CHAIN:
                raise ValueError("file table chain ends after %d of %d blocks"
                                 % (n + 1, self.s["file_table_block_count"]))
        for e in out:
            if e["parent"] is not None and not (0 <= e["parent"] < len(out) and out[e["parent"]]["directory"]):
                raise ValueError("entry %r names parent %r, not a directory" % (e["name"], e["parent"]))
        return out


def path_of(entries, e):
    parts = [e["name"]]
    while e["parent"] is not None:
        e = entries[e["parent"]]
        parts.append(e["name"])
    return "/".join(reversed(parts))


def inspect(path):
    with open(path, "rb") as f:
        info = Package(f).header()
        if info["volume_type"] == "STFS":
            entries = Stfs(f, info).entries()
            files = [e for e in entries if not e["directory"]]
            info["file_count"] = len(files)
            info["directory_count"] = len(entries) - len(files)
            info["files_total_bytes"] = sum(e["length"] for e in files)
            info["files"] = [{"path": path_of(entries, e), "length": e["length"]} for e in files]
    return info


def report(info, listing=False):
    out = ["magic %(magic)s  content type %(content_type)08X (%(content_type_name)s)  "
           "metadata v%(metadata_version)d  header size %(header_size)X" % info,
           "title id %(title_id)08X  media id %(media_id)08X  disc %(disc_number)d/%(disc_count)d" % info,
           "display name %r  title name %r  publisher %r" % (info["display_name"], info["title_name"], info["publisher"]),
           "volume %(volume_type)s  content size %(content_size)d  data files %(data_file_count)d" % info]
    if "stfs" in info:
        out.append("STFS read-only %(read_only_format)s  blocks %(total_block_count)d  "
                   "file table %(file_table_block_count)d block(s) at %(file_table_block_number)d" % info["stfs"])
        out.append("files %d (%d bytes) in %d directories"
                   % (info["file_count"], info["files_total_bytes"], info["directory_count"]))
        if listing:
            out += ["  %10d %s" % (f["length"], f["path"]) for f in info["files"]]
    return "\n".join(out) + "\n"


def main(argv):
    args = argv[1:]
    listing = "--list" in args
    args = [a for a in args if a != "--list"]
    if len(args) not in (1, 3) or (len(args) == 3 and args[1] != "--json"):
        print("usage: stfsinfo.py <package> [--json <out.json>] [--list]", file=sys.stderr)
        return 2
    info = inspect(args[0])
    sys.stdout.write(report(info, listing))
    if len(args) == 3:
        with open(args[2], "w") as f:
            json.dump(info, f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
