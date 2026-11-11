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
#include "resolve_resident.h"
#include <cstdint>
#include <string>
#include <vector>
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
// FM4 Recomp (M7 GPU cost): a resolve clear the EDRAM unit has not applied yet. The unit hands the
// pending clears of the acquired targets to the renderer, which records them (vkCmdClearAttachments) as the first commands of the
// draw's render pass, before the draw; the unit applies them itself before any other read or write of the target.
struct PendingClear
{
    VkClearAttachment attachment{};  // colour attachment 0, or depth + stencil
    VkRect2D rect{};
};

struct HostTargets
{
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, samples = 1;
    std::vector<PendingClear> clears;  // to record first in the draw's pass (the unit has forgotten them)
};

// FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): a clear-class draw (draw_record.h FindClearDraw) handed to acquireTargets instead of being
// drawn. After the bind (its cutouts are `rect`) and the transfers of the tiles outside the rectangle, the unit keeps a pending
// clear of `rect` on each bound target (`color`: colour 0 with `rgba`, as the pixel shader would export it; `depth`: depth + stencil
// with `depthValue` / `stencil`), recorded first in the next pass that touches the target exactly as a deferred resolve clear is; the
// targets' older pending clears stay pending (nothing is recorded for this draw) and HostTargets::clears stays empty.
struct ClearRequest
{
    edram::Rect rect;
    bool color = false, depth = false;
    float rgba[4] = {0, 0, 0, 0};
    float depthValue = 0;
    uint32_t stencil = 0;
};

// FM4 Recomp (FM4_PASS_MERGE=1): the renderer may leave a draw's render pass open for the next draw on the same framebuffer.
// Every recording that must happen outside a render pass (barriers, copies, clears, the units' own passes) first calls
// OutsideRenderPass, which ends it. renderer.cpp registers the function; it is null in the units' tests.
inline void (*endOpenRenderPass)(VkCommandBuffer) = nullptr;
// FM4 Recomp (FM4_DIAG_PASS_TIMES, diagnostic): the job recording at the moment (a scoped label), for the tallies of what ends a
// draw render pass and of the GPU time between two passes. Only labels, read by the renderer's diagnostic; no behaviour depends on it.
inline const char* passActivity = "other";
// FM4 Recomp (FM4_DIAG_OVERWRITE, diagnostic): the tiles the last acquireTargets transferred into the draw's targets (0: none); the
// renderer prints the draw that follows a transfer, once per signature, to find clear-style draws that overwrite what was moved.
inline uint32_t lastBindTransferTiles = 0;
// FM4 Recomp (M7 GPU cost, FM4_TRANSFER_SKIP_UNWRITTEN): the EDRAM unit's running count of reclaims taken without a transfer
// (edram::Ownership::Skipped); null when the skip is off. The renderer prints the difference on each swap line.
inline const edram::Ownership::SkipStats* transferSkipStats = nullptr;
// FM4 Recomp (FM4_DIAG_TRANSFERS=2, diagnostic): the draw about to acquire its targets (shader hashes, primitive, depth/stencil and
// colour write state), filled by the renderer only at that level; the EDRAM unit names it as the first consumer of the tiles the
// last transfers moved into a target the draw binds. Empty otherwise.
inline char diagDrawSignature[192] = "";
// FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): since the renderer's last swap line, the ownership transfers the EDRAM unit
// recorded as a clear of the destination instead of a copy (edram.h Transfer::value), and their tiles; the renderer prints and clears them.
inline uint64_t clearValueTransfersSinceSwap = 0, clearValueTilesSinceSwap = 0;
// FM4 Recomp (FM4_PARTIAL_RETIRE): the serial of the submission part being recorded (the renderer bumps it at each submit); a
// resolve recorded now, and the guest ranges it will write, carry it (edram_vk.cpp Pending, texture_upload.h GuestWriteLog).
inline uint64_t currentPartSerial = 1;
struct PassActivity
{
    const char* saved;
    explicit PassActivity(const char* activity) : saved(passActivity) { passActivity = activity; }
    ~PassActivity() { passActivity = saved; }
    PassActivity(const PassActivity&) = delete;
    PassActivity& operator=(const PassActivity&) = delete;
};
// FM4 Recomp (FM4_DIAG_PASS_TIMES, diagnostic): a unit's own render pass (an EDRAM transfer, clear, resolve dump or resident
// conversion) is timed on the GPU under `kind` when the renderer registers these; null in the units' tests.
inline void (*beginUnitPass)(VkCommandBuffer, const char* kind) = nullptr;
inline void (*endUnitPass)(VkCommandBuffer) = nullptr;
inline void UnitPassBegin(VkCommandBuffer c, const char* kind)
{
    if (beginUnitPass != nullptr)
        beginUnitPass(c, kind);
}
inline void UnitPassEnd(VkCommandBuffer c)
{
    if (endUnitPass != nullptr)
        endUnitPass(c);
}
inline void OutsideRenderPass(VkCommandBuffer c)
{
    if (endOpenRenderPass != nullptr)
        endOpenRenderPass(c);
}

