// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The texture unit on the renderer's device (M4 unit 7): TextureHooks::bind (render_hooks.h) over the CPU
// texture unit (texture_upload.h: description, validity, staging; texture_info.h, texture_untile.h,
// texture_cache.h), and the guest-write contract (guest_writes.h) the EDRAM unit calls. One host image per
// TextureKey (the storage fields), reloaded in place by a recorded buffer-to-image copy from the slot's ring
// (renderer.cpp Slot::ring), so no image is ever destroyed while a submission may use it: images, views
// and samplers live for the run, at most kMaxTextureImages / kMaxTextureImageBytes (exceeding them refuses
// the draw, named). Everything runs on the command processor's thread.
#pragma once
#include "render_hooks.h"
#include "vulkan_device.h"
#include <string>

namespace runtime::video {

class GlslangCompiler;

// At renderer Initialize, after the device exists: logs the features the unit depends on and returns the
// hooks to register (SetTextureHooks). FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): `compiler` builds the untile pipeline (used only here).
const TextureHooks* InitializeTextureUnit(const VulkanDevice& device, GlslangCompiler* compiler);
// At renderer Shutdown, only after GpuGuard::Shutdown kept nothing (every submission confirmed complete) and
// before DestroyVulkanDevice: destroys the views, samplers, images and their memory.
void ShutdownTextureUnit();
// One line for the renderer's [gpu] swap log (counts since the previous call, and totals).
std::string TextureUnitSummary();
// FM4 Recomp: at each swap, only after its RetireSlot (every submission confirmed complete): destroys idle images above the
// thresholds (texture_vk.cpp kEvictAbove).
void EvictIdleTextures();
// FM4 Recomp (diagnostic, FM4_DIAG_TRIGGER stale-textures; every submission confirmed complete): every non-resident image is re-hashed
// at its next bind (re-uploaded if its guest bytes differ). Returns how many.
size_t MarkTexturesStale();
// FM4 Recomp (M9, live resolution, step 2): at a change of the internal scale, only with every submission confirmed complete and no
// command buffer open: destroys every scaled twin (texture_vk.cpp HostImage::hiImage) and its views. Returns how many.
size_t DestroyTextureTwins();

}  // namespace runtime::video
