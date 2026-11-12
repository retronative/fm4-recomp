// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// texture_untile_gpu.h (see its header comment for sources and attribution).
#include "texture_untile_gpu.h"
#include "texture_untile.h"

namespace runtime::video {

#define FM4_UNTILE_STRINGIFY2(...) #__VA_ARGS__
#define FM4_UNTILE_STRINGIFY(...) FM4_UNTILE_STRINGIFY2(__VA_ARGS__)

std::string UntileGpuGlsl()
{
    return std::string("#version 450\n"
                       "layout(local_size_x = ") +
           std::to_string(kUntileGpuGroupSize) +
           ") in;\n"
           "layout(std430, set = 0, binding = 0) readonly buffer XeGuest { uint xe_guest[]; };\n"
           "layout(std430, set = 0, binding = 1) writeonly buffer XeStaging { uint xe_staging[]; };\n"
           "layout(push_constant) uniform XeUntilePush { uint src_byte; uint dst_word; uint dwords; uint level_bytes; uint width_blocks; uint x0;\n"
           "    uint y0; uint pitch; uint log2_bytes; uint tiled; uint swap_mask; } pc;\n"
           "uint XeGuestDword(uint i) { return xe_guest[i]; }\n" +
           FM4_UNTILE_STRINGIFY(FM4_UNTILE_GPU_BODY) +
           "\nvoid main()\n"
           "{\n"
           "    uint w = gl_GlobalInvocationID.x;\n"
           "    if (w >= pc.dwords)\n"
           "        return;\n"
           "    xe_staging[pc.dst_word + w] = XeUntileOutputDword(w, pc.level_bytes, pc.src_byte, pc.width_blocks, pc.x0, pc.y0, pc.pitch, pc.log2_bytes,\n"
           "                                                       pc.tiled, pc.swap_mask);\n"
           "}\n";
}

uint32_t UntileSwapMask(uint32_t log2Bytes, uint32_t endian)
{
    if (log2Bytes == 0)
        return 0;
    if (log2Bytes == 1)
        return endian == kEndian8in16 ? 1 : 0;
    static constexpr uint32_t kMask[4] = {0, 1, 3, 2};  // SwapBlockEndian kOrder: none, {1,0,3,2}, {3,2,1,0}, {2,3,0,1}
    return kMask[endian & 3];
}

namespace {
bool Inside(uint32_t address, uint64_t bytes, const GuestRange& r)
{
    return r.size != 0 && address >= r.physical && uint64_t(address) + bytes <= uint64_t(r.physical) + r.size;
}
}  // namespace

bool PlanGpuUntile(const TextureDescription& d, const LoadPlan& plan, uint32_t maxGroupsX, std::vector<GpuUntileLevel>& levels, std::string& why)
{
    levels.clear();
    if (d.dimension == 2 || d.layers != 1 || d.depth != 1)
        return why = "a 3D or cube texture", false;
    if (d.host.depth20e4)
        return why = "a converted format (20e4 depth)", false;
    if (d.levels.empty() || plan.levelOffsets.size() != d.levels.size() || plan.levelBytes.size() != d.levels.size())
        return why = "the load plan does not match the levels", false;
    const uint32_t log2 = d.log2BytesPerBlock;
    if (log2 > 4 || !EndianSupported(log2, d.fetch.endian))
        return why = "an unsupported block size or endian", false;
    for (size_t i = 0; i < d.levels.size(); i++)
    {
        const MipLevel& l = d.levels[i];
        const uint64_t bytes = (uint64_t(l.widthBlocks) * l.heightBlocks) << log2;
        if (l.widthBlocks == 0 || l.heightBlocks == 0 || bytes != plan.levelBytes[i] || bytes > 0xFFFFFFF0ull)
            return why = "level " + std::to_string(i) + ": an empty or oversized level", false;
        if ((plan.levelOffsets[i] & 3) != 0)
            return why = "level " + std::to_string(i) + ": a staging offset that is not a dword multiple", false;
        // The guard covers the texture's watch ranges; a level outside them would be read unguarded.
        if (l.extentBytes > 0xFFFFFFFFull || (!Inside(l.address, l.extentBytes, d.base) && !Inside(l.address, l.extentBytes, d.mips)))
            return why = "level " + std::to_string(i) + ": its storage is outside the texture's guest ranges", false;
        // DetileBlocksAt / CopyLinearBlocksAt's refusals (FillStaging names them): the shader cannot refuse.
        const bool fits = d.fetch.tiled ? (l.pitchBlocks & (kTileWidthHeight - 1)) == 0 && l.x <= l.pitchBlocks && l.widthBlocks <= l.pitchBlocks - l.x &&
                                              TiledAddressUpperBound2D(l.x + l.widthBlocks, l.y + l.heightBlocks, l.pitchBlocks, log2) <= l.extentBytes
                                        : (l.rowPitchBytes & ((1u << log2) - 1)) == 0 && (uint64_t(l.x + l.widthBlocks) << log2) <= l.rowPitchBytes &&
                                              uint64_t(l.rowPitchBytes) * l.y + (uint64_t(l.x) << log2) +
                                                      LinearExtent(l.widthBlocks, l.heightBlocks, l.rowPitchBytes, log2) <=
                                                  l.extentBytes;
        if (!fits)
            return why = "level " + std::to_string(i) + ": the " + (d.fetch.tiled ? "untile" : "linear copy") + " would refuse it", false;
        GpuUntileLevel g;
        g.address = l.address;
        g.extentBytes = l.extentBytes;
        g.levelOffset = plan.levelOffsets[i];
        g.push.levelBytes = uint32_t(bytes);
        g.push.dwords = uint32_t((bytes + 3) / 4);
        g.push.widthBlocks = l.widthBlocks, g.push.x0 = l.x, g.push.y0 = l.y;
        g.push.pitch = d.fetch.tiled ? l.pitchBlocks : l.rowPitchBytes;
        g.push.log2Bytes = log2;
        g.push.tiled = d.fetch.tiled ? 1 : 0;
        g.push.swapMask = UntileSwapMask(log2, d.fetch.endian);
        const uint64_t groups = (uint64_t(g.push.dwords) + kUntileGpuGroupSize - 1) / kUntileGpuGroupSize;
        if (groups > maxGroupsX)
            return levels.clear(), why = "level " + std::to_string(i) + ": " + std::to_string(groups) + " workgroups exceed the device's limit", false;
        g.groups = uint32_t(groups);
        levels.push_back(g);
    }
    return true;
}

bool GpuUntileBind(GpuUntileLevel& level, uint64_t ringLevelOffset, uint64_t guestBufferBytes, uint64_t ringBytes, uint64_t storageAlign,
                   uint64_t maxRange, GpuUntileBinding& b)
{
    if (storageAlign == 0 || (storageAlign & (storageAlign - 1)) != 0)
        return false;
    b.guestOffset = level.address & ~(storageAlign - 1);
    const uint64_t src = level.address - b.guestOffset;
    b.guestRange = (src + level.extentBytes + 3) & ~uint64_t(3);
    b.ringOffset = ringLevelOffset & ~(storageAlign - 1);
    const uint64_t dst = ringLevelOffset - b.ringOffset;
    b.ringRange = dst + uint64_t(level.push.dwords) * 4;
    if (b.guestOffset + b.guestRange > guestBufferBytes || b.guestRange > maxRange || b.guestRange > 0xFFFFFFFFull || (dst & 3) != 0 ||
        b.ringOffset + b.ringRange > ringBytes || b.ringRange > maxRange)
        return false;
    level.push.srcByte = uint32_t(src);
    level.push.dstWord = uint32_t(dst / 4);
    return true;
}

GpuLoadChoice ChooseTextureLoad(bool enabled, bool readsPending, bool planned, const TextureDescription& d, uint64_t serial, GuestReadGuardFn guard,
                                void* context)
{
    if (!enabled)
        return {false, "off"};
    if (readsPending)
        return {false, "it reads a pending resolve write-back"};
    if (!planned)
        return {false, "not a GPU layout"};
    if (guard == nullptr || !guard(context, d.base.physical, d.base.size, serial))
        return {false, "level 0 could not be guarded"};
    if (d.mips.size != 0 && !guard(context, d.mips.physical, d.mips.size, serial))
        return {false, "the mips could not be guarded"};
    return {true, "guarded"};
}

}  // namespace runtime::video
