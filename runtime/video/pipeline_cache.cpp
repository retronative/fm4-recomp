// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See pipeline_cache.h. Enum mappings: Xenia vulkan_pipeline_cache.cc (67f16c4e315f) kBlendFactorMap
// :2169-2184, kBlendOpMap :2186-2194, VK_COMPARE_OP_NEVER + CompareFunction :2128, VK_STENCIL_OP_KEEP +
// StencilOp :2134, topology :1994-2032, cull/front :2073-2082, the blend-enable rule :2203-2209
// (enabled unless (one, zero, add) for colour and alpha), dynamic states :2260-2272.
#include "pipeline_cache.h"
#include "diag_compile.h"  // FM4 Recomp: compile time beside a long swap (diagnostic)
#include "pipeline_cache_header.h"  // FM4 Recomp (M7 stability): FM4_SHADER_CACHE
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include "push_template.h"  // FM4 Recomp (FM4_PUSH_TEMPLATE)

#include <cstdio>
#include <cstdlib>  // FM4 Recomp: getenv (FM4_PIPELINE_WORD_HASH)
#include <cstring>
#include <iterator>

namespace runtime::video {

namespace {

VkBlendFactor BlendFactor(uint8_t xenos)
{
    switch (xenos)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_MAX_ENUM;  // DerivePipelineKey refused these
    }
}

VkBlendOp BlendOp(uint8_t xenos)
{
    static const VkBlendOp ops[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
    return xenos < 5 ? ops[xenos] : VK_BLEND_OP_MAX_ENUM;
}

VkPrimitiveTopology HostTopology(uint8_t t)
{
    switch (Topology(t))
    {
    case Topology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Topology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Topology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

VkStencilOpState StencilState(const uint8_t ops[4])
{
    VkStencilOpState s{};
    s.failOp = VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + ops[0]);
    s.passOp = VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + ops[1]);
    s.depthFailOp = VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + ops[2]);
    s.compareOp = VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + ops[3]);
    return s;  // reference and masks are dynamic state
}

std::string VkError(const char* what, VkResult r) { return std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")"; }

}  // namespace

VkFormat HostColorFormat(uint32_t xenosFormat)
{
    switch (xenosFormat)
    {
    case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;
    case 2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3: case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 4: return VK_FORMAT_R16G16_SNORM;  // 16_16 (edram_vk_plan.cpp HostColorFormat)
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;  // FM4 Recomp (alpha, RT format 5): 16_16_16_16 (edram_vk_plan.cpp HostColorFormat)
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat HostDepthFormat(uint32_t xenosDepthFormat)
{
    (void)xenosDepthFormat;
    return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// FM4 Recomp (M7 CPU cost): opt-in FM4_PIPELINE_WORD_HASH=1 (pipeline_key.h WordHash), read once.
namespace {
bool PipelineWordHash()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PIPELINE_WORD_HASH");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] pipeline word hash on (FM4_PIPELINE_WORD_HASH=1): the pipeline map hashes its 96-byte keys a word at a time\n");
        return enabled;
    }();
    return on;
}
}  // namespace

PipelineCache::PipelineCache(VkDevice device, const PipelineDeviceFeatures& features, ShaderClass allowedClasses, KeyResult& result)
    : device_(device), features_(features), allowed_(allowedClasses), pipelines_(0, KeyHasher{PipelineWordHash()})
{
    Layouts none;
    result = LayoutFor(0, none);
}

PipelineCache::~PipelineCache()
{
    if (cache_ != VK_NULL_HANDLE)
        vkDestroyPipelineCache(device_, cache_, nullptr);
    for (auto& [key, e] : pipelines_)
        vkDestroyPipeline(device_, e.pipeline, nullptr);
    for (auto& [key, pass] : renderPasses_)
        vkDestroyRenderPass(device_, pass, nullptr);
    for (VkShaderModule m : modules_)
        vkDestroyShaderModule(device_, m, nullptr);
    for (auto& [key, l] : layouts_)
    {
        if (l.pushTemplate != VK_NULL_HANDLE)  // FM4_PUSH_TEMPLATE: before the layouts it was created with
            vkDestroyDescriptorUpdateTemplate(device_, l.pushTemplate, nullptr);
        vkDestroyPipelineLayout(device_, l.pipeline, nullptr);
        vkDestroyDescriptorSetLayout(device_, l.set, nullptr);
    }
}

