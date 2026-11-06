// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Xenos texture storage -> tightly packed host rows, for every block size FM4's first screens use
// (1, 2, 4, 8 and 16 bytes per block: k_8, 16-bit formats, k_8_8_8_8, DXT1, DXT3/5), tiled or
// linear, with the fetch constant's endian swap. M4 unit 7, CPU side; the GPU untile (a compute
// pass, as Xenia does) is a later measurement, never a correctness change.
//
// Formulas (Xenia xenia-canary 67f16c4e315f, BSD-3-Clause, transcribed with their line numbers):
//  - TiledOffset2D: src/xenia/gpu/texture_util.cc:443-458 GetTiledOffset2D, token for token,
//    including `pitch = align(pitch, 32)` (:446; kTextureTileWidthHeight, xenos.h:1175-1176). It is
//    the function Xenia's load shaders use (shaders/texture_address.xesli XeTextureTiledOffset2D).
//    x, y and the pitch are in blocks (a DXT1 block is 4x4 texels and 8 bytes, log2 3; a k_8 texel is
//    a 1-byte block, log2 0). A review FALSIFIED the unaligned-pitch form
//    front_buffer.h still has (fix pending there); this header does not share its code.
//  - TiledAddressUpperBound2D: texture_util.cc:490-517: the 32x32-block tile holding the last block,
//    plus 0xA00 (1-byte blocks), 0xC00 (2-byte) or 0x400 << log2 (4 bytes and more): below 4 bytes,
//    tiles interleave inside 128x128 or 64x64 portions, so the extent exceeds pitch * rows.
//  - The base level's pitch in blocks: align(align(pitch texels, block width) / block width, 32)
//    (texture_util.cc:315-320); linear rows of the *base* are not re-aligned to 256 bytes (:321-327:
//    Direct3D 9 already wrote an aligned pitch); the linear extent is rows - 1 pitches plus the last
//    row's blocks (:420-423).
//  - Endian (xenos.h:1070-1088 GpuSwapInline; shaders/endian.xesli:25-46): blocks of 4 bytes and
//    more get XeEndianSwap32 on each dword (texture_load_32bpb/64bpb.xesli:41-43: 8in16 swaps the
//    bytes of each 16-bit half, 8in32 reverses the four bytes, 16in32 swaps the halves); 2-byte blocks
//    get XeEndianSwap16 (texture_load_16bpb.xesli:41-43; endian.xesli:25-31: only 8in16 does
//    anything, 8in32 and 16in32 are no-ops there, which this refuses rather than silently ignores);
//    1-byte blocks are copied with no swap at all (texture_load_8bpb.xesli:40-50), so only endian 0
//    is accepted for them (what the hardware does with another value is UNKNOWN).
// TiledPitchBlocks, TiledExtent, DetileBlocksAt, LinearExtent and CopyLinearBlocks follow
// ForzaRecomp runtime/gpu/texture.h:53-175 (GPL-3.0-or-later, same owner), re-derived
// against the lines above. FM4 Recomp changes: namespace runtime::video; the tiled offset takes the
// unaligned pitch as Xenia does; the endian swap is a byte permutation (host-order independent) and
// 2-byte blocks are supported; the copies take an origin block for packed mips (as DetileBlocksAt)
// for linear storage too; nothing stops: the caller reports.
#pragma once
#include <cstddef>
#include <cstdint>

namespace runtime::video {

constexpr uint32_t kTileWidthHeight = 32;  // xenos.h:1175-1176 kTextureTileWidthHeight

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// texture_util.cc:443-458 GetTiledOffset2D (x, y, pitch in blocks; 2^log2Bytes bytes per block).
constexpr uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t log2Bytes)
{
    const uint32_t pitch = AlignUp(pitchBlocks, kTileWidthHeight);                        // :446
    const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2Bytes + 7);        // :448-449
    const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2Bytes;                      // :451
    const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);  // :453-454
    return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +           // :456-457
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// texture_util.cc:490-517 GetTiledAddressUpperBound2D: bytes from the storage's origin that blocks
// [0, right) x [0, bottom) can touch (0 for an empty region).
constexpr uint32_t TiledAddressUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitchBlocks, uint32_t log2Bytes)
{
    if (right == 0 || bottom == 0)
        return 0;
    const uint32_t origin = TiledOffset2D((right - 1) & ~(kTileWidthHeight - 1), (bottom - 1) & ~(kTileWidthHeight - 1), pitchBlocks, log2Bytes);
    return origin + (log2Bytes == 0 ? 0xA00u : log2Bytes == 1 ? 0xC00u : 0x400u << log2Bytes);
}

