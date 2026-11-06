# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Inventory the Xenos shaders inside an FM4 .fxobj (a compiled D3DX9 effect, "D3DX9.1 Effect
# Compiler"): locate every 0x102A11xx shader container, check its layout, extract the
# microcode, classify the stage and hash the microcode. Reads the file only; never writes it.
#
#   /opt/homebrew/bin/python3 -I survey/fxobj.py <file.fxobj> [--json <out.json>]
#
# Exit status 1 when the file has any error (fail closed): a container whose layout does not
# add up is reported, never skipped.
#
# Sources (cited per definition below):
#   RC  = ReCarbon: runtime/gpu/xenos/ucode.h (ported from Xenia src/xenia/gpu/ucode.h,
#         BSD), runtime/gpu/xenos/xenos.h, runtime/gpu/shader_translator.cpp,
#         runtime/gpu/renderer.cpp, runtime/gpu/command_processor.cpp
#   XC  = Xenia canary 67f16c4 (src/xenia/gpu/xenos.h)
#   D3D = Direct3D 9 SDK headers: d3d9types.h (D3DVS_VERSION / D3DPS_VERSION),
#         d3dx9shader.h (D3DXSHADER_CONSTANTTABLE, D3DXSHADER_CONSTANTINFO)
# Written for FM4 Recomp from those definitions and from bytes of the USA disc's
# Media/Shaders files read with xxd; no code is copied.
#
# Labels: VERIFIED = checked against a cited definition and the disc's bytes;
#         OBSERVED = seen in the disc's bytes only (a handful of containers); HYPOTHESIS = a reading not yet confirmed.
import json
import struct
import sys

FILE_MAGIC = 0x00000101          # OBSERVED: word 0 of all 171 files
OUTER_HEADER = 28                # OBSERVED: BE word at +0x18 == file size - 28 in all 171 files
CONTAINER_MAGIC_HI = 0x102A11    # OBSERVED: top 24 bits of the container magic
# OBSERVED (VERIFIED against D3D version tokens on 4 containers): low byte 0x01 = vertex,
# 0x00 = pixel. Note this is the opposite of Xenos ShaderType (vertex 0, pixel 1).
CONTAINER_STAGE = {0x01: "vs", 0x00: "ps"}
# Xenos ShaderType, used as the hash seed: XC xenos.h:30-33, RC xenos.h:35-38 (kVertex 0, kPixel 1).
SHADER_TYPE = {"vs": 0, "ps": 1}
# D3D d3d9types.h: D3DVS_VERSION = 0xFFFE0000 | ver, D3DPS_VERSION = 0xFFFF0000 | ver.
VERSION_STAGE = {0xFFFE: "vs", 0xFFFF: "ps"}

# Container header, offsets from the magic (OBSERVED on particle_sparks, Sleeves and
# shaders_v16_debug containers; field meanings HYPOTHESIS except where checked):
H_BLOB_OFFSET = 0x04   # offset of the blob = literal block + microcode
H_BLOB_SIZE = 0x08     # == info[0] + info[1]; also the size word before the magic == blob offset + blob size
H_ZERO_0C = 0x0C       # always 0 so far
H_CTAB_OFFSET = 0x10   # always 0x24: offset of the constant table's byte-size word
H_EXTRA_OFFSET = 0x14  # 0, or the offset of a 0x28-byte block right after the constant table (meaning unknown)
H_INFO_OFFSET = 0x18   # offset of the info block (info[0] literal bytes, info[1] microcode bytes, rest unknown)
H_ZERO_1C = 0x1C
H_ZERO_20 = 0x20
CTAB_SIZE_WORD = 0x24
CTAB_START = 0x28
CONTAINER_HEADER_WORDS = 9

# D3D d3dx9shader.h: D3DXSHADER_CONSTANTTABLE is 7 DWORDs (Size, Creator, Version, Constants,
# ConstantInfo, Flags, Target); offsets are relative to the table. Big-endian here.
CTAB_HEADER = 0x1C
CONSTANTINFO = 20    # Name DWORD, RegisterSet/Index/Count/Reserved WORD, TypeInfo DWORD, DefaultValue DWORD
REGISTER_SETS = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}   # D3DXREGISTER_SET

