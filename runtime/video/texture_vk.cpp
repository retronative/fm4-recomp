// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// texture_vk.h. New code. The image, view and sampler parameters follow Xenia xenia-canary 67f16c4e315f
// src/xenia/gpu/vulkan/vulkan_texture_cache.cc (host formats :91-395 kBestHostFormats, through
// texture_info.cpp; VkSamplerCreateInfo :755-820, with texture_info.h's documented difference that min and
// mip filters are their own fields), BSD-3-Clause, read, not copied. The upload is the renderer's present
// upload pattern (renderer.cpp Present: staging -> vkCmdCopyBufferToImage between two layout barriers),
// with the staging block in the slot's ring instead of a held buffer.
#include "texture_vk.h"
#include "guest_writes.h"
#include "pipeline_key.h"
#include "stops.h"
#include "texture_dump.h"
#include "texture_upload.h"
#include "kernel/virtual_memory.h"
#include <cstdio>
#include <optional>
#include <unordered_map>

namespace runtime::video {

// texture_info.h's numeric mirrors of Vulkan (it builds without the header; its comment promises these).
static_assert(vk::kFormatUndefined == VK_FORMAT_UNDEFINED && vk::kFormatR8Unorm == VK_FORMAT_R8_UNORM &&
              vk::kFormatR8G8B8A8Unorm == VK_FORMAT_R8G8B8A8_UNORM && vk::kFormatA2B10G10R10UnormPack32 == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
              vk::kFormatBc1RgbaUnormBlock == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
static_assert(vk::kFilterNearest == VK_FILTER_NEAREST && vk::kFilterLinear == VK_FILTER_LINEAR &&
              vk::kMipmapModeNearest == VK_SAMPLER_MIPMAP_MODE_NEAREST && vk::kMipmapModeLinear == VK_SAMPLER_MIPMAP_MODE_LINEAR);
static_assert(vk::kAddressRepeat == VK_SAMPLER_ADDRESS_MODE_REPEAT && vk::kAddressMirroredRepeat == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
              vk::kAddressClampToEdge == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE && vk::kAddressClampToBorder == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
static_assert(vk::kBorderFloatTransparentBlack == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK && vk::kBorderFloatOpaqueWhite == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
static_assert(vk::kSwizzleZero == VK_COMPONENT_SWIZZLE_ZERO && vk::kSwizzleOne == VK_COMPONENT_SWIZZLE_ONE && vk::kSwizzleR == VK_COMPONENT_SWIZZLE_R &&
              vk::kSwizzleG == VK_COMPONENT_SWIZZLE_G && vk::kSwizzleB == VK_COMPONENT_SWIZZLE_B && vk::kSwizzleA == VK_COMPONENT_SWIZZLE_A);

namespace {

constexpr size_t kMaxTextureImages = 256;
constexpr uint64_t kMaxTextureImageBytes = 512ull << 20;

struct HostImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    bool initialized = false;  // a copy was recorded (layout SHADER_READ_ONLY_OPTIMAL after it)
};

struct Unit
{
    VulkanDevice dev;  // handles only (not owned)
    TextureDeviceFeatures features;
    uint64_t copyAlignment = 1;
    TextureUnitModel model{kTextureValidity};
    std::unordered_map<TextureKey, HostImage, TextureKeyHash> images;
    std::unordered_map<ViewKey, VkImageView, ViewKeyHash> views;
    std::unordered_map<SamplerDescription, VkSampler, SamplerDescriptionHash> samplers;
    std::unordered_map<uint32_t, VkFormatFeatureFlags> formatFeatures;
    uint64_t imageBytes = 0, uploads = 0, uploadedBytes = 0, uploadsSince = 0, uploadedBytesSince = 0;
};
std::optional<Unit> unit;

std::string Hex(uint32_t v)
{
    char text[16];
    std::snprintf(text, sizeof text, "%08X", v);
    return text;
}

bool Failed(VkResult r, const char* what, std::string& error)
{
    if (r == VK_SUCCESS)
        return false;
    error = std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")";
    return true;
}

// guest_memory through the view that holds the frames; both ends in one view, every page committed (a read
// of an uncommitted page would be an unnamed guest fault stop).
const uint8_t* GuestBytes(void* context, uint32_t physical, uint32_t bytes, std::string& error)
{
    const uint8_t* base = static_cast<const uint8_t*>(context);
    uint32_t guest = 0, guestEnd = 0;
    if (bytes == 0 || !runtime::PhysicalToGuest(physical, guest) || !runtime::PhysicalToGuest(physical + bytes - 1, guestEnd) || guestEnd - guest != bytes - 1)
    {
        error = "guest range " + Hex(physical) + " + " + std::to_string(bytes) + " bytes is not in one view";
        return nullptr;
    }
    const runtime::Heap* heap = runtime::LookupHeap(guest);
    if (heap == nullptr || !heap->IsCommitted(guest, bytes))
    {
        error = "guest range " + Hex(guest) + " + " + std::to_string(bytes) + " bytes is not committed";
        return nullptr;
    }
    return base + guest;
}

bool FormatSupported(VkFormat format, bool linear, std::string& error)
{
    auto it = unit->formatFeatures.find(uint32_t(format));
    if (it == unit->formatFeatures.end())
    {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(unit->dev.gpu, format, &p);
        it = unit->formatFeatures.emplace(uint32_t(format), p.optimalTilingFeatures).first;
        std::fprintf(stderr, "[gpu] texture format %d: optimal tiling features 0x%X\n", int(format), unsigned(p.optimalTilingFeatures));
    }
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (linear)
        needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((it->second & needed) != needed)
    {
        error = "host format " + std::to_string(int(format)) + " lacks features 0x" + Hex(needed & ~it->second) + " (sampled, transfer dst" +
                (linear ? ", linear filter)" : ")");
        return false;
    }
    return true;
}

HostImage* Image(const TextureEntry& e, std::string& error)
{
    if (auto it = unit->images.find(e.key); it != unit->images.end())
        return &it->second;
    const TextureDescription& d = e.description;
    if (unit->images.size() >= kMaxTextureImages)
    {
        error = "the texture unit holds " + std::to_string(kMaxTextureImages) + " images already (no eviction yet)";
        return nullptr;
    }
    HostImage h;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = d.dimension == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = VkFormat(d.host.vkFormat);
    info.extent = {d.width, d.height, d.depth};
    info.mipLevels = d.levelCount;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The device's limits for this format and type (a 3D BC or float image is a per-format property on Metal).
    VkImageFormatProperties limits{};
    if (Failed(vkGetPhysicalDeviceImageFormatProperties(unit->dev.gpu, info.format, info.imageType, info.tiling, info.usage, 0, &limits),
               "vkGetPhysicalDeviceImageFormatProperties (texture)", error))
    {
        error += ": host format " + std::to_string(int(info.format)) + (d.dimension == 2 ? " as a 3D image" : " as a 2D image");
        return nullptr;
    }
    if (info.extent.width > limits.maxExtent.width || info.extent.height > limits.maxExtent.height || info.extent.depth > limits.maxExtent.depth ||
        info.mipLevels > limits.maxMipLevels)
    {
        error = "a " + std::to_string(d.width) + "x" + std::to_string(d.height) + "x" + std::to_string(d.depth) + " image of host format " +
                std::to_string(int(info.format)) + " exceeds the device's " + std::to_string(limits.maxExtent.width) + "x" +
                std::to_string(limits.maxExtent.height) + "x" + std::to_string(limits.maxExtent.depth);
        return nullptr;
    }
    if (Failed(vkCreateImage(unit->dev.device, &info, nullptr, &h.image), "vkCreateImage (texture)", error))
        return nullptr;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(unit->dev.device, h.image, &requirements);
    if (unit->imageBytes + requirements.size > kMaxTextureImageBytes)
    {
        vkDestroyImage(unit->dev.device, h.image, nullptr);  // never bound, never used
        error = "texture images would exceed " + std::to_string(kMaxTextureImageBytes >> 20) + " MB (no eviction yet)";
        return nullptr;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(unit->dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (Failed(vkAllocateMemory(unit->dev.device, &allocate, nullptr, &h.memory), "vkAllocateMemory (texture)", error) ||
        Failed(vkBindImageMemory(unit->dev.device, h.image, h.memory, 0), "vkBindImageMemory (texture)", error))
    {
        vkDestroyImage(unit->dev.device, h.image, nullptr);  // never used by a command
        vkFreeMemory(unit->dev.device, h.memory, nullptr);   // null-safe
        return nullptr;
    }
    h.bytes = requirements.size;
    unit->imageBytes += h.bytes;
    return &unit->images.emplace(e.key, h).first->second;
}

void Barrier(VkCommandBuffer c, VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
             VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Guest memory -> the slot's ring -> the image. An image an earlier draw of this (or an earlier) submission
// sampled is overwritten only behind a barrier from the shader stages (write after read, same queue).
bool Upload(const TextureBindContext& c, const uint32_t words[6], const TextureDescription& d, const TextureBinding& binding, const LoadPlan& plan,
            HostImage& image, std::string& error)
{
    const uint64_t alignment = StagingAlignment(1u << d.log2BytesPerBlock, unit->copyAlignment);
    if (alignment == 0)
    {
        error = "no staging alignment for " + std::to_string(1u << d.log2BytesPerBlock) + "-byte blocks and optimalBufferCopyOffsetAlignment " +
                std::to_string(unit->copyAlignment);
        return false;
    }
    uint64_t offset = 0;
    if (!PlaceInRing(*c.allocator, plan.stagingBytes, alignment, offset))
    {
        error = "the slot's upload ring (" + std::to_string(c.allocator->Used()) + " of " + std::to_string(c.allocator->Capacity()) +
                " bytes used) cannot hold the " + std::to_string(plan.stagingBytes) + " staging bytes of the texture at " + Hex(d.base.physical) +
                " (every bind reloads, texture_upload.h kTextureValidity)";
        return false;
    }
    if (!FillStaging(d, plan, GuestBytes, const_cast<uint8_t*>(c.base), c.ringMapped + offset, error))
        return false;
    MaybeDumpTexture(words, d, binding, plan, GuestBytes, const_cast<uint8_t*>(c.base), c.ringMapped + offset);  // FM4_DUMP_TEXTURE (texture_dump.h)
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    Barrier(c.commands, image.image, d.levelCount, image.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.initialized ? shaders : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> copies;
    for (const CopyRegion& r : CopyRegions(d, plan, offset))
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = r.bufferOffset;  // rows tightly packed: bufferRowLength = bufferImageHeight = 0
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, 0, 1};
        copy.imageExtent = {r.width, r.height, r.depth};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(c.commands, c.ring, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    Barrier(c.commands, image.image, d.levelCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, shaders, VK_ACCESS_SHADER_READ_BIT);
    image.initialized = true;
    unit->uploads++, unit->uploadsSince++;
    unit->uploadedBytes += plan.stagingBytes, unit->uploadedBytesSince += plan.stagingBytes;
    return true;
}

VkImageView View(const TextureEntry& e, const HostImage& image, const uint32_t components[4], std::string& error)
{
    ViewKey key{e.key, {components[0], components[1], components[2], components[3]}};
    if (auto it = unit->views.find(key); it != unit->views.end())
        return it->second;
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.image;
    info.viewType = e.description.dimension == 2 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;  // the shader's sampler3D / sampler2D
    info.format = VkFormat(e.description.host.vkFormat);
    info.components = {VkComponentSwizzle(components[0]), VkComponentSwizzle(components[1]), VkComponentSwizzle(components[2]), VkComponentSwizzle(components[3])};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e.description.levelCount, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (Failed(vkCreateImageView(unit->dev.device, &info, nullptr, &view), "vkCreateImageView (texture)", error))
        return VK_NULL_HANDLE;
    unit->views.emplace(key, view);
    return view;
}

VkSampler Sampler(const SamplerDescription& s, std::string& error)
{
    if (auto it = unit->samplers.find(s); it != unit->samplers.end())
        return it->second;
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VkFilter(s.magFilter);
    info.minFilter = VkFilter(s.minFilter);
    info.mipmapMode = VkSamplerMipmapMode(s.mipmapMode);
    info.addressModeU = VkSamplerAddressMode(s.addressModeU);
    info.addressModeV = VkSamplerAddressMode(s.addressModeV);
    info.addressModeW = VkSamplerAddressMode(s.addressModeW);
    info.mipLodBias = 0.0f;  // LOD bias is the shader's (XE_TEXTURE_<slot>_LOD_BIAS)
    info.anisotropyEnable = VK_FALSE;  // refused before (texture_upload.cpp)
    info.maxAnisotropy = 1.0f;
    info.compareEnable = VK_FALSE;
    info.minLod = s.minLod;
    info.maxLod = s.maxLod;
    info.borderColor = VkBorderColor(s.borderColor);
    info.unnormalizedCoordinates = VK_FALSE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (Failed(vkCreateSampler(unit->dev.device, &info, nullptr, &sampler), "vkCreateSampler", error))
        return VK_NULL_HANDLE;
    unit->samplers.emplace(s, sampler);
    return sampler;
}

bool Bind(const TextureBindContext& c, const TextureSlot& t, VkImageView& view, VkSampler& sampler, std::string& error)
{
    if (!unit || c.allocator == nullptr || c.ringMapped == nullptr || c.base == nullptr)
    {
        error = "the texture unit is not initialized or the bind context is incomplete";
        return false;
    }
    // The resolved filters (the instruction's unless 3); aniso 7 = the fetch constant's (DrawState refused
    // any non-zero one).
    const ShaderFetch shader{t.magFilter, t.minFilter, t.mipFilter, 7, t.fetchDimension};
    PreparedTexture p = unit->model.Prepare(t.words, shader, t.gamma, unit->features);
    if (p.entry == nullptr)
    {
        error = p.error;
        return false;
    }
    const TextureDescription& d = p.entry->description;
    const SamplerDescription& s = p.binding.sampler;
    const bool linear = s.magFilter == vk::kFilterLinear || s.minFilter == vk::kFilterLinear || s.mipmapMode == vk::kMipmapModeLinear;
    if (!FormatSupported(VkFormat(d.host.vkFormat), linear, error))
        return false;
    HostImage* image = Image(*p.entry, error);
    if (image == nullptr)
        return false;
    if (p.load)
    {
        if (!Upload(c, t.words, d, p.binding, p.plan, *image, error))
            return false;
        if (!unit->model.Loaded(*p.entry, p.plan))
        {
            error = "the write watch refused the texture at " + Hex(d.base.physical);
            return false;
        }
    }
    else if (!image->initialized)
    {
        error = "the texture at " + Hex(d.base.physical) + " is valid in the cache but its host image was never loaded";
        return false;
    }
    view = View(*p.entry, *image, p.binding.components, error);
    if (view == VK_NULL_HANDLE)
        return false;
    sampler = Sampler(s, error);
    return sampler != VK_NULL_HANDLE;
}

uint64_t StagingBytes(const TextureSlot& t)
{
    if (!unit)
        return 0;
    const ShaderFetch shader{t.magFilter, t.minFilter, t.mipFilter, 7, t.fetchDimension};
    const PreparedTexture p = unit->model.Prepare(t.words, shader, t.gamma, unit->features);
    if (p.entry == nullptr)
        return 0;
    const uint64_t alignment = StagingAlignment(1u << p.entry->description.log2BytesPerBlock, unit->copyAlignment);
    return p.plan.stagingBytes + alignment;  // the worst-case padding PlaceInRing can add
}

const TextureHooks hooks{Bind, StagingBytes};

}  // namespace

const TextureHooks* InitializeTextureUnit(const VulkanDevice& device)
{
    unit.emplace();
    unit->dev = device;
    unit->features = {device.textureCompressionBC, device.imageViewFormatSwizzle};
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device.gpu, &properties);
    unit->copyAlignment = properties.limits.optimalBufferCopyOffsetAlignment;
    std::fprintf(stderr, "[gpu] texture unit: textureCompressionBC %d, imageViewFormatSwizzle %d, optimalBufferCopyOffsetAlignment %llu, validity %s\n",
                 int(device.textureCompressionBC), int(device.imageViewFormatSwizzle), (unsigned long long)unit->copyAlignment,
                 kTextureValidity == TextureValidity::ReloadEveryBind ? "reload at every bind" : "write watch");
    return &hooks;
}

void ShutdownTextureUnit()
{
    if (!unit)
        return;
    for (auto& [key, view] : unit->views)
        vkDestroyImageView(unit->dev.device, view, nullptr);
    for (auto& [key, sampler] : unit->samplers)
        vkDestroySampler(unit->dev.device, sampler, nullptr);
    for (auto& [key, image] : unit->images)
    {
        vkDestroyImage(unit->dev.device, image.image, nullptr);
        vkFreeMemory(unit->dev.device, image.memory, nullptr);
    }
    unit.reset();
}

std::string TextureUnitSummary()
{
    if (!unit)
        return "textures: no unit";
    char text[256];
    std::snprintf(text, sizeof text, "textures: %llu upload(s), %llu bytes since the last swap; %zu image(s) (%llu MB), %zu view(s), %zu sampler(s); %llu GPU-written range(s) marked",
                  (unsigned long long)unit->uploadsSince, (unsigned long long)unit->uploadedBytesSince, unit->images.size(),
                  (unsigned long long)(unit->imageBytes >> 20), unit->views.size(), unit->samplers.size(), (unsigned long long)unit->model.Log().Marks());
    unit->uploadsSince = unit->uploadedBytesSince = 0;
    return text;
}

void MarkGuestWritten(uint32_t physical, uint32_t bytes)
{
    if (!unit)
        Stop(StopCode::Unimplemented, "renderer: guest range %08X + %u bytes written by the GPU before the texture unit exists", physical, bytes);
    unit->model.MarkWritten(physical, bytes);
}

bool GuestWritesPending() { return unit && unit->model.Pending(); }

void GuestWritesRetired()
{
    if (unit)
        unit->model.Retired();
}

}  // namespace runtime::video