void PipelineCache::UsePersistentCache(const std::string& file, uint32_t vendorID, uint32_t deviceID, const uint8_t (&uuid)[16])
{
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(file.c_str(), "rb"))
    {
        uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
            data.insert(data.end(), chunk, chunk + n);
        std::fclose(f);
    }
    pipeline_cache::Maker device{vendorID, deviceID, {}};
    std::memcpy(device.uuid.data(), uuid, device.uuid.size());
    const pipeline_cache::HeaderCheck check = pipeline_cache::CheckHeader(data.data(), data.size(), device);
    if (const std::string line = pipeline_cache::LogLine(check, device); !line.empty())
    {
        std::fprintf(stderr, "%s\n", line.c_str());
        data.clear();
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &cache_) != VK_SUCCESS)
    {
        std::fprintf(stderr, "[gpu] pipeline cache: the driver refused %zu KB from %s: starting empty\n", data.size() / 1024, file.c_str());
        data.clear();
        info.initialDataSize = 0, info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device_, &info, nullptr, &cache_) != VK_SUCCESS)
        {
            cache_ = VK_NULL_HANDLE;
            std::fprintf(stderr, "[gpu] pipeline cache: vkCreatePipelineCache failed: pipelines are created without a cache\n");
            return;
        }
    }
    cacheFile_ = file;
    std::fprintf(stderr, "[gpu] pipeline cache on (FM4_SHADER_CACHE): %zu KB loaded from %s\n", data.size() / 1024, file.c_str());
}

bool PipelineCache::SavePersistentCache()
{
    static std::mutex saveLock;
    if (!PersistentCacheOn())
        return false;
    std::lock_guard guard(saveLock);
    size_t size = 0;
    std::vector<uint8_t> data;
    for (int attempt = 0;; attempt++)  // pipelines may be added between the two calls (VK_INCOMPLETE)
    {
        if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS || size == 0)
            return false;
        data.resize(size);
        const VkResult r = vkGetPipelineCacheData(device_, cache_, &size, data.data());
        if (r == VK_SUCCESS)
            break;
        if (r != VK_INCOMPLETE || attempt == 3)
            return false;
    }
    const std::string temporary = cacheFile_ + ".tmp" + std::to_string(getpid());
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool written = std::fwrite(data.data(), 1, size, f) == size;
    if (std::fclose(f) != 0 || !written || std::rename(temporary.c_str(), cacheFile_.c_str()) != 0)
        return std::remove(temporary.c_str()), false;
    return true;
}