# Control flow, RC ucode.h:94-166 (ControlFlowOpcode) and 225-531 (bitfields, LSB first;
# UnpackControlFlowInstructions). Words are big-endian guest values, decoded to integers.
CF_EXEC = {1, 2, 3, 4, 5, 6, 13, 14}        # IsControlFlowOpcodeExec, RC ucode.h:169-178
CF_END = {2, 4, 6, 14}                      # DoesControlFlowOpcodeEndShader, RC ucode.h:182-187
CF_LOOP = {7, 8}
CF_CALL = {9, 10}
CF_JUMP = {11}
CF_ALLOC = 12
ALLOC_VS_POSITION = 1                       # AllocType, RC ucode.h:204-216
FETCH_VERTEX, FETCH_TEXTURE = 0, 1          # FetchOpcode, RC ucode.h:533-603 (bits 0-4 of word 0)
SEQUENCE_SLOTS = 6                          # 12 sequence bits, 2 per instruction (RC ucode.h:232-233)

# FNV-1a-64 constants (Fowler/Noll/Vo, published offset basis and prime).
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


class FormatError(ValueError):
    pass


def be32(data, off):
    if off < 0 or off + 4 > len(data):
        raise FormatError("read of 4 bytes at 0x%X outside 0x%X bytes" % (off, len(data)))
    return struct.unpack_from(">I", data, off)[0]


def fnv1a64_words(words, seed):
    """FNV-1a-64 with a whole 32-bit word per step: h = (h ^ w) * prime mod 2^64.
    For words below 256 this is exactly byte-wise FNV-1a-64 of those bytes."""
    h = seed
    for w in words:
        h = ((h ^ w) * FNV_PRIME) & MASK64
    return h


def shader_hash(stage, ucode):
    """The runtime's shader hash (RC renderer.cpp:7390-7399, command_processor.cpp:363-374):
    seed FNV offset ^ ShaderType (vs 0, ps 1), one step per microcode dword, each dword loaded
    little-endian from the guest's big-endian bytes (a host load of guest memory as stored)."""
    if len(ucode) % 4:
        raise FormatError("microcode length %d is not whole dwords" % len(ucode))
    words = [int.from_bytes(ucode[i:i + 4], "little") for i in range(0, len(ucode), 4)]
    return fnv1a64_words(words, FNV_OFFSET ^ SHADER_TYPE[stage])


def cstring(blob, off, what):
    if not 0 <= off < len(blob):
        raise FormatError("%s offset 0x%X outside the constant table (0x%X bytes)" % (what, off, len(blob)))
    end = blob.find(b"\0", off)
    if end < 0:
        raise FormatError("%s at 0x%X is not NUL-terminated inside the constant table" % (what, off))
    return blob[off:end].decode("ascii", errors="replace")


def parse_ctab(blob):
    """D3DXSHADER_CONSTANTTABLE (D3D d3dx9shader.h), big-endian."""
    if len(blob) < CTAB_HEADER:
        raise FormatError("constant table of 0x%X bytes is shorter than its header" % len(blob))
    size, creator, version, count, info, flags, target = struct.unpack_from(">7I", blob, 0)
    if size != CTAB_HEADER:
        raise FormatError("constant table header size 0x%X, expected 0x1C" % size)
    stage = VERSION_STAGE.get(version >> 16)
    if stage is None:
        raise FormatError("shader version token 0x%08X is neither vs (0xFFFE) nor ps (0xFFFF)" % version)
    target_name = cstring(blob, target, "target")
    if not target_name.startswith(stage + "_"):
        raise FormatError("target %r disagrees with version token 0x%08X" % (target_name, version))
    if info + count * CONSTANTINFO > len(blob):
        raise FormatError("%d constant records at 0x%X overrun the constant table" % (count, info))
    constants = []
    for n in range(count):
        name, reg_set, index, regs, _, _, _ = struct.unpack_from(">IHHHHII", blob, info + n * CONSTANTINFO)
        if reg_set not in REGISTER_SETS:
            raise FormatError("constant %d has unknown register set %d" % (n, reg_set))
        constants.append({"name": cstring(blob, name, "constant name"), "set": REGISTER_SETS[reg_set],
                          "index": index, "count": regs})
    return {"version": version, "stage": stage, "target": target_name,
            "creator": cstring(blob, creator, "creator"), "flags": flags, "constants": constants}


def unpack_cf(w0, w1, w2):
    """UnpackControlFlowInstructions, RC ucode.h:522-531: two 48-bit instructions per 3 dwords,
    each as (dword_0, dword_1 low 16 bits)."""
    return ((w0, w1 & 0xFFFF), (((w1 >> 16) | (w2 << 16)) & 0xFFFFFFFF, w2 >> 16))


