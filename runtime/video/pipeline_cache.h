// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The Vulkan side of the pipeline cache: the one descriptor set layout and pipeline layout of the
// translator's binding contract (shader_glsl.h, pipeline_key.h DescriptorBinding), shader modules from
// the shader cache's SPIR-V, render passes compatible with a key's attachments, and one VkPipeline
// per PipelineKey, created synchronously at its first use (execution model §2: no async pipeline
// path for M4). The CPU-only derivation lives in pipeline_key.{h,cpp}; this file only maps the key
// to Vulkan structures, after Xenia's vulkan_pipeline_cache.cc:1980-2275 (67f16c4e315f).
//
// Safety: the class allow-list (shader_class.h) is checked in Get before vkCreateGraphicsPipelines,
// because MoltenVK compiles the Metal shaders there. Every object is created once and destroyed only
// in the destructor, which the renderer runs from Shutdown after GpuGuard::Shutdown kept nothing
// (no pipeline is ever destroyed while a submission that may use it is in flight: GPU
// safety). Nothing here submits or records.
#pragma once
#include "pipeline_key.h"
#include "shader_cache.h"
#include "shader_class.h"
#include <cstdint>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace runtime::video {

// The device features the key may need (queried by the renderer from VkPhysicalDeviceFeatures and
// VK_KHR_portability_subset; never assumed).
struct PipelineDeviceFeatures
{
    bool depthClamp = false;                      // VkPhysicalDeviceFeatures::depthClamp (key.depthClamp)
    bool constantAlphaColorBlendFactors = true;   // VkPhysicalDevicePortabilitySubsetFeaturesKHR; false refuses colour factors 14, 15
    bool independentBlend = false;                // unused while colour 0 is the only target
};

// The host formats of the Xenos render target formats pipeline_key.h ColorFormatMapped accepts.
// 1 (8_8_8_8_GAMMA) is UNORM with the shader's XE_COLOR0_GAMMA encode; 3 (2_10_10_10_FLOAT) is
// RGBA16 float with the shader's XE_COLOR0_7E3 clamp (shader_glsl.h). VK_FORMAT_UNDEFINED for the rest.
VkFormat HostColorFormat(uint32_t xenosFormat);
// D32_SFLOAT_S8_UINT for both D24S8 (0) and D24FS8 (1): MoltenVK on Apple GPUs has no
// D24_UNORM_S8_UINT (Metal's Depth24Unorm_Stencil8 is Intel/AMD only); the precision difference
// and the 20e4 float depth are APPROXIMATE.
VkFormat HostDepthFormat(uint32_t xenosDepthFormat);

class PipelineCache
{
public:
    // Creates the descriptor set layout and the pipeline layout. `result` false (with the VkResult
    // in the reason) means nothing usable was created and the renderer stops.
    PipelineCache(VkDevice device, const PipelineDeviceFeatures& features, ShaderClass allowedClasses, KeyResult& result);
    ~PipelineCache();  // only after GpuGuard::Shutdown kept nothing
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    VkDescriptorSetLayout SetLayout() const { return setLayout_; }
    VkPipelineLayout Layout() const { return layout_; }

    struct Entry
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
    };
    // The pipeline of `key`, created at first use from the variants' SPIR-V (modules created at
    // their first use too). Refuses, before any Vulkan call: a class the allow-list does not cover
    // (naming both hashes and the classes); depth clamp without the feature; constant-alpha colour
    // factors without the portability feature; a Vulkan creation failure (its VkResult).
    KeyResult Get(const PipelineKey& key, const CachedShader& vs, ShaderVariant& vsVariant, const CachedShader& ps, ShaderVariant& psVariant,
                  Entry& out);

    struct Stats
    {
        uint64_t pipelines = 0, renderPasses = 0, modules = 0, hits = 0, classRefusals = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    KeyResult Module(const CachedShader& shader, ShaderVariant& variant);
    KeyResult RenderPass(const PipelineKey& key, VkRenderPass& out);

    VkDevice device_;
    PipelineDeviceFeatures features_;
    ShaderClass allowed_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, Entry, PipelineKey::Hasher> pipelines_;
    std::unordered_map<uint64_t, VkRenderPass> renderPasses_;  // by the attachment part of the key
    std::vector<VkShaderModule> modules_;                       // every module created, for the destructor
    Stats stats_;
};

}  // namespace runtime::video
