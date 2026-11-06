// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// The copies of texture_untile.h (see its header comment for sources and attribution).
#include "texture_untile.h"
#include <cstring>

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

}  // namespace runtime::video
