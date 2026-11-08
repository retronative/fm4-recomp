// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The renderer's forward contracts with the two units drafted in parallel (execution model §7: unit 6
// the EDRAM ownership core and resolves, unit 7 textures through write watch). Declared here as what
// renderer.cpp:Draw calls, not implemented here; until a unit registers its hooks the renderer stops at
// the first draw that needs it (fail closed, never a placeholder image). Both run on the command
// processor's thread, inside the slot the renderer is recording, and may record into `commands`
// (layout transitions, transfers) but never submit: GpuGuard owns the queue (gpu_guard.h).
#pragma once
#include "draw_state.h"
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace runtime::video {

// The host images of a draw's render target, owned by the EDRAM unit (D-e, D-f: ownership map, GPU
// transfers, resolves). Contract: `color` / `depth` are views of 2D images of `format` /
// `depthFormat` with `samples` samples (= RenderTarget::samples), `width` x `height` pixels covering
// at least the draw's scissor (RenderTarget::pitch x targetHeight), each in the attachment-optimal
// layout (COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL) when returned and left in it by
// the render pass (pipeline_cache.cpp RenderPass: load, store, initial = final layout, external
// dependencies on both sides covering the attachment and transfer stages only). Anything the EDRAM
// unit does to these images outside those stages (a compute resolve reading the attachment, an
// ownership transfer) must record its own barriers into `commands` before and after. A view the draw
// does not use is VK_NULL_HANDLE. The views live for the run (the renderer's framebuffers reference
// them) and the images outlive every submission that uses them: destroyed only at shutdown, after
// GpuGuard::Shutdown confirmed that no submission's completion is unknown (EdramHooks::shutdown).
struct HostTargets
{
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, samples = 1;
};

struct EdramHooks
{
    // Before a Draw / DepthOnly draw is recorded: the host targets of `target` for the attachments the
    // draw uses (`needColor`: DrawKind::Draw with a colour write; `needDepth`: a depth or stencil test).
    // False with `error` refuses the draw (the renderer stops with it).
    bool (*acquireTargets)(VkCommandBuffer commands, const RenderTarget& target, bool needColor, bool needDepth, HostTargets& out, std::string& error);
    // A DrawKind::Resolve draw (RB_MODECONTROL 6, X xenos.h:926-960): the EDRAM unit decodes RB_COPY_*
    // from `base`'s register file and records the resolve into `commands` (D-f; the M4 unit takes the
    // fallback: a copy of the rectangle into its staging buffer, written into guest memory by `retired`).
    // `d` carries the packet and the edram mode only (draw_state.cpp returns before the rest). The
    // renderer flushes the slot after every resolve (renderer.cpp FlushSlot).
    bool (*resolve)(VkCommandBuffer commands, const uint8_t* base, const DrawState& d, uint64_t draw, std::string& error);
    // From RetireSlot, after the slot's submission was confirmed complete (GpuGuard::Retire) and before
    // GuestWritesRetired (guest_writes.h): the EDRAM unit writes that submission's read-back resolves into
    // guest memory. False with `error` stops.
    bool (*retired)(std::string& error);
    // Renderer shutdown, only after GpuGuard::Shutdown kept nothing and after the renderer destroyed its
    // framebuffers (they reference the EDRAM unit's views): destroys every resource of the unit.
    void (*shutdown)();
};

class UploadRing;  // pipeline_key.h

// What a texture bind may use of the slot being recorded (unit 7): its command buffer (outside any render
// pass), guest memory, and the slot's upload ring (renderer.cpp Slot::ring: host-visible, coherent, usable
// as a transfer source; reset only in OpenSlot, after the slot's previous submission was confirmed
// complete, so a block allocated here is never one the GPU may still read).
struct TextureBindContext
{
    VkCommandBuffer commands = VK_NULL_HANDLE;
    const uint8_t* base = nullptr;  // guest memory
    VkBuffer ring = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    UploadRing* allocator = nullptr;
};

// Textures (unit 7): the view and sampler of a decoded fetch constant, for binding 8 + slot. The view's
// type matches TextureSlot::fetchDimension (2D only so far); the image is in SHADER_READ_ONLY_OPTIMAL
// when returned. Images, views and samplers live for the run (texture_vk.h) and are destroyed only after
// GpuGuard::Shutdown confirmed every submission; staging bytes are in the slot's ring (above). False with
// `error` refuses the draw (the renderer stops with it); a hook never stops by itself.
struct TextureHooks
{
    bool (*bind)(const TextureBindContext& context, const TextureSlot& slot, VkImageView& view, VkSampler& sampler, std::string& error);
    // The ring bytes `bind` will place for this slot (staging plus alignment), so the renderer can flush the slot before a draw
    // whose uploads would not fit; 0 when the texture cannot be prepared (bind then refuses it with the reason).
    uint64_t (*stagingBytes)(const TextureSlot& slot);
};

}  // namespace runtime::video
