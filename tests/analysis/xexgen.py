# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Synthetic, unencrypted XEX2 images for the function-analysis tests (tests/test_analysis.py). No
# game bytes: every case lays out its own instruction words.
#
# Layout (the same as tests/loader/xex_fixture.h, extended to several sections):
#   XEX2 header: Xenia canary 67f16c4e315f src/xenia/kernel/util/xex2_info.h (xex2_header,
#     xex2_opt_file_format_info with one BASIC block, xex2_security_info image_size at +0x4 and
#     load_address at +0x110); optional headers 0x3FF (file format), 0x10100 (entry, inline),
#     0x10201 (image base, inline).
#   PE image: PE32 headers (e_lfanew at 0x3C, IMAGE_NT_HEADERS32 with Machine 0x1F2 POWERPCBE,
#     Characteristics 0x102, SizeOfOptionalHeader 224, Magic 0x10B, Subsystem 14 XBOX: the checks
#     of Xenia's ReadPEHeaders), 40-byte section headers.
#   .pdata entries: 8 bytes, BeginAddress (big-endian) then a word with PrologLength (bits 0-7),
#     FunctionLength in instructions (bits 8-29), ThirtyTwoBit (30), ExceptionFlag (31): Microsoft
#     PE/COFF specification, ".pdata" for PowerPC/Windows CE; XenonUtils xbox.h
#     IMAGE_CE_RUNTIME_FUNCTION; Xenia reads BeginAddress the same way (xex_module.cc:1559).
import struct

BASE = 0x82000000
CODE = 0x60000020   # IMAGE_SCN_CNT_CODE | MEM_EXECUTE | MEM_READ
DATA = 0x40000040   # IMAGE_SCN_CNT_INITIALIZED_DATA | MEM_READ

# PowerPC encodings used by the cases (PowerPC ISA; hand-checked)
BLR = 0x4E800020
NOP = 0x60000000


def li(rt, value):          # addi rT, 0, SIMM
    return 0x38000000 | (rt << 21) | (value & 0xFFFF)


def bl(at, target):         # b with LK=1, AA=0: LI = target - at
    return 0x48000001 | ((target - at) & 0x03FFFFFC)


def b(at, target):          # b with LK=0, AA=0
    return 0x48000000 | ((target - at) & 0x03FFFFFC)


def ld(rt, d, ra):           # DS-form, opcode 58, XO 0
    return 0xE8000000 | (rt << 21) | (ra << 16) | (d & 0xFFFC)


def lwz(rt, d, ra):          # D-form, opcode 32
    return 0x80000000 | (rt << 21) | (ra << 16) | (d & 0xFFFF)


MTLR_R12 = 0x7D8803A6       # mtspr LR (SPR 8), r12


def restgprlr_14():
    """The MSVC register-restore helper: ld r14..r31 from -0x98(r1) up in steps of 8, then
    lwz r12,-8(r1); mtlr r12; blr. 21 words; entry restgprlr_i is at (i-14)*4."""
    return [ld(r, -0x98 + 8 * (r - 14), 1) for r in range(14, 32)] + [lwz(12, -8, 1), MTLR_R12, BLR]


def beq(at, target):       # bc 12, 2 (cr0.eq), BD = target - at
    return 0x41820000 | ((target - at) & 0xFFFC)


def bne(at, target):       # bc 4, 2 (cr0.eq clear), BD = target - at
    return 0x40820000 | ((target - at) & 0xFFFC)


def cmpwi(ra, value):      # cmpi cr0, L=0, rA, SIMM
    return 0x2C000000 | (ra << 16) | (value & 0xFFFF)


def words(*ws):
    return b"".join(struct.pack(">I", w) for w in ws)


def pdata(*entries):
    """entries: (address, length in instructions[, prolog length[, exception flag]]) -> .pdata
    bytes. ExceptionFlag is bit 31 of the second word."""
    out = b""
    for e in entries:
        addr, length = e[0], e[1]
        prolog = e[2] if len(e) > 2 else 0
        flag = e[3] if len(e) > 3 else 0
        out += struct.pack(">II", addr, (prolog & 0xFF) | ((length & 0x3FFFFF) << 8) | ((flag & 1) << 31))
    return out


def export_table(offsets, base=BASE, magic=(0x48000000, 0x00485645, 0x48000000), count=None):
    """An XEX export table (Xenia xex2_info.h xex2_export_table): magic[3], modulenumber[2],
    version[3], imagebaseaddr (base >> 16), count, base ordinal, then one offset per export (function =
    offset + (imagebaseaddr << 16), xex_module.cc GetProcAddress). The magic words are FM4's own
    (ForzaRecomp patch 0018); Xenia does not check them."""
    n = len(offsets) if count is None else count
    return words(*magic, 0, 0, 0, 0, 0, base >> 16, n, 1, *offsets)


def build(sections, entry, image_size=0x4000, base=BASE, export_table_address=0):
    """sections: list of (name, rva, bytes, characteristics, virtual_size or None)."""
    image = bytearray(image_size)
    le16 = lambda o, v: struct.pack_into("<H", image, o, v)
    le32 = lambda o, v: struct.pack_into("<I", image, o, v)
    image[0:2] = b"MZ"
    le32(0x3C, 0x80)
    pe = 0x80
    image[pe:pe + 4] = b"PE\0\0"
    le16(pe + 4, 0x1F2)                 # Machine POWERPCBE
    le16(pe + 6, len(sections))         # NumberOfSections
    le16(pe + 20, 224)                  # SizeOfOptionalHeader
    le16(pe + 22, 0x102)                # EXECUTABLE_IMAGE | 32BIT_MACHINE
    opt = pe + 24
    le16(opt, 0x10B)                    # PE32 magic
    le32(opt + 16, entry - base)        # AddressOfEntryPoint
    le32(opt + 28, base)                # ImageBase
    le32(opt + 56, image_size)          # SizeOfImage
    le16(opt + 68, 14)                  # Subsystem XBOX
    sh = opt + 224
    for i, sec in enumerate(sections):
        name, rva, data, chars = sec[0], sec[1], sec[2], sec[3]
        vsize = sec[4] if len(sec) > 4 and sec[4] is not None else len(data)
        s = sh + 40 * i
        image[s:s + 8] = name.encode().ljust(8, b"\0")[:8]
        le32(s + 8, vsize)
        le32(s + 12, rva)
        le32(s + 16, len(data))
        le32(s + 36, chars)
        image[rva:rva + len(data)] = data

    header_size = 0x400
    xex = bytearray(header_size)
    be32 = lambda o, v: struct.pack_into(">I", xex, o, v)
    be16 = lambda o, v: struct.pack_into(">H", xex, o, v)
    xex[0:4] = b"XEX2"
    be32(4, 1)                          # module flags: title
    be32(8, header_size)
    be32(16, 0x80)                      # security info offset
    be32(20, 3)                         # optional headers
    be32(24, 0x3FF); be32(28, 0x60)     # file format info at 0x60
    be32(32, 0x10100); be32(36, entry)  # entry point (inline)
    be32(40, 0x10201); be32(44, base)   # image base (inline)
    be32(0x60, 16); be16(0x64, 0); be16(0x66, 1)   # info size, no encryption, BASIC
    be32(0x68, image_size); be32(0x6C, 0)          # one block: all data, no zero fill
    be32(0x84, image_size)              # security: image size
    be32(0x80 + 0x110, base)            # security: load address
    be32(0x80 + 0x160, export_table_address)  # security: export table (xex2_security_info +0x160)
    return bytes(xex) + bytes(image)