struct EdramHooks
{
    // Before a Draw / DepthOnly draw is recorded: the host targets of `target` for the attachments the
    // draw uses (`needColor`: DrawKind::Draw with a colour write; `needDepth`: a depth or stencil test).
    // False with `error` refuses the draw (the renderer stops with it).
    // FM4 Recomp (FM4_OVERWRITE_CLAIM): `depthCutout` / `colorCutout` (null: none) are the rectangles the draw overwrites (edram::Bind).
    // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): `clear` (null: a drawn draw) is the clear the draw stands for (ClearRequest above).
    bool (*acquireTargets)(VkCommandBuffer commands, const RenderTarget& target, bool needColor, bool needDepth, const edram::Rect* depthCutout,
                           const edram::Rect* colorCutout, const ClearRequest* clear, HostTargets& out, std::string& error);
    // A DrawKind::Resolve draw (RB_MODECONTROL 6, X xenos.h:926-960): the EDRAM unit decodes RB_COPY_*
    // from `base`'s register file and records the resolve into `commands` (D-f; the M4 unit takes the
    // fallback: a copy of the rectangle into its staging buffer, written into guest memory by `retired`).
    // `d` carries the packet and the edram mode only (draw_state.cpp returns before the rest). The
    // renderer flushes the slot after every resolve (renderer.cpp FlushSlot).
    bool (*resolve)(VkCommandBuffer commands, const uint8_t* base, const DrawState& d, uint64_t draw, std::string& error);
    // From RetireSlot, after the slot's submission was confirmed complete (GpuGuard::Retire) and before
    // GuestWritesRetired (guest_writes.h): the EDRAM unit writes that submission's read-back resolves into
    // guest memory. False with `error` stops. FM4_PARTIAL_RETIRE: only the resolves recorded by parts up to
    // `upToSerial` (UINT64_MAX: every pending one, the whole slot).
    bool (*retired)(uint64_t upToSerial, std::string& error);
    // Renderer shutdown, only after GpuGuard::Shutdown kept nothing and after the renderer destroyed its
    // framebuffers (they reference the EDRAM unit's views): destroys every resource of the unit.
    void (*shutdown)();
    // FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): the resolve `resolve` just recorded, as the texture unit sees it
    // (false: it copied nothing), and when `out.converted` the unit's image holding its converted words (1x R32_UINT, in
    // TRANSFER_SRC_OPTIMAL, the rectangle at (0, 0)), valid in `commands` until the next resolve is recorded.
    bool (*lastResolve)(ResidentResolve& out, VkImage& words);
    // FM4 Recomp (FM4_RESOLVE_RESIDENT): whether one more resolve fits the staging before the open submission retires (the
    // EDRAM unit then keeps several read-back resolves pending; otherwise one).
    bool (*resolveRoom)();
    // FM4 Recomp (FM4_DIAG_TRANSFERS, diagnostic): prints and clears the unit's tallies for swaps [first, last]; null when off.
    void (*diagTally)(uint64_t firstSwap, uint64_t lastSwap);
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
    uint64_t (*stagingBytes)(const TextureSlot& slot, const uint8_t* base);  // base: guest memory (the reuse hash, texture_upload.h ReservedStagingBytes)
    // FM4 Recomp (FM4_RESOLVE_DEFER): whether the slot's texture reads a range a recorded resolve has not yet written back.
    // FM4_RESOLVE_RESIDENT: false for an image that already holds the resolves' words (adoptResolve).
    bool (*readsPendingWrite)(const TextureSlot& slot);
    // FM4_PARTIAL_RETIRE: the newest part whose pending resolve the texture reads (0: none; readsPendingWrite false means 0).
    uint64_t (*pendingWriteSerial)(const TextureSlot& slot);
    // FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): after every recorded resolve. Copies the resolve's converted words
    // (`words`, EdramHooks::lastResolve) into each host image that takes them verbatim and is otherwise in step with guest
    // memory; every other image over the resolve's range stops being resident. Returns the images written.
    uint32_t (*adoptResolve)(VkCommandBuffer commands, const ResidentResolve& resolve, VkImage words);
};

}  // namespace runtime::video