KeyResult PipelineCache::LayoutFor(uint64_t layoutKey, Layouts& out)
{
    if (const auto it = layouts_.find(layoutKey); it != layouts_.end())
        return out = it->second, Accept();
    // Bindings 0-5 as shader_glsl.h declares them; 8 + slot for each slot of the key, visible to the stages that fetch it
    // (the sampler type, 2D/3D/cube, is the image view's).
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    const struct { uint32_t binding; VkDescriptorType type; VkShaderStageFlags stages; } fixed[6] = {
        {kBindingVertexData, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},
        {kBindingSystem, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},
        {kBindingVsConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},
        {kBindingPsConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {kBindingIndices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},
        {kBindingErrors, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT},
    };
    for (const auto& f : fixed)  // FM4 Recomp (FM4_DESCRIPTOR_CACHE): dynamic, the ring offsets given at each bind
        bindings.push_back({f.binding, !features_.dynamicBuffers ? f.type
                                       : f.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                            1, f.stages, nullptr});
    const uint32_t vsSlots = LayoutVsSlots(layoutKey), psSlots = LayoutPsSlots(layoutKey);
    for (uint32_t slot = 0; slot < kTextureBindings; slot++)
    {
        const VkShaderStageFlags stages = (vsSlots >> slot & 1 ? VK_SHADER_STAGE_VERTEX_BIT : 0u) | (psSlots >> slot & 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u);
        if (stages != 0)
            bindings.push_back({kBindingTexture0 + slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr});
    }
    Layouts l;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = uint32_t(bindings.size());
    setInfo.pBindings = bindings.data();
    // FM4 Recomp (FM4_PUSH_DESCRIPTORS): every binding is one descriptor, so the set's descriptor count is its binding count
    // (VUID-VkDescriptorSetLayoutCreateInfo-flags-00281: at most maxPushDescriptors); dynamic buffers are never pushed (-00280).
    if (features_.pushDescriptorLimit != 0 && !features_.dynamicBuffers && bindings.size() <= features_.pushDescriptorLimit)
        setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, l.push = true;
    if (const VkResult r = vkCreateDescriptorSetLayout(device_, &setInfo, nullptr, &l.set); r != VK_SUCCESS)
        return Refuse(VkError("vkCreateDescriptorSetLayout", r));
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &l.set;
    if (const VkResult r = vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &l.pipeline); r != VK_SUCCESS)
    {
        vkDestroyDescriptorSetLayout(device_, l.set, nullptr);  // never used by any pipeline or set
        return Refuse(VkError("vkCreatePipelineLayout", r));
    }
    l.layoutKey = layoutKey;
    // FM4 Recomp (M7 CPU cost, FM4_PUSH_TEMPLATE): the push layout's template, one entry per binding declared above, in the order and
    // with the types of `bindings` (push_template.h PlanPushTemplate gives the same bindings: 0-5, then 8 + each slot of the key in
    // ascending order). VUID-vkCmdPushDescriptorSetWithTemplate-layout-07993 / -set-07995: the renderer pushes with l.pipeline, set 0.
    if (l.push && features_.pushTemplates)
    {
        const PushTemplatePlan plan = PlanPushTemplate(layoutKey, sizeof(VkDescriptorBufferInfo), sizeof(VkDescriptorImageInfo));
        if (plan.count != bindings.size())
        {
            vkDestroyPipelineLayout(device_, l.pipeline, nullptr), vkDestroyDescriptorSetLayout(device_, l.set, nullptr);  // never used
            return Refuse("push template: " + std::to_string(plan.count) + " entries for a layout of " + std::to_string(bindings.size()) + " bindings");
        }
        VkDescriptorUpdateTemplateEntry entries[kPushBufferBindings + kTextureBindings];
        for (uint32_t i = 0; i < plan.count; i++)
        {
            const PushTemplateEntry& e = plan.entries[i];
            if (e.binding != bindings[i].binding || (e.image != (bindings[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)))
            {
                vkDestroyPipelineLayout(device_, l.pipeline, nullptr), vkDestroyDescriptorSetLayout(device_, l.set, nullptr);
                return Refuse("push template: entry " + std::to_string(i) + " (binding " + std::to_string(e.binding) + ") is not the layout's binding " +
                              std::to_string(bindings[i].binding));
            }
            entries[i] = {e.binding, 0, 1, bindings[i].descriptorType, e.offset, e.stride};
        }
        VkDescriptorUpdateTemplateCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
        info.descriptorUpdateEntryCount = plan.count;
        info.pDescriptorUpdateEntries = entries;
        info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
        info.descriptorSetLayout = l.set;  // ignored for a push template
        info.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        info.pipelineLayout = l.pipeline;
        info.set = 0;
        if (const VkResult r = vkCreateDescriptorUpdateTemplate(device_, &info, nullptr, &l.pushTemplate); r != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(device_, l.pipeline, nullptr), vkDestroyDescriptorSetLayout(device_, l.set, nullptr);
            return Refuse(VkError("vkCreateDescriptorUpdateTemplate", r));
        }
    }
    layouts_[layoutKey] = l;
    stats_.layouts++;
    out = l;
    return Accept();
}

KeyResult PipelineCache::Module(const CachedShader& shader, ShaderVariant& variant)
{
    if (variant.module != 0)
        return Accept();
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = variant.spirv.size() * sizeof(uint32_t);
    info.pCode = variant.spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateShaderModule(device_, &info, nullptr, &module); r != VK_SUCCESS)
    {
        char hash[17];
        std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(shader.hash));
        return Refuse(VkError("vkCreateShaderModule", r) + " for " + (shader.pixel ? "ps_" : "vs_") + hash);
    }
    modules_.push_back(module);
    variant.module = uint64_t(reinterpret_cast<uintptr_t>(module));
    stats_.modules++;
    return Accept();
}

