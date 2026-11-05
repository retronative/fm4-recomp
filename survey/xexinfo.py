# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Summarize an Xbox 360 XEX2 header from its plaintext fields only (no decryption).
#
#   /opt/homebrew/bin/python3 -I survey/xexinfo.py <file.xex> [--json <out.json>]
#
# Adapted from ReCarbon reference/analysis/xexinfo.py (ReCarbon, GPL-3.0-or-later).
# FM4 Recomp changes: parse() returns a dict (testable without game bytes), bounds-checked reads
# that fail with ValueError, the inline-value rule for optional headers, image flags decoded,
# JSON output. Layout from Xenia canary 67f16c4e315f (BSD-3), src/xenia/kernel/util/xex2_info.h
# (xex2_header, xex2_security_info, xex2_page_descriptor, xex2_opt_*) and
# src/xenia/cpu/xex_module.cc XexModule::GetOptHeader (key low byte 0x00/0x01: value inline).
import json
import struct
import sys

KEY_NAMES = {
    0x000002FF: "resource info", 0x000003FF: "file format", 0x000005FF: "delta patch",
    0x00000405: "base reference", 0x000080FF: "bounding path", 0x00008105: "device id",
    0x00010001: "original base", 0x00010100: "entry point", 0x00010201: "image base",
    0x000103FF: "import libraries", 0x00018002: "checksum/timestamp",
    0x00018102: "enabled for callcap", 0x00018200: "enabled for fastcap",
    0x000183FF: "original PE name", 0x000200FF: "static libraries", 0x00020104: "TLS info",
    0x00020200: "default stack size", 0x00020301: "fs cache size", 0x00020401: "default heap size",
    0x00028002: "page heap", 0x00030000: "system flags", 0x00040006: "execution info",
    0x00040201: "title workspace", 0x00040310: "game ratings", 0x00040404: "LAN key",
    0x000405FF: "Xbox 360 logo", 0x000406FF: "multidisc media ids", 0x000407FF: "alt title ids",
    0x00040801: "additional title memory", 0x00E10402: "exports by name",
}
SECTION_KINDS = {1: "code", 2: "data", 3: "read-only"}  # xex2_section_type
ENCRYPTION = {0: "none", 1: "normal"}
COMPRESSION = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}
PAGE_SIZE_4KB_FLAG = 0x10000000  # XEX_IMAGE_PAGE_SIZE_4KB; otherwise 64 KB pages


class Reader:
    def __init__(self, data):
        self.d = data

    def _unpack(self, fmt, off):
        if off < 0 or off + struct.calcsize(fmt) > len(self.d):
            raise ValueError("read of %s at 0x%X past end of file (0x%X bytes)" % (fmt, off, len(self.d)))
        return struct.unpack_from(fmt, self.d, off)[0]

    def u32(self, off):
        return self._unpack(">I", off)

    def u16(self, off):
        return self._unpack(">H", off)

    def u8(self, off):
        return self._unpack(">B", off)

    def bytes(self, off, n):
        if off < 0 or off + n > len(self.d):
            raise ValueError("read of %d bytes at 0x%X past end of file" % (n, off))
        return self.d[off:off + n]

    def cstr(self, off, limit):
        raw = self.bytes(off, limit)
        return raw.split(b"\0")[0].decode("ascii")


def version(v):
    """XEX version word as major.minor.build.qfe, major in the top nibble.

    Xenia declares xex2_version as bitfields major:4 minor:4 build:16 qfe:8, which on a
    little-endian host would put major in the low nibble; ReCarbon decodes major from the top
    nibble. The raw value is always reported alongside.
    """
    return "%d.%d.%d.%d" % (v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF)


