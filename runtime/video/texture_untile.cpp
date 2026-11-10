// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// The copies of texture_untile.h (see its header comment for sources and attribution).
#include "texture_untile.h"
#include "parallel_rows.h"
#include <cstring>
#include <utility>

namespace runtime::video {

void SwapBlockEndian(uint8_t* block, uint32_t log2Bytes, uint32_t endian)
{
    if (endian == kEndianNone)
        return;
    if (log2Bytes == 1)  // XeEndianSwap16: 8in16 only (EndianSupported refuses the rest)
    {
        const uint8_t b0 = block[0];
        block[0] = block[1], block[1] = b0;
        return;
    }
    // XeEndianSwap32 on each dword of the block, as the byte permutation it is on a little-endian view.
    static constexpr uint8_t kOrder[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1}};
    const uint32_t bytes = 1u << log2Bytes;
    for (uint32_t i = 0; i + 4 <= bytes; i += 4)
    {
        uint8_t b[4];
        std::memcpy(b, block + i, 4);
        for (uint32_t k = 0; k < 4; k++)
            block[i + k] = b[kOrder[endian & 3][k]];
    }
}

bool DetileBlocksAt(const uint8_t* tiled, size_t tiledBytes, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks,
                    uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t log2Bytes, uint32_t endian)
{
    if (!EndianSupported(log2Bytes, endian) || (pitchBlocks & (kTileWidthHeight - 1)) || x0 > pitchBlocks || widthBlocks > pitchBlocks - x0 ||
        TiledAddressUpperBound2D(x0 + widthBlocks, y0 + heightBlocks, pitchBlocks, log2Bytes) > tiledBytes)
        return false;
    const uint32_t bytes = 1u << log2Bytes;
    for (uint32_t y = 0; y < heightBlocks; y++)
        for (uint32_t x = 0; x < widthBlocks; x++)
        {
            uint8_t* block = out + ((size_t(y) * widthBlocks + x) << log2Bytes);
            std::memcpy(block, tiled + TiledOffset2D(x0 + x, y0 + y, pitchBlocks, log2Bytes), bytes);
            SwapBlockEndian(block, log2Bytes, endian);
        }
    return true;
}

