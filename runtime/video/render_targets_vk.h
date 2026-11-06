// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The Vulkan side of the EDRAM render-target model (execution model unit 6): the PLAN, header level only.
// Nothing here runs on the GPU yet; the declarations fix the shape the implementation takes so that unit 6's
// GPU validation and unit 8's draws build on it. The CPU model (edram.h) decides everything that can be
// decided without a device; this layer only executes its outputs on MoltenVK 1.4.2 (the pinned driver,
// vulkan_device.h) through GpuGuard (gpu_guard.h: every submission gated, every resource held by its slot's
// lifetime, every wait bounded, GPU_STOP honoured). Citations: X = xenia-canary 67f16c4e315f src/xenia/gpu;
// MVK = MoltenVK v1.4.2 (Docs/MoltenVK_Runtime_UserGuide.md, MoltenVK/GPUObjects/*.mm).
//
// 1. Host render targets: one VkImage per edram::Key (X vulkan_render_target_cache.cc:1700-1760
//    CreateRenderTarget), sized Key::Width() x Key::Height() (the full EDRAM extent of the pitch: X
//    render_target_cache.h:528-537), usage colour/depth attachment + sampled + transfer src/dst; created on
//    first use, never destroyed while a slot may use them (SubmissionLifetime). Formats:
//      colour storage 0 (8_8_8_8, GAMMA)   VK_FORMAT_R8G8B8A8_UNORM, an sRGB view for GAMMA (X :1593-1597)
//      colour storage 2 (2_10_10_10, _AS_) VK_FORMAT_A2B10G10R10_UNORM_PACK32: R in bits 0-9 as the Xenos
//                                          word (front_buffer.h swizzle evidence). X canary :1598-1601 writes
//                                          VK_FORMAT_A8B8G8R8_UNORM_PACK32 here (a 4x8 format): recorded as a
//                                          discrepancy, not followed.
//      colour 3/12, 4-7, 14, 15             refused in M4 (edram.h SupportedKey; X :1602-1626 for later)
//      depth kD24S8                         VK_FORMAT_D24_UNORM_S8_UINT when vkGetPhysicalDeviceFormatProperties
//                                          reports DEPTH_STENCIL_ATTACHMENT | SAMPLED for it (X :266-274,
//                                          :1581-1588), else VK_FORMAT_D32_SFLOAT_S8_UINT. MVK maps it to
//                                          MTLPixelFormatDepth24Unorm_Stencil8 only when the Metal device reports
//                                          isDepth24Stencil8PixelFormatSupported (MVKDevice.mm:128,
//                                          MVKPixelFormats.mm:912, :1488), which Apple GPUs do not (UNRESOLVED on
//                                          this Mac until the self-check logs the format properties): plan for
//                                          D32_SFLOAT_S8_UINT.
//      depth kD24FS8 (20e4)                 VK_FORMAT_D32_SFLOAT_S8_UINT (X :1587); the guest's [0, 2) range is
//                                          stored halved, [0, 1) (X :5357-5359), which unit 8's viewport depth
//                                          scale must match.
// 2. MSAA (decision D-1): (a) true multisampled images, VK_SAMPLE_COUNT_4_BIT, as
//    X (:1733-1740) does; or (b) "samples as
//    pixels": a 4x Key is a single-sample image of (2 * width) x (2 * height) whose pixel (2x + (s >> 1 & 1),
//    2y + (s & 1)) is sample s of pixel (x, y) (X shaders/edram.xesli:27-33). Under (b) FM4's 4x <-> 1x alias of
//    the same format is the same bytes in the same layout, so its transfer is a plain copy (or nothing, if one
//    image serves both Keys); under (a) it is a resolve-like draw each way. (a) is faithful to the console's
//    coverage; (b) supersamples. Recommendation: (a) for M4, (b) measured at M5 against the console frames.
// 3. Render passes and framebuffers are cached by key (X vulkan_render_target_cache.h:38-90 RenderPassKey:
//    samples, which of depth + 4 colour are used, their formats; :436-460 FramebufferKey: the render pass and the
//    attachments' views; maps :877-880). A draw whose Binding keys equal the open pass's continues it; otherwise
//    the pass ends, the Binding's transfers run, and a new pass begins (transfers never run inside a pass).
// 4. Transfers (edram::Transfer -> rectangles by edram::RangeRectangles in BOTH images' layouts: the source's
//    rectangles are the same tiles in the source Key's pixel space):
//      same VkFormat and sample count         vkCmdCopyImage per rectangle (bit copy; FM4's alias under 2(b))
//      otherwise                              a fullscreen-triangle draw into the destination with the source
//                                             bound as a sampled image, through integer views so that NaNs and
//                                             SNORM -1 survive (X :1628-1650 GetColorOwnershipTransferVulkanFormat),
//                                             per-sample when the destination is multisampled (sampleRateShading,
//                                             gl_SampleID), as X's transfer pipelines do (:594-700 setup,
//                                             TransferShaderKey vulkan_render_target_cache.h:570-608). Depth
//                                             destinations write gl_FragDepth and stencil through
//                                             VK_EXT_shader_stencil_export (MVK UserGuide.md:395, Mac GPU family 2).
//                                             Compute cannot write multisampled images on Metal, so no compute
//                                             transfer into a 4x target. Colour -> depth and depth-format changes
//                                             are refused by the CPU model in M4 (unit 6b).
// 5. Clears (edram::ResolveClear): vkCmdClearAttachments with the rectangle inside a pass on that target (X
//    :5340-5366), the depth value decoded as X does (kD24S8: UNorm24To32(bits >> 8); kD24FS8: Float20e4To32
//    (bits >> 8) * 0.5), the stencil = bits & 0xFF. A clear draw by the title (D3D's fast clear) is
//    an ordinary draw.
// 6. Resolve copy (edram::ResolvePlan.copyRect of copySource -> guest memory), decision D-f. First the plan's
//    copyTransfers run as in 4 (FM4 resolves the 1x surface in frames whose last colour writer was the 4x clear),
//    outside any render pass; then:
//      default   a compute pass reading the source image (per-sample; averaging per ResolveInfo.sampleSelect,
//                X shaders/resolve_fast_32bpp_*.xesli), converting storage 2 -> destFormat 6/7/50/54, applying
//                destSwap and destEndian (GpuSwap32 in the shader), and writing the tiled texel addresses
//                (front_buffer.h TiledOffset = X texture_address.xesli XeTextureTiledOffset2D) into a buffer that
//                IS guest memory: VkDeviceMemory imported once from the guest's physical block with
//                VK_EXT_external_memory_host (MVK UserGuide.md:348 lists it; minImportedHostPointerAlignment =
//                the host page size, 16 KB on Apple silicon, MVKDevice.mm:1279-1280; the import takes the
//                host-visible memory types, MVKDevice.mm:4051-4066; ReCarbon presenter.cpp:2522-2545 is the
//                precedent). The written range is ResolveInfo.destExtentStart/Length: GuardGpu(write) on it
//                until the slot completes, then MarkWritten so that textures over it reload (unit 7).
//      fallback  (if the import fails on this device, UNRESOLVED until unit 4's device log): vkCmdCopyImageToBuffer
//                into a staging buffer, a bounded wait, then the CPU tiles into guest memory with TiledOffset
//                and marks it written. Rejected as the default (ForzaRecomp L T4: ~5 ms a resolve) but exact.
// 7. Present continues unchanged: VdSwap's XE_SWAP names the front buffer's physical address; Present
//    (renderer.cpp) reads it from guest memory under the GPU guard and detiles it (front_buffer.h). A resolve
//    that writes the front buffer through 6 is what makes the first drawn frame appear; no resolve-to-texture
//    fusion in M4 (ShiftGlue disables it for FM4 too, proposal D-f).
#pragma once
#include "edram.h"
#include <cstdint>
#include <vulkan/vulkan.h>