def parse(data):
    r = Reader(data)
    if r.bytes(0, 4) != b"XEX2":
        raise ValueError("not an XEX2 file (magic %r)" % r.bytes(0, 4))
    info = {
        "module_flags": r.u32(0x4),
        "pe_data_offset": r.u32(0x8),
        "security_offset": r.u32(0x10),
        "optional_headers": [],
    }
    count = r.u32(0x14)
    hdr = {}
    for i in range(count):
        key, value = r.u32(0x18 + i * 8), r.u32(0x1C + i * 8)
        inline = (key & 0xFF) in (0x00, 0x01)
        hdr[key] = value
        info["optional_headers"].append(
            {"key": key, "name": KEY_NAMES.get(key, "?"), "value": value, "inline": inline})

    for key, field in ((0x00010100, "entry_point"), (0x00010201, "image_base"),
                       (0x00010001, "original_base"), (0x00020200, "default_stack_size"),
                       (0x00020401, "default_heap_size"), (0x00030000, "system_flags")):
        if key in hdr:
            info[field] = hdr[key]

    if 0x00040006 in hdr:  # xex2_opt_execution_info
        o = hdr[0x00040006]
        info["execution"] = {
            "media_id": r.u32(o), "version_raw": r.u32(o + 4), "version": version(r.u32(o + 4)),
            "base_version_raw": r.u32(o + 8), "base_version": version(r.u32(o + 8)),
            "title_id": r.u32(o + 0xC), "platform": r.u8(o + 0x10),
            "executable_type": r.u8(o + 0x11), "disc_number": r.u8(o + 0x12),
            "disc_count": r.u8(o + 0x13), "savegame_id": r.u32(o + 0x14),
        }
    if 0x000003FF in hdr:  # xex2_opt_file_format_info
        o = hdr[0x000003FF]
        info["file_format"] = {"encryption": ENCRYPTION.get(r.u16(o + 4), r.u16(o + 4)),
                               "compression": COMPRESSION.get(r.u16(o + 6), r.u16(o + 6))}
    if 0x000183FF in hdr:  # size-prefixed string
        o = hdr[0x000183FF]
        info["pe_name"] = r.cstr(o + 4, r.u32(o) - 4)
    if 0x00018002 in hdr:  # checksum, timestamp
        o = hdr[0x00018002]
        info["pe_checksum"], info["pe_timestamp"] = r.u32(o), r.u32(o + 4)
    if 0x00020104 in hdr:  # xex2_opt_tls_info
        o = hdr[0x00020104]
        info["tls"] = {"slot_count": r.u32(o), "raw_data_address": r.u32(o + 4),
                       "data_size": r.u32(o + 8), "raw_data_size": r.u32(o + 0xC)}

    s = info["security_offset"]  # xex2_security_info
    image_flags = r.u32(s + 0x10C)
    load = r.u32(s + 0x110)
    page = 0x1000 if image_flags & PAGE_SIZE_4KB_FLAG else 0x10000
    sec = {"image_size": r.u32(s + 4), "image_flags": image_flags, "load_address": load,
           "region": r.u32(s + 0x178), "allowed_media_types": r.u32(s + 0x17C),
           "page_size": page, "page_descriptor_count": r.u32(s + 0x180), "page_runs": []}
    addr, runs = load, sec["page_runs"]
    for i in range(sec["page_descriptor_count"]):
        word = r.u32(s + 0x184 + i * 24)  # xex2_page_descriptor: info:4 (low), page_count:28
        size, kind = (word >> 4) * page, word & 0xF
        if runs and runs[-1]["kind_id"] == kind and runs[-1]["end"] == addr:
            runs[-1]["end"] += size
        else:
            runs.append({"start": addr, "end": addr + size, "kind_id": kind,
                         "kind": SECTION_KINDS.get(kind, "unknown(%d)" % kind)})
        addr += size
    sec["page_total"] = addr - load
    info["security"] = sec

    if 0x000200FF in hdr:  # size + xex2_opt_static_library[]
        o = hdr[0x000200FF]
        libs = []
        for i in range((r.u32(o) - 4) // 16):
            e = o + 4 + i * 16
            libs.append({"name": r.cstr(e, 8), "major": r.u16(e + 8), "minor": r.u16(e + 0xA),
                         "build": r.u16(e + 0xC), "approval_type": r.u8(e + 0xE),
                         "qfe": r.u8(e + 0xF)})
        info["static_libraries"] = libs

    if 0x000103FF in hdr:  # xex2_opt_import_libraries + xex2_import_library[]
        o = hdr[0x000103FF]
        str_size, nlibs = r.u32(o + 4), r.u32(o + 8)
        names, p, end = [], o + 0xC, o + 0xC + str_size
        while len(names) < nlibs:
            if p >= end:
                raise ValueError("import string table holds fewer than %d names" % nlibs)
            n = r.bytes(p, end - p).index(b"\0")
            names.append(r.bytes(p, n).decode("ascii"))
            p += n + 1
            while p < end and r.u8(p) == 0:
                p += 1
        p, libs = end, []
        for _ in range(nlibs):
            size, name_index, nrec = r.u32(p), r.u16(p + 0x24), r.u16(p + 0x26)
            if name_index >= len(names):
                raise ValueError("import library name index %d out of range" % name_index)
            libs.append({"name": names[name_index], "version_raw": r.u32(p + 0x1C),
                         "version": version(r.u32(p + 0x1C)),
                         "version_min": version(r.u32(p + 0x20)), "records": nrec})
            if size == 0:
                raise ValueError("import library record of size 0")
            p += size
        info["import_libraries"] = libs
        info["import_records_total"] = sum(l["records"] for l in libs)
    return info


def report(info):
    out = []
    w = out.append
    w("module flags %08X  security info at %X  optional headers %d"
      % (info["module_flags"], info["security_offset"], len(info["optional_headers"])))
    for h in info["optional_headers"]:
        w("  %08X %-22s %08X%s" % (h["key"], h["name"], h["value"], "" if h["inline"] else " (offset)"))
    e = info.get("execution")
    if e:
        tid = e["title_id"]
        w("title id     %08X (%s-%d)" % (tid, chr(tid >> 24) + chr((tid >> 16) & 0xFF), tid & 0xFFFF))
        w("media id     %08X  version %s (raw %08X)  base version %s  disc %d/%d"
          % (e["media_id"], e["version"], e["version_raw"], e["base_version"],
             e["disc_number"], e["disc_count"]))
    if "file_format" in info:
        w("file format  encryption %(encryption)s  compression %(compression)s" % info["file_format"])
    if "pe_name" in info:
        w("PE name      %s" % info["pe_name"])
    for f in ("entry_point", "image_base", "original_base", "default_stack_size",
              "default_heap_size", "system_flags"):
        if f in info:
            w("%-12s %08X" % (f.replace("default_", "").replace("_", " "), info[f]))
    if "tls" in info:
        w("TLS          slots %(slot_count)d  raw data %(raw_data_address)08X  "
          "data size %(data_size)X  raw size %(raw_data_size)X" % info["tls"])
    s = info["security"]
    w("load address %08X  image size %08X  end %08X"
      % (s["load_address"], s["image_size"], s["load_address"] + s["image_size"]))
    w("image flags  %08X  region %08X  allowed media %08X  page size %X"
      % (s["image_flags"], s["region"], s["allowed_media_types"], s["page_size"]))
    w("page runs (%d descriptors, %08X bytes):" % (s["page_descriptor_count"], s["page_total"]))
    for run in s["page_runs"]:
        w("  %08X-%08X %-10s %7.2f MB" % (run["start"], run["end"], run["kind"],
                                          (run["end"] - run["start"]) / 2**20))
    for lib in info.get("static_libraries", []):
        if lib is info["static_libraries"][0]:
            w("static libraries (%d):" % len(info["static_libraries"]))
        w("  %-8s %d.%d.%d.%d" % (lib["name"], lib["major"], lib["minor"], lib["build"], lib["qfe"]))
    if "import_libraries" in info:
        w("import libraries (%d):" % len(info["import_libraries"]))
        for lib in info["import_libraries"]:
            w("  %-14s version %-14s min %-14s %4d import records"
              % (lib["name"], lib["version"], lib["version_min"], lib["records"]))
        w("  total import records: %d (function and data records; functions are split from data"
          " only after decryption, by tools/survey/xexsurvey)" % info["import_records_total"])
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != "--json"):
        print("usage: xexinfo.py <file.xex> [--json <out.json>]", file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        info = parse(f.read())
    sys.stdout.write(report(info))
    if len(argv) == 4:
        with open(argv[3], "w") as f:
            json.dump(info, f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
