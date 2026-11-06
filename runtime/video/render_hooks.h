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
// them) and the images outlive every submission that uses them: destroyed only through the slot
// lifetime (GpuGuard::Hold) at shutdown.
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
    // from `base`'s register file and records the resolve into `commands` (compute to guest memory,
    // D-f). `d` carries the packet and the edram mode only (draw_state.cpp returns before the rest).
    bool (*resolve)(VkCommandBuffer commands, const uint8_t* base, const DrawState& d, uint64_t draw, std::string& error);
};

// Textures (unit 7): the view and sampler of a decoded fetch constant, for binding 8 + slot. The view's
// type matches TextureSlot::fetchDimension (2D, 3D, cube); the image is in SHADER_READ_ONLY_OPTIMAL
// when returned and stays valid until the slot's submission completes (held through the slot lifetime).
struct TextureHooks
{
    bool (*bind)(VkCommandBuffer commands, const TextureSlot& slot, VkImageView& view, VkSampler& sampler, std::string& error);
};

}  // namespace runtime::video