def analyze_microcode(ucode, stage):
    """Walk the control-flow program and the instruction slots it executes. Errors are raised."""
    words = struct.unpack(">%dI" % (len(ucode) // 4), ucode)
    slots = len(words) // 3
    # Control flow ends at the first exec's instruction address (RC shader_translator.cpp:785-795).
    bound, i = slots, 0
    while i < bound:
        for d0, d1 in unpack_cf(*words[3 * i:3 * i + 3]):
            if (d1 >> 12) & 0xF in CF_EXEC:
                bound = min(bound, d0 & 0xFFF)
        i += 1
    if bound == 0:
        raise FormatError("no control-flow instruction before the first exec's address")
    cf = [c for t in range(bound) for c in unpack_cf(*words[3 * t:3 * t + 3])]
    kinds, allocs, ops, ends = {}, [], set(), 0
    for d0, d1 in cf:
        op = (d1 >> 12) & 0xF
        ops.add(op)
        ends += op in CF_END
        if op == CF_ALLOC:
            alloc_type, size = (d1 >> 9) & 3, d0 & 7
            if alloc_type == ALLOC_VS_POSITION and stage != "vs":
                raise FormatError("position alloc in a %s" % stage)
            allocs.append([alloc_type, size])
        if op not in CF_EXEC:
            continue
        address, count, sequence = d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF
        if count > SEQUENCE_SLOTS:
            raise FormatError("exec of %d instructions: sequence bits cover %d" % (count, SEQUENCE_SLOTS))
        if address + count > slots:
            raise FormatError("exec at slot %d of %d instructions runs past %d slots" % (address, count, slots))
        for k in range(count):
            slot = address + k
            if (sequence >> (2 * k)) & 1:
                fetch = words[3 * slot] & 0x1F
                kind = {FETCH_VERTEX: "vfetch", FETCH_TEXTURE: "tfetch"}.get(fetch, "fetch_other")
            else:
                kind = "alu"
            if kinds.setdefault(slot, kind) != kind:
                raise FormatError("slot %d executed as both %s and %s" % (slot, kinds[slot], kind))
    if not ends:
        raise FormatError("no ending exec in %d control-flow instructions" % len(cf))
    counts = {k: sum(1 for v in kinds.values() if v == k) for k in ("alu", "vfetch", "tfetch", "fetch_other")}
    return dict(counts, cf_instructions=len(cf), slots=slots, unexecuted_slots=slots - bound - len(kinds),
                allocs=allocs, loops=bool(ops & CF_LOOP), calls=bool(ops & CF_CALL),
                jumps=bool(ops & CF_JUMP), conditional=bool(ops & (CF_EXEC - {1, 2})))


def parse_container(data, c):
    magic = be32(data, c)
    stage = CONTAINER_STAGE.get(magic & 0xFF)
    if magic >> 8 != CONTAINER_MAGIC_HI or stage is None:
        raise FormatError("unknown container magic 0x%08X" % magic)
    h = [be32(data, c + 4 * n) for n in range(CONTAINER_HEADER_WORDS)]
    blob_off, blob_size = h[H_BLOB_OFFSET // 4], h[H_BLOB_SIZE // 4]
    extra_off, info_off = h[H_EXTRA_OFFSET // 4], h[H_INFO_OFFSET // 4]
    for off in (H_ZERO_0C, H_ZERO_1C, H_ZERO_20):
        if h[off // 4]:
            raise FormatError("header word +0x%X is 0x%X, expected 0" % (off, h[off // 4]))
    if h[H_CTAB_OFFSET // 4] != CTAB_SIZE_WORD:
        raise FormatError("constant table offset 0x%X, expected 0x24" % h[H_CTAB_OFFSET // 4])
    end = c + blob_off + blob_size
    if end > len(data):
        raise FormatError("container ends at 0x%X past the file (0x%X bytes)" % (end, len(data)))
    if c < OUTER_HEADER + 4 or be32(data, c - 4) != blob_off + blob_size:
        raise FormatError("size word before the magic is not blob offset + blob size (0x%X)" % (blob_off + blob_size))
    ctab_size = be32(data, c + CTAB_SIZE_WORD)
    after_ctab = CTAB_START + ctab_size
    if extra_off and (extra_off != after_ctab or extra_off >= info_off):
        raise FormatError("extra block at 0x%X, expected 0x%X before info 0x%X" % (extra_off, after_ctab, info_off))
    if not extra_off and info_off != after_ctab:
        raise FormatError("info block at 0x%X, expected 0x%X right after the constant table" % (info_off, after_ctab))
    if (ctab_size | info_off | blob_off) & 3 or info_off + 8 > blob_off:
        raise FormatError("misaligned or overlapping sections (table 0x%X, info 0x%X, blob 0x%X)"
                          % (ctab_size, info_off, blob_off))
    ctab = parse_ctab(data[c + CTAB_START:c + after_ctab])
    if ctab["stage"] != stage:
        raise FormatError("magic 0x%08X says %s, version token says %s" % (magic, stage, ctab["stage"]))
    info = [be32(data, c + off) for off in range(info_off, blob_off, 4)]
    literal_bytes, ucode_bytes = info[0], info[1]
    if literal_bytes + ucode_bytes != blob_size:
        raise FormatError("info literal 0x%X + microcode 0x%X != blob size 0x%X" % (literal_bytes, ucode_bytes, blob_size))
    if literal_bytes % 16:
        raise FormatError("literal block of 0x%X bytes is not whole vec4s" % literal_bytes)
    if ucode_bytes == 0 or ucode_bytes % 12:
        raise FormatError("microcode of 0x%X bytes is not whole 3-dword instructions" % ucode_bytes)
    ucode_off = c + blob_off + literal_bytes
    ucode = data[ucode_off:ucode_off + ucode_bytes]
    extra = [be32(data, c + off) for off in range(extra_off, info_off, 4)] if extra_off else []
    return {"offset": c, "end": end, "magic": magic, "stage": stage,
            "header": ["%08X" % w for w in h], "ctab": ctab, "extra": ["%08X" % w for w in extra],
            "info": ["%08X" % w for w in info], "literal_bytes": literal_bytes,
            "ucode_offset": ucode_off, "ucode_bytes": ucode_bytes, "ucode_dwords": ucode_bytes // 4,
            "hash": "%016x" % shader_hash(stage, ucode), "cf": analyze_microcode(ucode, stage)}


def parse_fxobj(data):
    """Returns {"size", "outer", "containers", "errors"}; any error means the file is not accounted for."""
    out = {"size": len(data), "outer": None, "containers": [], "errors": []}
    errors = out["errors"]
    if len(data) < OUTER_HEADER:
        errors.append("file of %d bytes is shorter than the 28-byte header" % len(data))
        return out
    outer = struct.unpack_from(">7I", data, 0)
    out["outer"] = ["%08X" % w for w in outer]
    if outer[0] != FILE_MAGIC:
        errors.append("file magic 0x%08X, expected 0x00000101" % outer[0])
    if outer[6] != len(data) - OUTER_HEADER:
        errors.append("size field 0x%X != file size - 28 (0x%X)" % (outer[6], len(data) - OUTER_HEADER))
    needle = struct.pack(">I", CONTAINER_MAGIC_HI << 8)[:3]
    candidates, at = [], data.find(needle)
    while at >= 0:
        if at % 4:
            errors.append("container magic bytes at unaligned offset 0x%X" % at)
        else:
            candidates.append(at)
        at = data.find(needle, at + 1)
    for c in candidates:
        try:
            out["containers"].append(parse_container(data, c))
        except FormatError as e:
            errors.append("container at 0x%X: %s" % (c, e))
    previous_end = 0
    for k in out["containers"]:
        if k["offset"] - 4 < previous_end:
            errors.append("container at 0x%X overlaps the previous one (ends 0x%X)" % (k["offset"], previous_end))
        previous_end = k["end"]
    if not candidates:
        errors.append("no shader container")
    return out


def inspect(path):
    with open(path, "rb") as f:
        return parse_fxobj(f.read())


def report(info):
    out = ["size %d  outer %s  containers %d  errors %d"
           % (info["size"], " ".join(info["outer"] or []), len(info["containers"]), len(info["errors"]))]
    for k in info["containers"]:
        cf = k["cf"]
        out.append("  0x%06X %s %-8s %s  ucode %5d B  literals %4d B  alu %3d vfetch %2d tfetch %2d%s%s%s  %s"
                   % (k["offset"], k["stage"], k["ctab"]["target"], k["hash"], k["ucode_bytes"],
                      k["literal_bytes"], cf["alu"], cf["vfetch"], cf["tfetch"],
                      "  loop" if cf["loops"] else "", "  call" if cf["calls"] else "",
                      "  jump" if cf["jumps"] else "", k["ctab"]["creator"]))
    out += ["  ERROR " + e for e in info["errors"]]
    return "\n".join(out) + "\n"


def main(argv):
    args = argv[1:]
    if len(args) not in (1, 3) or (len(args) == 3 and args[1] != "--json"):
        print("usage: fxobj.py <file.fxobj> [--json <out.json>]", file=sys.stderr)
        return 2
    info = inspect(args[0])
    sys.stdout.write(report(info))
    if len(args) == 3:
        with open(args[2], "w") as f:
            json.dump(info, f, indent=1, sort_keys=True)
            f.write("\n")
    return 1 if info["errors"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
