// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// gpu_present.h (see its header comment for sources and attribution).
#include "gpu_present.h"

namespace runtime::video {

#define FM4_GPU_PRESENT_STRINGIFY2(...) #__VA_ARGS__
#define FM4_GPU_PRESENT_STRINGIFY(...) FM4_GPU_PRESENT_STRINGIFY2(__VA_ARGS__)

std::string GpuPresentGlsl()
{
    return std::string("#version 450\n"
                       "layout(local_size_x = ") +
           std::to_string(kGpuPresentGroupSize) +
           ") in;\n"
           "layout(std430, set = 0, binding = 0) readonly buffer XeWords { uint xe_words[]; };\n"
           "layout(std430, set = 0, binding = 1) readonly buffer XeLut { uint xe_lut[]; };\n"
           "layout(std430, set = 0, binding = 2) writeonly buffer XeDisplay { uint xe_display[]; };\n"
           "layout(push_constant) uniform XePresentPush { uint count; uint endian; uint swap_red_blue; } pc;\n"
           "uint XePresentLut(uint i) { return xe_lut[i]; }\n" +
           FM4_GPU_PRESENT_STRINGIFY(FM4_GPU_PRESENT_BODY) +
           "\nvoid main()\n"
           "{\n"
           "    uint i = gl_GlobalInvocationID.x;\n"
           "    if (i >= pc.count)\n"
           "        return;\n"
           "    xe_display[i] = XePresentWord(xe_words[i], pc.endian, pc.swap_red_blue);\n"
           "}\n";
}

FrontBand FrontResolveBand(const ResidentResolve& r, const FrontShape& f, std::string* why)
{
    FrontBand band;
    const uint32_t tilesPerRow = TilesPerRow(f.pitch);
    const uint32_t delta = r.textureBase - f.base, macro = delta >> 12;
    const uint32_t cornerX = tilesPerRow == 0 ? 0 : macro % tilesPerRow * 32, cornerY = tilesPerRow == 0 ? 0 : macro / tilesPerRow * 32;
    const uint64_t row = uint64_t(cornerY) + r.y0;
    const char* reason = !r.converted                                                  ? "the resolve has no GPU words"
                         : r.bytesLog2 != 2                                            ? "not a 32 bpp destination"
                         : r.destFormat != 6 && r.destFormat != 7 && r.destFormat != 50 && r.destFormat != 54
                             ? "a destination format whose write-back is not its GPU words"
                         : tilesPerRow == 0 || TilesPerRow(r.destPitch) != tilesPerRow ? "another pitch"
                         : r.textureBase < f.base || delta % 4096 != 0                 ? "another base"
                         : cornerX + r.x0 != 0 || r.width != f.width                   ? "not a band of whole rows"
                         : r.height == 0 || row + r.height > f.height                  ? "rows outside the front buffer"
                                                                                       : nullptr;
    if (reason != nullptr)
    {
        if (why != nullptr)
            *why = reason;
        return band;
    }
    band.ok = true, band.row = uint32_t(row), band.rows = r.height;
    return band;
}

void FrontWordsTracker::Drop(Entry& e)
{
    e.state = State::Empty, e.covered = 0;
    e.rows.assign(e.shape.height, false);
}

size_t FrontWordsTracker::Note(const FrontShape& s, uint64_t swap)
{
    size_t pick = kEntries;
    for (size_t i = 0; i < kEntries && pick == kEntries; i++)
        if (entries_[i].used && entries_[i].shape.base == s.base)
            pick = i;
    for (size_t i = 0; i < kEntries && pick == kEntries; i++)
        if (!entries_[i].used)
            pick = i;
    if (pick == kEntries)  // the least recently noted
    {
        pick = 0;
        for (size_t i = 1; i < kEntries; i++)
            if (entries_[i].noted < entries_[pick].noted)
                pick = i;
    }
    Entry& e = entries_[pick];
    if (!e.used || !(e.shape == s))
    {
        e = Entry{}, e.shape = s, e.used = true;
        Drop(e);
    }
    e.noted = swap;
    return pick;
}

FrontWordsTracker::Take FrontWordsTracker::OnResolve(const ResidentResolve& r, uint64_t serial)
{
    Take take;
    for (size_t i = 0; i < kEntries; i++)
    {
        Entry& e = entries_[i];
        const uint64_t start = e.shape.base, end = start + FrontBytes(e.shape);
        const uint64_t rStart = r.extentStart, rEnd = rStart + r.extentLength;
        if (!e.used || r.extentLength == 0 || rEnd <= start || rStart >= end)
            continue;
        const FrontBand band = FrontResolveBand(r, e.shape, nullptr);
        if (!band.ok || (e.state == State::Recorded && r.destEndian != e.endian))
        {
            Drop(e);
            continue;
        }
        if (e.state != State::Recorded)  // Empty, or Landed: a new set of bands
        {
            Drop(e);
            e.state = State::Recorded, e.firstSerial = serial, e.endian = r.destEndian;
        }
        e.lastSerial = serial;
        for (uint32_t y = band.row; y < band.row + band.rows; y++)
            if (!e.rows[y])
                e.rows[y] = true, e.covered++;
        take.entry = int(i), take.row = band.row;
    }
    return take;
}

}  // namespace runtime::video
