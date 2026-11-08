// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The Vulkan EDRAM unit (execution model unit 6, render_targets_vk.h's plan executed for the M4 scope): the
// renderer's EdramHooks (render_hooks.h) on top of the CPU model (edram.h). One VkImage per edram::Key, created
// on first use and held for the run; edram::Ownership's transfers executed as fullscreen-triangle draws with a
// built-in GLSL shader (compiled once at InitializeEdramVk through the renderer's glslang compiler); resolves by
// the fallback path (render_targets_vk.h plan 6): a copy into a persistent staging buffer, converted on the CPU
// into tiled guest memory after the slot's completion is confirmed (EdramHooks::retired), then the resolve's
// clears with vkCmdClearAttachments. Everything not covered is refused with its name (edram_vk_plan.h
// ClassifyTransfer, ResolveConvertible, the model's own refusals) and the renderer stops.
//
// Lifetime: every image, view, framebuffer, pipeline, descriptor and the staging buffer lives for the run, like
// the renderer's display image and readback buffer; nothing is destroyed before EdramHooks::shutdown, which the
// renderer calls only after GpuGuard::Shutdown kept nothing. The staging buffer is written by at most one
// submission at a time: the renderer flushes the slot (renderer.cpp FlushSlot) after every resolve, and a second resolve in
// one submission is refused.
//
// Layouts: every image rests in its attachment layout (COLOR_ATTACHMENT_OPTIMAL or
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL, the renderer's render passes' initial = final layout) whenever a hook returns;
// each operation records an explicit barrier from the tracked layout to the one it needs and back.
#pragma once
#include "render_hooks.h"
#include "shader_cache.h"
#include "vulkan_device.h"
#include <cstdint>
#include <string>

namespace runtime::video {

// Told about every guest range a resolve will write, when the resolve is RECORDED (the renderer passes
// guest_writes.h MarkGuestWritten: the texture unit marks the range stale and pending until GuestWritesRetired).
using ResolveWriteObserver = void (*)(uint32_t physical, uint32_t bytes);

// Creates the unit's device objects (sampler, layouts, shader modules, staging buffer) and checks the formats and
// sample counts it needs. Null with `error` on any failure (the renderer stops). The returned hooks stay valid
// until their shutdown hook ran. `compiler` is used during this call only. `guestBase`: main.cpp's writable guest
// memory base (renderer.h Options::guestBase); the resolve hook refuses a CP base that differs from it, and the
// read-back resolves are written through it (the CP hands the renderer a const base). `observer` must not be null.
const EdramHooks* InitializeEdramVk(const VulkanDevice& device, SpirvCompiler& compiler, uint8_t* guestBase, ResolveWriteObserver observer,
                                    std::string& error);


// The isolated GPU check of the 4x -> 2x depth transfer (renderer.cpp RunSelfCheck with FM4_SELFCHECK_EDRAM=depth4x2x;
// expectations edram_vk_plan.h DepthSelfCheck*): RecordDepthSelfCheck records into `commands`, after InitializeEdramVk,
// an upload of known depth and stencil into the 1x kD24FS8 key at base 1024 (16 tiles a row, 1280 wide, 32 rows),
// its transfer into the 4x key 708400 and from there into the 2x key 688400 (tiles 1024-1055), and the depth dump of
// both (640x16 and 1280x16 pixels, every guest sample) into the staging buffer. After that submission is confirmed
// complete, VerifyDepthSelfCheck compares every dumped word with its expectation.
constexpr uint32_t kDepthSelfCheckWidth = 1280, kDepthSelfCheckRows1x = 32;
constexpr uint64_t kDepthSelfCheckDump4x = 1ull << 20, kDepthSelfCheckDump2x = 2ull << 20;  // staging offsets (the upload is below 1 MiB)
struct DepthSelfCheckResult
{
    uint64_t checked4x = 0, checked2x = 0, mismatches4x = 0, mismatches2x = 0;
    std::string first;  // the first mismatch, if any
};
bool RecordDepthSelfCheck(VkCommandBuffer commands, std::string& error);
DepthSelfCheckResult VerifyDepthSelfCheck();

}  // namespace runtime::video