// The base level's row pitch in blocks (texture_util.cc:315-318), from the fetch constant's pitch in
// texels (already a multiple of 32 texels: dword 0 bits 22-30 << 5).
constexpr uint32_t TiledPitchBlocks(uint32_t pitchTexels, uint32_t blockWidth)
{
    return AlignUp(AlignUp(pitchTexels, blockWidth) / blockWidth, kTileWidthHeight);
}

// texture_util.cc:420-423 with one slice: the bytes a linear widthBlocks x heightBlocks region
// starting at block (0, 0) touches (0 for an empty region).
constexpr size_t LinearExtent(uint32_t widthBlocks, uint32_t heightBlocks, uint32_t rowPitchBytes, uint32_t log2Bytes)
{
    if (widthBlocks == 0 || heightBlocks == 0)
        return 0;
    return size_t(rowPitchBytes) * (heightBlocks - 1) + (size_t(widthBlocks) << log2Bytes);
}

// ---- Endian ---------------------------------------------------------------------------------------
constexpr uint32_t kEndianNone = 0, kEndian8in16 = 1, kEndian8in32 = 2, kEndian16in32 = 3;

// xenos.h:1070-1088 on a 32-bit value (endian.xesli:37-46 XeEndianSwap32).
constexpr uint32_t EndianSwap32(uint32_t v, uint32_t endian)
{
    if (endian == kEndian8in16 || endian == kEndian8in32)
        v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == kEndian8in32 || endian == kEndian16in32)
        v = (v << 16) | (v >> 16);
    return v;
}

// endian.xesli:25-31 XeEndianSwap16 on two packed 16-bit values: only 8in16 swaps.
constexpr uint32_t EndianSwap16Pair(uint32_t v, uint32_t endian)
{
    return endian == kEndian8in16 ? ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8) : v;
}

// The endians the copies accept per block size (header comment): 1-byte blocks only 0, 2-byte blocks
// 0 and 8in16, 4 bytes and more all four.
constexpr bool EndianSupported(uint32_t log2Bytes, uint32_t endian)
{
    return log2Bytes == 0 ? endian == kEndianNone : log2Bytes == 1 ? endian <= kEndian8in16 : log2Bytes <= 4 && endian <= kEndian16in32;
}

// The swap as a byte permutation of one block (little-endian host view of the dwords): for 4 bytes
// and more, per dword {1,0,3,2} 8in16, {3,2,1,0} 8in32, {2,3,0,1} 16in32; for 2-byte blocks {1,0}
// under 8in16. `block` holds 2^log2Bytes bytes; EndianSupported must hold.
void SwapBlockEndian(uint8_t* block, uint32_t log2Bytes, uint32_t endian);

// ---- Copies ---------------------------------------------------------------------------------------
// Tiled storage -> tightly packed rows of widthBlocks blocks, the region whose first block is
// (x0, y0) (a level inside a packed mip tail is at a non-zero origin). `tiledBytes` must cover
// TiledAddressUpperBound2D(x0 + widthBlocks, y0 + heightBlocks, pitchBlocks, log2Bytes). False
// (nothing copied) for an unsupported endian, a pitch that is not a multiple of 32, a region wider
// than the pitch, or a short source.
bool DetileBlocksAt(const uint8_t* tiled, size_t tiledBytes, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks,
                    uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t log2Bytes, uint32_t endian);

// Linear storage (block (x, y) at y * rowPitchBytes + (x << log2Bytes), texture_address.xesli
// XeTextureGuestLinearOffset) -> the same tightly packed rows. `sourceBytes` must cover
// rowPitchBytes * y0 + LinearExtent(x0 + widthBlocks, heightBlocks, rowPitchBytes, log2Bytes).
bool CopyLinearBlocksAt(const uint8_t* source, size_t sourceBytes, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks,
                        uint32_t heightBlocks, uint32_t rowPitchBytes, uint32_t log2Bytes, uint32_t endian);

}  // namespace runtime::video