namespace runtime::video {

// The host image of one edram::Key.
struct HostTarget
{
    edram::Key key;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;         // the attachment view (sRGB for GAMMA keys is a second view)
    VkImageView transferView = VK_NULL_HANDLE; // the integer view transfers sample from (plan 4)
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 0, height = 0;            // Key::Width/Height, times 2 each under plan 2(b)
};

// Plan 1: the VkFormat of a storage colour format; VK_FORMAT_UNDEFINED for a refused one.
VkFormat HostColorFormat(edram::ColorFormat storage);
// Plan 1: the VkFormat of a depth format; `d24s8Supported` from vkGetPhysicalDeviceFormatProperties at device
// creation (X vulkan_render_target_cache.cc:266-274).
VkFormat HostDepthFormat(edram::DepthFormat format, bool d24s8Supported);

// Plan 3.
struct RenderPassKey
{
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depth = VK_FORMAT_UNDEFINED;      // UNDEFINED: no depth attachment
    VkFormat color[4] = {};                    // UNDEFINED: slot unused
    constexpr bool operator==(const RenderPassKey&) const = default;
};
struct FramebufferKey
{
    RenderPassKey pass;
    VkImageView depth = VK_NULL_HANDLE;
    VkImageView color[4] = {};
    constexpr bool operator==(const FramebufferKey&) const = default;
};

// Plan 4: one recorded transfer, both images' rectangles for the same tiles.
struct TransferOp
{
    const HostTarget* source = nullptr;
    const HostTarget* dest = nullptr;
    edram::Rect sourceRects[3], destRects[3];
    uint32_t rectCount = 0;
    bool bitCopy = false;  // vkCmdCopyImage (same format and samples) rather than the transfer draw
};

// Plan 6: the compute resolve's push constants (all from edram::ResolveInfo; no title address).
struct ResolvePush
{
    uint32_t x0, y0, width, height;            // the source rectangle in the source Key's pixels
    uint32_t sampleSelect;                     // edram::SampleSelect
    uint32_t destFormat, destEndian, destSwap; // ResolveInfo.destFormat/destEndian/destSwap
    uint32_t destPitch;                        // texels; tiled addressing as TiledOffset(x, y, pitch, 2)
    uint32_t destOffsetX, destOffsetY;         // (x0, y0) within the destination's first 32x32 tile
    uint32_t destBytesOffset;                  // ResolveInfo.destBase - the imported block's physical base
};

}  // namespace runtime::video