KeyResult PipelineCache::RenderPass(const PipelineKey& key, VkRenderPass& out)
{
    // The attachment part of the key: formats and presence. Load and store everything (the targets
    // persist across draws; the EDRAM core of unit 6 owns their contents).
    const uint64_t passKey = uint64_t(key.colorUsed) | uint64_t(key.color[0].format) << 8 | uint64_t(key.depthUsed) << 16 |
                             uint64_t(key.depthFormat) << 24 | uint64_t(key.samples) << 32;
    if (const auto it = renderPasses_.find(passKey); it != renderPasses_.end())
        return out = it->second, Accept();
    VkAttachmentDescription attachments[2]{};
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t count = 0;
    if (key.colorUsed)
    {
        VkAttachmentDescription& a = attachments[count];
        a.format = HostColorFormat(key.color[0].format);
        a.samples = VkSampleCountFlagBits(key.samples);
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRef.attachment = count++;
    }
    if (key.depthUsed)
    {
        VkAttachmentDescription& a = attachments[count];
        a.format = HostDepthFormat(key.depthFormat);
        a.samples = VkSampleCountFlagBits(key.samples);
        a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef.attachment = count++;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = key.colorUsed ? 1 : 0;
    subpass.pColorAttachments = key.colorUsed ? &colorRef : nullptr;
    subpass.pDepthStencilAttachment = key.depthUsed ? &depthRef : nullptr;
    // One render pass per draw, each loading what the previous one stored (renderer.cpp:Draw): the
    // external dependencies order consecutive passes on the same attachments (attachment writes before
    // the next pass's loads and tests) and the EDRAM unit's transfers around them (render_hooks.h).
    const VkPipelineStageFlags attachmentStages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = attachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = attachmentStages;
    dependencies[0].srcAccessMask = attachmentAccess | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = attachmentAccess;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = attachmentStages;
    dependencies[1].dstStageMask = attachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = attachmentAccess;
    dependencies[1].dstAccessMask = attachmentAccess | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateRenderPass(device_, &info, nullptr, &pass); r != VK_SUCCESS)
        return Refuse(VkError("vkCreateRenderPass", r));
    renderPasses_[passKey] = pass;
    stats_.renderPasses++;
    out = pass;
    return Accept();
}

