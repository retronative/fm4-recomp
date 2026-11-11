// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The display gamma ramp model (gamma_ramp.h). "X file:line": Xenia xenia-canary 67f16c4e315f src/xenia/gpu/.
#include "gamma_ramp.h"

namespace runtime::gpu::gamma {
namespace {
// The 8-bit rw_index field of DC_LUT_RW_INDEX (X registers.h:852-861) increased by one, wrapping in its 8 bits.
uint32_t NextIndex8(uint32_t rwIndex) { return (rwIndex & ~0xFFu) | ((rwIndex + 1) & 0xFFu); }
}  // namespace

Ramp DefaultRamp()
{
    Ramp r;
    for (uint32_t i = 0; i < 256; i++)
    {
        const uint32_t value = i * 0x3FF / 0xFF;
        r.table[i] = value | value << 10 | value << 20;
    }
    for (uint32_t i = 0; i < 128; i++)
        for (uint32_t j = 0; j < 3; j++)
            r.pwl[i][j] = {uint16_t((i * 0xFFFF / 0x7F) & ~0x3Fu), uint16_t(i < 0x7F ? 0x200 : 0)};
    return r;
}

Write OnRegisterWritten(Ramp& ramp, uint32_t index, uint32_t value, uint32_t rwMode, uint32_t writeEnable, uint32_t& rwIndex)
{
    switch (index)
    {
    case reg::kRwIndex:  // X :469-473
        ramp.component = 0;
        return Write::None;
    case reg::kSeqColor:  // X :475-510
    {
        if (rwMode & 1)
            return Write::ModeMismatch;
        // Red, green, blue order; the write enable mask is blue (bit 0), green, red (bit 2).
        const bool enabled = (writeEnable & (1u << (2 - ramp.component))) != 0;
        if (enabled)
        {
            uint32_t& entry = ramp.table[rwIndex & 0xFF];
            const uint32_t color = (value & 0xFFFF) >> 6;  // seq_color bits 0-15, 0-5 hardwired to zero
            const uint32_t shift = 20 - 10 * ramp.component;  // red 20-29, green 10-19, blue 0-9
            entry = (entry & ~(0x3FFu << shift)) | color << shift;
        }
        if (++ramp.component >= 3)
        {
            ramp.component = 0;
            rwIndex = NextIndex8(rwIndex);
        }
        return enabled ? Write::Table : Write::None;
    }
    case reg::kPwlData:  // X :511-544
    {
        if (!(rwMode & 1))
            return Write::ModeMismatch;
        const uint32_t entry = rwIndex & 0x7F;  // bit 7 of the index is ignored for PWL
        const bool enabled = (writeEnable & (1u << (2 - ramp.component))) != 0;
        if (enabled)  // bits 0-5 of the base and the delta are hardwired to zero
            ramp.pwl[entry][ramp.component] = {uint16_t(value & 0xFFC0u), uint16_t((value >> 16) & 0xFFC0u)};
        if (++ramp.component >= 3)
        {
            ramp.component = 0;
            rwIndex = (rwIndex & ~0x7Fu) | ((entry + 1) & 0x7F);
        }
        return enabled ? Write::Pwl : Write::None;
    }
    case reg::k30Color:  // X :546-573
    {
        if (rwMode & 1)
            return Write::ModeMismatch;
        const uint32_t mask = writeEnable & 7;
        if (mask)
        {
            uint32_t& entry = ramp.table[rwIndex & 0xFF];
            for (uint32_t bit = 0; bit < 3; bit++)  // bit 0 blue (0-9), 1 green (10-19), 2 red (20-29)
                if (mask >> bit & 1)
                    entry = (entry & ~(0x3FFu << (10 * bit))) | (value & (0x3FFu << (10 * bit)));
        }
        rwIndex = NextIndex8(rwIndex);
        ramp.component = 0;
        return mask ? Write::Table : Write::None;
    }
    default:
        return Write::None;
    }
}

uint32_t ApplyPwl10(uint32_t v, PwlEntry e)
{
    const uint32_t numerator = 8 * uint32_t(e.base) + (v & 7) * uint32_t(e.delta);  // < 2^23
    const uint32_t out = (numerator + 256) / 512;
    return out > 1023 ? 1023 : out;
}

void ApplyPwlToFrame(uint32_t* pixels, size_t count, const PwlEntry (&pwl)[128][3])
{
    // FM4 Recomp (M7 CPU cost; measured: 381 ms of the windowed CP profile, ~1.5 ms a swap): every
    // channel value's result, already shifted into place, in three 1024-entry tables built from ApplyPwl10 itself (3 x 1024
    // evaluations instead of 3 a pixel); the per-pixel result is the same expression, so the frame is bit-identical.
    uint32_t lut[3][1024];
    for (uint32_t c = 0; c < 3; c++)
        for (uint32_t v = 0; v < 1024; v++)
            lut[c][v] = ApplyPwl10(v, pwl[v >> 3][c]) << (10 * c);
    for (size_t i = 0; i < count; i++)
    {
        const uint32_t p = pixels[i];
        pixels[i] = (3u << 30) | lut[0][p & 0x3FF] | lut[1][(p >> 10) & 0x3FF] | lut[2][(p >> 20) & 0x3FF];
    }
}

uint64_t PwlDigest(const PwlEntry (&pwl)[128][3])
{
    uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&h](uint16_t v) {
        for (uint32_t b = 0; b < 2; b++)
            h = (h ^ ((v >> (8 * b)) & 0xFF)) * 0x100000001B3ull;
    };
    for (const auto& entry : pwl)
        for (const PwlEntry& e : entry)
            mix(e.base), mix(e.delta);
    return h;
}

bool IsD3dLinearPwl(const PwlEntry (&pwl)[128][3])
{
    for (uint32_t i = 0; i < 128; i++)
        for (const PwlEntry& e : pwl[i])
            if (e.base != (i < 0x7F ? i * 0x200 : 0xFFC0) || e.delta != (i < 0x7F ? 0x200 : 0))
                return false;
    return true;
}

}  // namespace runtime::gpu::gamma
