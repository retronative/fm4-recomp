// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7 CPU cost, opt-in FM4_PUSH_TEMPLATE=1 with FM4_PUSH_DESCRIPTORS=1): the descriptor update template of a push
// layout, so a draw pushes its set with one vkCmdPushDescriptorSetWithTemplateKHR and one block of data instead of 6 + n
// VkWriteDescriptorSet. Why: MoltenVK 1.4.2 (Commands/MVKCmdPipeline.mm MVKCmdPushDescriptorSet::setContent) deep-copies every
// write's pImageInfo / pBufferInfo with one `new[]` each and frees them in clearDescriptorWrites when the pooled command is reused;
// its MVKCmdPushDescriptorSetWithTemplate::setContent copies the template's data into a buffer it keeps (malloc only when it grows).
//
// The template describes exactly the writes Draw records for a push layout (renderer.cpp, step 8): bindings 0-5 (one buffer each,
// in binding order) and, for each texture slot of the layout key in ascending slot order, binding kBindingTexture0 + slot (one
// combined image sampler). The data block is the six VkDescriptorBufferInfo, then the image infos in the same order:
// Vulkan spec, VkDescriptorUpdateTemplateEntry (offset: "the offset in bytes of the first binding in the raw data structure";
// stride: "the stride in bytes between two consecutive array elements of the descriptor update informations in the raw data
// structure"). Vulkan-free (the CPU test builds it without vulkan.h): the info sizes are parameters; the renderer passes
// sizeof(VkDescriptorBufferInfo) / sizeof(VkDescriptorImageInfo) (24 / 24 on LP64: two handles and a VkDeviceSize; two handles and
// a VkImageLayout, padded to 8) and static_asserts its data block's layout against this plan.
#pragma once
#include "pipeline_key.h"
#include <cstdint>

namespace runtime::video {

constexpr uint32_t kPushBufferBindings = 6;  // kBindingVertexData .. kBindingErrors

struct PushTemplateEntry
{
    uint32_t binding = 0;
    bool image = false;   // a combined image sampler (else one buffer: storage 0, 4, 5; uniform 1, 2, 3)
    uint32_t offset = 0;  // in the data block
    uint32_t stride = 0;  // the info's size (one descriptor per entry; Vulkan needs the stride anyway)
};

struct PushTemplatePlan
{
    uint32_t count = 0;   // entries used
    uint32_t images = 0;  // texture bindings among them (the draw must bind exactly this many)
    uint32_t bytes = 0;   // the data block's size
    PushTemplateEntry entries[kPushBufferBindings + kTextureBindings];
};

// The template of the push layout `layoutKey` (pipeline_key.h SamplerLayoutKey): a slot is a binding when either stage fetches it,
// as PipelineCache::LayoutFor declares it.
constexpr PushTemplatePlan PlanPushTemplate(uint64_t layoutKey, uint32_t bufferInfoBytes, uint32_t imageInfoBytes)
{
    PushTemplatePlan p;
    for (uint32_t b = 0; b < kPushBufferBindings; b++)
        p.entries[p.count++] = {b, false, b * bufferInfoBytes, bufferInfoBytes};
    const uint32_t slots = LayoutVsSlots(layoutKey) | LayoutPsSlots(layoutKey);
    for (uint32_t s = 0; s < kTextureBindings; s++)
        if (slots >> s & 1)
            p.entries[p.count++] = {kBindingTexture0 + s, true, kPushBufferBindings * bufferInfoBytes + p.images++ * imageInfoBytes, imageInfoBytes};
    p.bytes = kPushBufferBindings * bufferInfoBytes + p.images * imageInfoBytes;
    return p;
}

// The opt-in (renderer.cpp, vulkan_device.cpp); read once.
bool PushTemplateAsked();

}  // namespace runtime::video
