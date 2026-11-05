// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Taken from ForzaRecomp tests/loader/xex_fixture.h (ForzaRecomp, GPL-3.0-or-later), reviewed and
// re-run in FM4 Recomp: fixture layout checked against Xenia canary 67f16c4e315f
// xex2_info.h and the PE32 header layout; expected values follow by hand from the fixture bytes.
// FM4 Recomp changes: this attribution block only.
//
// Synthetic, unencrypted XEX with one BASIC block and one PE section, for the loader tests.
// No game bytes.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

inline void be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = uint8_t(value >> (24 - 8 * i));
}
inline void le32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = uint8_t(value >> (8 * i));
}
inline void le16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = uint8_t(value);
    bytes.at(offset + 1) = uint8_t(value >> 8);
}

// Offsets of the BASIC block's size fields, for tests that rewrite them.
constexpr size_t kBlockDataSize = 0x68, kBlockZeroSize = 0x6C;

// One BASIC block of 0x1000 data bytes; `declared` is the security header's image size. One
// section at RVA 0x400, named `name` (at most 8 characters, the PE field size), with
// `virtualSize` and `characteristics` (default: 4 bytes of code).
inline std::vector<uint8_t> fixture(uint32_t declared, const char* name = ".text", uint32_t virtualSize = 4,
                                    uint32_t characteristics = 0x60000020) {
    std::vector<uint8_t> bytes(0x1400);
    be32(bytes, 0, 0x58455832);  // "XEX2"
    be32(bytes, 4, 1);
    be32(bytes, 8, 0x400);       // header size
    be32(bytes, 16, 0x80);       // security info offset
    be32(bytes, 20, 3);          // optional headers
    be32(bytes, 24, 0x3FF); be32(bytes, 28, 0x60);         // file format info
    be32(bytes, 32, 0x10100); be32(bytes, 36, 0x82000400);  // entry point
    be32(bytes, 40, 0x10201); be32(bytes, 44, 0x82000000);  // image base
    be32(bytes, 0x60, 16); bytes[0x67] = 1;                 // no encryption, BASIC compression
    be32(bytes, kBlockDataSize, 0x1000); be32(bytes, kBlockZeroSize, 0);  // 0x1000 data, 0 zero bytes
    be32(bytes, 0x84, declared); be32(bytes, 0x190, 0x82000000);
    const size_t pe = 0x400;
    bytes[pe] = 'M'; bytes[pe + 1] = 'Z'; le32(bytes, pe + 60, 0x80);
    le32(bytes, pe + 0x80, 0x4550); le16(bytes, pe + 0x84, 0x1F2); le16(bytes, pe + 0x86, 1);
    le16(bytes, pe + 0x94, 224); le16(bytes, pe + 0x98, 0x10B);
    le32(bytes, pe + 0xA8, 0x400); le32(bytes, pe + 0xB4, 0x82000000);
    le32(bytes, pe + 0xD0, declared);
    const size_t section = pe + 0x178;
    std::memcpy(bytes.data() + section, name, std::strlen(name));
    le32(bytes, section + 8, virtualSize); le32(bytes, section + 12, 0x400);
    le32(bytes, section + 16, 4); le32(bytes, section + 36, characteristics);
    return bytes;
}