KeyResult PipelineCache::Get(const PipelineKey& key, const CachedShader& vs, ShaderVariant& vsVariant, const CachedShader& ps,
                             ShaderVariant& psVariant, Entry& out)
{
    if (const auto it = pipelines_.find(key); it != pipelines_.end())
        return stats_.hits++, out = it->second, Accept();
    // The isolation rule, before anything reaches the driver (MoltenVK compiles the MSL in
    // vkCreateGraphicsPipelines).
    if (const ShaderClass refused = RefusedClasses(vs.cls, ps.cls, allowed_); refused != 0)
    {
        stats_.classRefusals++;
        char vsHex[17], psHex[17];
        std::snprintf(vsHex, sizeof vsHex, "%016llx", static_cast<unsigned long long>(vs.hash));
        std::snprintf(psHex, sizeof psHex, "%016llx", static_cast<unsigned long long>(ps.hash));
        return Refuse("shader class " + ShaderClassName(refused) + " is not in --shader-classes (vs_" + vsHex + " is " + ShaderClassName(vs.cls) + ", ps_" +
                      psHex + " is " + ShaderClassName(ps.cls) + "); a never-seen class runs first in its own approved run");
    }
    // The key's sampler layout must be the pair's own (PlanDescriptors' layoutKey): a key built without it would bind a
    // layout that lacks the shaders' sampler bindings (invalid Vulkan), so it is refused before any Vulkan call.
    {
        uint32_t vsSlots = 0, psSlots = 0;
        for (const uint32_t slot : vs.t.textureSlots)
            vsSlots |= slot < kTextureBindings ? 1u << slot : 0u;
        for (const uint32_t slot : ps.t.textureSlots)
            psSlots |= slot < kTextureBindings ? 1u << slot : 0u;
        if (key.samplerLayout != SamplerLayoutKey(vsSlots, psSlots))
            return Refuse("the pipeline key's sampler layout " + std::to_string(key.samplerLayout) + " is not the pair's (" +
                          std::to_string(SamplerLayoutKey(vsSlots, psSlots)) + "): the key was built without PlanDescriptors");
    }
    if (key.depthClamp && !features_.depthClamp)
        return Refuse("PA_CL_CLIP_CNTL clip_disable needs the device's depthClamp feature, which it lacks");
    if (key.colorUsed && !features_.constantAlphaColorBlendFactors)
        for (const uint8_t f : {key.color[0].srcColor, key.color[0].dstColor})
            if (f == 14 || f == 15)
                return Refuse("a constant-alpha colour blend factor needs constantAlphaColorBlendFactors (VK_KHR_portability_subset), which the device lacks");
    if (KeyResult r = Module(vs, vsVariant); !r)
        return r;
    if (KeyResult r = Module(ps, psVariant); !r)
        return r;
    Entry e;
    if (KeyResult r = LayoutFor(key.samplerLayout, e.layouts); !r)
        return r;
    if (KeyResult r = RenderPass(key, e.renderPass); !r)
        return r;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = reinterpret_cast<VkShaderModule>(uintptr_t(vsVariant.module));
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = reinterpret_cast<VkShaderModule>(uintptr_t(psVariant.module));
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo input{};  // empty: the shader pulls its vertices (pipeline_key.h)
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = HostTopology(key.topology);
    // Metal cannot disable primitive restart on strips (MoltenVK 1.4.2 warns VK_ERROR_FEATURE_NOT_PRESENT),
    // so strip pipelines always enable it. Exact: restart fires only on index 0xFFFFFFFF, which the host emits
    // only for a guest reset index (draw_record.cpp BuildHostIndices; every other index is masked to 24 bits; auto-index
    // draws are non-indexed or use generated small indices). Lists keep it off, as Vulkan requires.
    const bool strip = assembly.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assembly.primitiveRestartEnable = (key.primitiveRestart || strip) ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;  // dynamic
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = key.depthClamp ? VK_TRUE : VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = (key.cullFront ? VK_CULL_MODE_FRONT_BIT : 0u) | (key.cullBack ? VK_CULL_MODE_BACK_BIT : 0u);
    raster.frontFace = key.frontClockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = key.depthBias ? VK_TRUE : VK_FALSE;  // the values are dynamic
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);  // 1, 2 or 4 (KeyFromDrawState); the EDRAM unit's images match
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.depthUsed && (key.depthWrite || key.depthFunc != 7))
    {
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + key.depthFunc);
    }
    if (key.stencilEnable)
    {
        depthStencil.stencilTestEnable = VK_TRUE;
        depthStencil.front = StencilState(key.stencilFront);
        depthStencil.back = StencilState(key.stencilBack);
    }
    VkPipelineColorBlendAttachmentState attachment{};
    if (key.colorUsed)
    {
        const PipelineKeyColor& c = key.color[0];
        if (c.srcColor != 1 || c.dstColor != 0 || c.colorOp != 0 || c.srcAlpha != 1 || c.dstAlpha != 0 || c.alphaOp != 0)
        {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = BlendFactor(c.srcColor);
            attachment.dstColorBlendFactor = BlendFactor(c.dstColor);
            attachment.colorBlendOp = BlendOp(c.colorOp);
            attachment.srcAlphaBlendFactor = BlendFactor(c.srcAlpha);
            attachment.dstAlphaBlendFactor = BlendFactor(c.dstAlpha);
            attachment.alphaBlendOp = BlendOp(c.alphaOp);
        }
        attachment.colorWriteMask = VkColorComponentFlags(c.writeMask);
    }
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = key.colorUsed ? 1 : 0;
    blending.pAttachments = &attachment;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = uint32_t(std::size(dynamic));
    dynamicState.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamicState;
    info.layout = e.layouts.pipeline;
    info.renderPass = e.renderPass;
    const uint64_t createStart = diag::CompileClockNs();  // diag_compile.h (counted always; printed beside a long swap)
    const VkResult created = vkCreateGraphicsPipelines(device_, cache_, 1, &info, nullptr, &e.pipeline);
    diag::Compiles().pipelineNs.fetch_add(diag::CompileClockNs() - createStart, std::memory_order_relaxed);
    diag::Compiles().pipelines.fetch_add(1, std::memory_order_relaxed);
    if (created != VK_SUCCESS)
        return Refuse(VkError("vkCreateGraphicsPipelines", created));
    pipelines_[key] = e;
    stats_.pipelines++;
    out = e;
    return Accept();
}

}  // namespace runtime::video
