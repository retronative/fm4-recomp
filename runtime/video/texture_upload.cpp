// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// texture_upload.h (see its header comment).
#include "texture_upload.h"
#include <cstdio>

namespace runtime::video {
namespace {

std::string Hex(uint32_t v)
{
    char text[16];
    std::snprintf(text, sizeof text, "%08X", v);
    return text;
}

}  // namespace

PreparedTexture TextureUnitModel::Prepare(const uint32_t fetch[6], const ShaderFetch& shader, bool drawStateGamma, const TextureDeviceFeatures& device)
{
    PreparedTexture p;
    TextureCache::Lookup lookup = cache_.Find(fetch);
    if (lookup.entry == nullptr)
    {
        p.error = lookup.error;
        return p;
    }
    const TextureDescription& d = lookup.entry->description;
    const std::string where = std::string(d.info.name) + " " + std::to_string(d.width) + "x" + std::to_string(d.height) + " at " + Hex(d.base.physical);
    if (!ObservedFormat(d.fetch.format))
    {
        p.error = where + ": format " + std::to_string(d.fetch.format) + " is mapped (texture_info.cpp) but not in the observed set (2, 50, 51) this unit was checked for";
        return p;
    }
    if (d.levelCount != 1)  // every observed constant has one level (mip address 0); DrawState samples base maps only
    {
        p.error = where + ": " + std::to_string(d.levelCount) + " levels (mip address " + Hex(d.fetch.mipAddress) + "): only one level is uploaded so far";
        return p;
    }
    if (d.host.blockCompressed && !device.textureCompressionBC)
    {
        p.error = where + ": a BC format, and the device has no textureCompressionBC (the DXT1 -> RGBA8 decode is not implemented)";
        return p;
    }
    p.binding = DescribeBinding(fetch, d.host, shader);
    if (!p.binding.ok)
    {
        p.error = where + ": " + p.binding.error;
        return p;
    }
    if (p.binding.sampler.anisotropyEnable)
    {
        p.error = where + ": anisotropic filtering (the device feature is not enabled)";
        return p;
    }
    if (!IdentityComponents(p.binding.components) && !device.imageViewFormatSwizzle)
    {
        p.error = where + ": component mapping " + std::to_string(p.binding.components[0]) + std::to_string(p.binding.components[1]) +
                  std::to_string(p.binding.components[2]) + std::to_string(p.binding.components[3]) +
                  " needs imageViewFormatSwizzle, which the device did not enable";
        return p;
    }
    if (p.binding.gamma != drawStateGamma)
    {
        p.error = where + ": the binding's gamma (" + std::to_string(p.binding.gamma) + ") disagrees with DrawState's (" + std::to_string(drawStateGamma) +
                  "), which chose the shader define";
        return p;
    }
    GuestRange hit;
    if (log_.Overlaps(d.base.physical, d.base.size, hit) || log_.Overlaps(d.mips.physical, d.mips.size, hit))
    {
        p.error = where + ": its guest range was written by a resolve recorded into the open submission (" + Hex(hit.physical) + " + " +
                  std::to_string(hit.size) + " bytes) that is not confirmed complete; the renderer must retire it first";
        return p;
    }
    p.entry = lookup.entry;
    p.load = validity_ == TextureValidity::ReloadEveryBind || lookup.load;
    if (p.load)
        p.plan = cache_.Plan(*lookup.entry);
    return p;
}

bool FillStaging(const TextureDescription& d, const LoadPlan& plan, GuestBytesFn guest, void* context, uint8_t* out, std::string& error)
{
    if (plan.levelOffsets.size() != d.levels.size() || plan.levelBytes.size() != d.levels.size())
    {
        error = "the load plan does not match the description's levels";
        return false;
    }
    for (size_t i = 0; i < d.levels.size(); i++)
    {
        const MipLevel& l = d.levels[i];
        const uint8_t* source = guest(context, l.address, uint32_t(l.extentBytes), error);
        if (source == nullptr)
            return false;
        uint8_t* level = out + plan.levelOffsets[i];
        const bool ok = d.fetch.tiled ? DetileBlocksAt(source, l.extentBytes, level, l.x, l.y, l.widthBlocks, l.heightBlocks, l.pitchBlocks,
                                                       d.log2BytesPerBlock, d.fetch.endian)
                                      : CopyLinearBlocksAt(source, l.extentBytes, level, l.x, l.y, l.widthBlocks, l.heightBlocks, l.rowPitchBytes,
                                                           d.log2BytesPerBlock, d.fetch.endian);
        if (!ok)
        {
            error = "level " + std::to_string(i) + " at " + Hex(l.address) + ": the " + (d.fetch.tiled ? "untile" : "linear copy") + " refused it";
            return false;
        }
    }
    return true;
}

std::vector<CopyRegion> CopyRegions(const TextureDescription& d, const LoadPlan& plan, uint64_t ringOffset)
{
    std::vector<CopyRegion> regions;
    for (size_t i = 0; i < d.levels.size() && i < plan.levelOffsets.size(); i++)
        regions.push_back({ringOffset + plan.levelOffsets[i], uint32_t(i), d.levels[i].widthTexels, d.levels[i].heightTexels});
    return regions;
}

}  // namespace runtime::video