namespace {
// One dword of a block under `endian` as SwapBlockEndian permutes it ({1,0,3,2} 8in16, {3,2,1,0} 8in32, {2,3,0,1} 16in32).
inline uint32_t SwapDword(uint32_t v, uint32_t endian)
{
    switch (endian)
    {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}
template <uint32_t Bytes>
void DetileRowsFast(const uint8_t* tiled, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks, uint32_t pitchBlocks, uint32_t log2Bytes,
                    uint32_t endian, uint32_t firstRow, uint32_t lastRow)
{
    for (uint32_t y = firstRow; y < lastRow; y++)
        for (uint32_t x = 0; x < widthBlocks; x++)
        {
            uint8_t* block = out + (size_t(y) * widthBlocks + x) * Bytes;
            std::memcpy(block, tiled + TiledOffset2D(x0 + x, y0 + y, pitchBlocks, log2Bytes), Bytes);
            if constexpr (Bytes == 2)
            {
                if (endian != kEndianNone)
                    std::swap(block[0], block[1]);
            }
            else if constexpr (Bytes >= 4)
            {
                if (endian != kEndianNone)
                    for (uint32_t i = 0; i < Bytes; i += 4)
                    {
                        uint32_t v;
                        std::memcpy(&v, block + i, 4);
                        v = SwapDword(v, endian);
                        std::memcpy(block + i, &v, 4);
                    }
            }
        }
}
}  // namespace

bool DetileBlocksAtFast(const uint8_t* tiled, size_t tiledBytes, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks,
                        uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t log2Bytes, uint32_t endian)
{
    if (!EndianSupported(log2Bytes, endian) || (pitchBlocks & (kTileWidthHeight - 1)) || x0 > pitchBlocks || widthBlocks > pitchBlocks - x0 ||
        TiledAddressUpperBound2D(x0 + widthBlocks, y0 + heightBlocks, pitchBlocks, log2Bytes) > tiledBytes)
        return false;
    const auto rows = [&](uint32_t first, uint32_t last) {
        switch (log2Bytes)
        {
        case 0: DetileRowsFast<1>(tiled, out, x0, y0, widthBlocks, pitchBlocks, 0, endian, first, last); break;
        case 1: DetileRowsFast<2>(tiled, out, x0, y0, widthBlocks, pitchBlocks, 1, endian, first, last); break;
        case 2: DetileRowsFast<4>(tiled, out, x0, y0, widthBlocks, pitchBlocks, 2, endian, first, last); break;
        case 3: DetileRowsFast<8>(tiled, out, x0, y0, widthBlocks, pitchBlocks, 3, endian, first, last); break;
        default: DetileRowsFast<16>(tiled, out, x0, y0, widthBlocks, pitchBlocks, 4, endian, first, last); break;
        }
    };
    ParallelRows(heightBlocks, widthBlocks, rows);
    return true;
}

bool CopyLinearBlocksAt(const uint8_t* source, size_t sourceBytes, uint8_t* out, uint32_t x0, uint32_t y0, uint32_t widthBlocks,
                        uint32_t heightBlocks, uint32_t rowPitchBytes, uint32_t log2Bytes, uint32_t endian)
{
    const size_t rowBytes = size_t(widthBlocks) << log2Bytes;
    const size_t origin = size_t(rowPitchBytes) * y0 + (size_t(x0) << log2Bytes);
    if (!EndianSupported(log2Bytes, endian) || (rowPitchBytes & ((1u << log2Bytes) - 1)) ||
        (size_t(x0 + widthBlocks) << log2Bytes) > rowPitchBytes ||
        origin + LinearExtent(widthBlocks, heightBlocks, rowPitchBytes, log2Bytes) > sourceBytes)
        return false;
    for (uint32_t y = 0; y < heightBlocks; y++)
    {
        uint8_t* row = out + size_t(y) * rowBytes;
        std::memcpy(row, source + origin + size_t(y) * rowPitchBytes, rowBytes);
        if (endian != kEndianNone)
            for (size_t i = 0; i < rowBytes; i += size_t(1) << log2Bytes)
                SwapBlockEndian(row + i, log2Bytes, endian);
    }
    return true;
}

bool DetileBlocks3D(const uint8_t* tiled, size_t tiledBytes, uint8_t* out, uint32_t widthBlocks, uint32_t heightBlocks, uint32_t depth,
                    uint32_t pitchBlocks, uint32_t zStrideBlockRows, uint32_t log2Bytes, uint32_t endian)
{
    if (!EndianSupported(log2Bytes, endian) || (pitchBlocks & (kTileWidthHeight - 1)) || widthBlocks > pitchBlocks || heightBlocks > zStrideBlockRows ||
        TiledAddressUpperBound3D(widthBlocks, heightBlocks, depth, pitchBlocks, heightBlocks, log2Bytes) > tiledBytes)
        return false;
    const uint32_t bytes = 1u << log2Bytes;
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < heightBlocks; y++)
            for (uint32_t x = 0; x < widthBlocks; x++)
            {
                const size_t offset = TiledOffset3D(x, y, z, pitchBlocks, zStrideBlockRows, log2Bytes);
                if (offset + bytes > tiledBytes)  // never for a covered region; kept as the bound of every read
                    return false;
                uint8_t* block = out + (((size_t(z) * heightBlocks + y) * widthBlocks + x) << log2Bytes);
                std::memcpy(block, tiled + offset, bytes);
                SwapBlockEndian(block, log2Bytes, endian);
            }
    return true;
}

bool CopyLinearBlocks3D(const uint8_t* source, size_t sourceBytes, uint8_t* out, uint32_t widthBlocks, uint32_t heightBlocks, uint32_t depth,
                        uint32_t rowPitchBytes, uint32_t zStrideBlockRows, uint32_t log2Bytes, uint32_t endian)
{
    if (depth == 0)
        return false;
    const size_t sliceStride = size_t(rowPitchBytes) * zStrideBlockRows, sliceBytes = (size_t(widthBlocks) * heightBlocks) << log2Bytes;
    if (heightBlocks > zStrideBlockRows || sliceStride * (depth - 1) > sourceBytes)
        return false;
    for (uint32_t z = 0; z < depth; z++)
        if (!CopyLinearBlocksAt(source + sliceStride * z, sourceBytes - sliceStride * z, out + sliceBytes * z, 0, 0, widthBlocks, heightBlocks,
                                rowPitchBytes, log2Bytes, endian))
            return false;
    return true;
}

}  // namespace runtime::video
