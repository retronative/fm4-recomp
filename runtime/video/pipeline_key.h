// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The pipeline key and everything the pipeline cache derives from a draw's state on the CPU, free
// of Vulkan types so that tests/runtime/test_pipeline_cache.cpp covers it without vulkan.h:
//   - DrawStateView: the forward contract with the DrawState being drafted in parallel (the fields
//     this unit reads, by Xenos register, host order). Not implemented here.
//   - PipelineKey: the Vulkan pipeline state of a draw as a plain, zero-filled, memcmp-comparable
//     struct with a documented FNV-1a-64 hash. Equal states map to one key; irrelevant bits (a
//     depth function while the depth test is off, blend factors while the target's write mask is 0,
//     back-face stencil while backface_enable is 0) are normalized away, as Xenia does
//     (vulkan_pipeline_cache.cc:509-660 GetCurrentStateDescription at 67f16c4e315f).
//   - DerivePipelineKey: the state -> key, refusing (with a reason) what the host cannot render
//     faithfully. Every refusal is a strict stop in the renderer; none is a silent fallback.
//   - DeriveVariants: the host `#define`s each stage's shader needs for this draw (shader_glsl.h).
//   - VertexSlotPlan: the per-slot copy of guest vertex data the translator's pull contract needs.
//   - DescriptorPlan: the texture bindings of the pair, with the dimension checks shader_glsl.h asks.
//   - UploadRing: the per-slot bump allocator for the per-draw uniform and storage data.
//   - XeSystem / constants block layouts: byte offsets the uploads must match (shader_glsl.h).
//
// Vertex input (the brief's "vertex input bindings/attributes"): there is none. The translator's
// contract (shader_glsl.h bindings 0 and 1, kVertexHeader in shader_glsl.cpp: xe_vertex_word,
// xe_swap) pulls every vertex element from a storage buffer by the fetch constant's stride and
// offset and swaps the endian in the shader, as Xenia's SPIR-V translator does. So the key carries no vertex formats,
// and the per-draw work is a copy of each used slot's data (VertexSlotPlan) plus the xe_fetch /
// xe_vertex_base fill. Index buffers are swapped on the CPU into host order (vkCmdDrawIndexed).
//
// Register semantics: Xenia 67f16c4e315f registers.h (PA_SU_SC_MODE_CNTL :416, RB_SURFACE_INFO
// :618, RB_COLORCONTROL :630, RB_COLOR_INFO :684, RB_COLOR_MASK :702, RB_BLENDCONTROL :726,
// RB_DEPTHCONTROL :744, RB_STENCILREFMASK :767, RB_DEPTH_INFO :779, VGT_DRAW_INITIATOR :282) and
// xenos.h (PrimitiveType :41, Endian :193, MsaaSamples :288, ColorRenderTargetFormat :299,
// DepthRenderTargetFormat :406, CompareFunction :726, StencilOp :737, BlendFactor :749, BlendOp
// :768, FetchConstantType :1124, xe_gpu_vertex_fetch_t :1135, xe_gpu_texture_fetch_t :1206,
// TextureSign :121, DataDimension :97). Register indices: register_table.inc.
#pragma once
#include "shader_cache.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace runtime::video {

// ---- The forward contract with DrawState ----------------------------------------------------------

// What this unit reads of a draw. The DrawState drafted in parallel fills it from the register page
// (host order; floats as the register's IEEE bits reinterpreted) and the fetch / ALU / bool / loop
// constant ranges. Register indices (Xenia register_table.inc) are given per field.
struct DrawStateView
{
    // VGT_DRAW_INITIATOR (0x21FC): prim_type bits 0-5, num_indices bits 16-31; source select bits 6-7
    // (0 DMA = indexed, 2 auto-index = not indexed), index_size bit 11 (0: 16-bit, 1: 32-bit).
    uint32_t primitiveType = 0;
    uint32_t indexCount = 0;
    bool indexed = false, index32 = false;
    uint32_t indexEndian = 0;  // VGT_DMA_BASE low 2 bits / DRAW_INDX packet (xenos::Endian), for the host's index swap
    uint32_t vgtIndxOffset = 0, vgtMultiPrimIbResetIndx = 0;  // 0x2102, 0x2103 (24-bit fields)
    uint32_t paSuScModeCntl = 0;    // 0x2205
    uint32_t paClClipCntl = 0;      // 0x2204
    uint32_t paClVteCntl = 0;       // 0x2206
    float viewport[6] = {};         // 0x210F-0x2114: PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET}
    uint32_t windowScissorTl = 0, windowScissorBr = 0, windowOffset = 0;  // 0x2081, 0x2082, 0x2080
    uint32_t rbDepthControl = 0;    // 0x2200
    uint32_t rbStencilRefMask = 0, rbStencilRefMaskBf = 0;  // 0x210D, 0x210C
    uint32_t rbBlendControl[4] = {};  // 0x2201, 0x2209-0x220B
    float blendConstant[4] = {};      // 0x2105-0x2108 RB_BLEND_RED..ALPHA
    uint32_t rbColorControl = 0;    // 0x2202
    float alphaRef = 0.0f;          // 0x210E RB_ALPHA_REF
    uint32_t rbColorMask = 0;       // 0x2104
    uint32_t rbSurfaceInfo = 0;     // 0x2000
    uint32_t rbColorInfo[4] = {};   // 0x2001, 0x2003, 0x2005, 0x2007 (RB_COLOR[1-3]_INFO)
    uint32_t rbDepthInfo = 0;       // 0x2002
    uint32_t rbModeControl = 0;     // 0x2208 edram_mode (xenos::ModeControl: 0 ignore, 4 colour + depth, 5 depth, 6 copy = a resolve)
    uint32_t sqProgramCntl = 0;     // 0x2180
    // The fetch constants, registers 0x4800-0x48BF: 192 dwords; texture slot t = dwords 6t..6t+5,
    // vertex slot s = dwords 2s, 2s+1 (xe_gpu_fetch_group_t).
    const uint32_t* fetchConstants = nullptr;
    const float* aluConstants = nullptr;     // 0x4000-0x47FF: 512 float4 (VS 0-255, PS 256-511)
    const uint32_t* boolConstants = nullptr; // 0x4900-0x4907: 8 dwords
    const uint32_t* loopConstants = nullptr; // 0x4908-0x4927: 32 dwords
};

// ---- The key ------------------------------------------------------------------------------------

// Host topologies. Rect lists are triangle lists drawn with the vertex shader's XE_RECT_LIST variant
// (6 vertices per rect, shader_glsl.h), so they need no topology of their own.
enum class Topology : uint8_t
{
    PointList = 0,
    LineList = 1,
    LineStrip = 2,
    TriangleList = 3,
    TriangleStrip = 4,
};

struct PipelineKeyColor
{
    uint8_t format;     // RB_COLOR_INFO color_format (xenos::ColorRenderTargetFormat)
    uint8_t writeMask;  // RB_COLOR_MASK bits for the target (R 1, G 2, B 4, A 8)
    uint8_t srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp;  // xenos::BlendFactor / BlendOp; (1, 0, 0, 1, 0, 0) = off
};

struct PipelineKey
{
    uint64_t vsHash, psHash;
    uint64_t vsVariant, psVariant;  // ShaderVariantDefines::Id of each stage's variant
    uint64_t samplerLayout;         // DescriptorPlan::layoutKey: the pair's sampler bindings and their stages (the pipeline layout)
    uint8_t topology;               // Topology
    uint8_t primitiveRestart;       // strips only (PA_SU_SC_MODE_CNTL multi_prim_ib_ena)
    uint8_t cullFront, cullBack, frontClockwise;  // PA_SU_SC_MODE_CNTL bits 0, 1, 2
    uint8_t depthBias;              // poly_offset_front_enable | back << 1 (the values are dynamic state)
    uint8_t depthUsed;              // the pass has a depth/stencil attachment
    uint8_t depthFormat;            // RB_DEPTH_INFO depth_format (0 D24S8, 1 D24FS8)
    uint8_t depthWrite, depthFunc;  // normalized: (0, 7 = always) when z_enable is 0
    uint8_t stencilEnable;
    uint8_t stencilFront[4], stencilBack[4];  // fail, zpass, zfail (StencilOp), func (CompareFunction); 0 when off
    uint8_t samples;                // 1 (MSAA is refused until the EDRAM core, unit 6)
    uint8_t colorUsed;              // bit i: target i is attached (the shader exports colour 0 only, so 0 or 1)
    uint8_t depthClamp;             // PA_CL_CLIP_CNTL clip_disable (bit 16); needs the device's depthClamp feature
    uint8_t pad[2];
    PipelineKeyColor color[4];      // zero where colorUsed's bit is clear

    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
    bool operator!=(const PipelineKey& o) const { return !(*this == o); }
    // FNV-1a-64 over the struct's bytes in memory order (it is zero-filled, padding included).
    uint64_t Hash() const
    {
        const auto* b = reinterpret_cast<const unsigned char*>(this);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof *this; i++)
            h = (h ^ b[i]) * 0x100000001b3ull;
        return h;
    }
    struct Hasher
    {
        size_t operator()(const PipelineKey& k) const { return size_t(k.Hash()); }
    };
};
static_assert(std::is_trivially_copyable_v<PipelineKey>);
static_assert(sizeof(PipelineKey) == 96, "the key's layout is documented; a change needs the tests updated");

// A refusal names its field and value; the renderer stops with it.
struct KeyResult
{
    bool ok = false;
    std::string reason;
    explicit operator bool() const { return ok; }
};
inline KeyResult Refuse(std::string reason) { return {false, std::move(reason)}; }
inline KeyResult Accept() { return {true, {}}; }

inline uint32_t Field(uint32_t v, uint32_t low, uint32_t count) { return (v >> low) & ((1u << count) - 1); }

// The colour formats the host maps (pipeline_cache.cpp HostColorFormat); others are refused.
inline bool ColorFormatMapped(uint32_t format)
{
    switch (format)
    {
    case 0: case 1: case 2: case 3: case 4: case 7: case 14: case 15: return true;  // 8_8_8_8, _GAMMA, 2_10_10_10, _FLOAT, 16_16 (R16G16_SNORM), 16_16_16_16_FLOAT, 32_FLOAT, 32_32_FLOAT
    default: return false;  // 5 16_16_16_16, 6, 10, 12
    }
}

// FM4 Recomp: the colour exponent bias (RB_COLOR_INFO bits 20-25) the host accepts. Xenia scales the shader's colour by
// 2^bias, and on its host-render-target path by 2^(bias - 5) for 16_16 / 16_16_16_16, whose -32..32 range its SNORM target
// holds as -1..1 (vulkan_command_processor.cc:3755-3774; render_target_cache.cc:188 snorm16_render_target_full_range true).
// Only the case where that scale is 1 is accepted: 16_16 with bias 5 (FM4's intro, RB_COLOR_INFO
// 00540340), so no shader scale is emitted; any other bias is refused.
inline bool ColorExpBiasAccepted(uint32_t format, int32_t bias)
{
    return format == 4 ? bias == 5 : bias == 0;
}

// The pipeline state of a draw. `vs`/`ps`: the stage's current shaders; the variants (DeriveVariants)
// are the caller's, computed from the same state. Refusals (each with a negative control in the test):
// a primitive type the host has no topology for (fan, polygon, quads, line loop: ReCarbon converts
// indices on the CPU, not adopted yet; 7 triangle-with-w-flags, 0 none, 9-11 unused);
// poly_mode dual with a line or point polygon type on an unculled face; MSAA (RB_SURFACE_INFO
// msaa_samples != 0); an edram_mode that is not a draw (RB_MODECONTROL 0 ignore, 6 copy); colour 0's
// format not mapped while its write mask is non-zero; a blend factor 2, 3 or above 16, a blend op
// above 4 (Xenia maps 2, 3 to zero with a "?"; we refuse). Write masks of targets 1-3 are ignored
// (normalized away): the translator accepts exports to colour 0 only (shader_glsl.cpp:1111-1118), so
// the pair can only write target 0, as Xenia attaches only targets the pixel shader writes.
// A depth function or stencil op/function out of range is impossible (3-bit fields).
KeyResult DerivePipelineKey(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant,
                            PipelineKey& out);

// The host defines of each stage for this draw (shader_glsl.h): the vertex shader's XE_RECT_LIST
// for a rect list; per texture slot the shader reads, XE_TEXTURE_<s>_GAMMA when the fetch constant's
// signs of the channels its swizzle puts in x, y, z are all 3 (gamma; a constant 0/1 channel is
// neutral), XE_TEXTURE_<s>_SCALE ldexp(1.0, n) for a non-zero exp_adjust (dword 3 bits 13-18, signed),
// XE_TEXTURE_<s>_LOD_BIAS for a non-zero lod_bias (dword 4 bits 12-21, signed, / 32); the pixel
// shader's XE_ALPHA_TEST <func> + XE_ALPHA_REF <ref> when RB_COLORCONTROL alpha_test_enable (bit 3)
// is set (func bits 0-2), XE_COLOR0_GAMMA for colour format 1, XE_COLOR0_7E3 for format 3.
// Refuses a texture slot whose fetch constant is not a texture (type != 2) or whose dimension
// (dword 5 bits 9-10) disagrees with the shader's: 2D needs 1 with stacked (dword 1 bit 10) clear,
// 3D needs 2, cube needs 3 (shader_glsl.h textureDimensions; a stacked 2D fetched as 3D is refused).
KeyResult DeriveVariants(const DrawStateView& s, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                         ShaderVariantDefines& psDefines);

// ---- Vertex data -----------------------------------------------------------------------------------

// One used vertex fetch slot of a draw: the guest range the host copies into the slot's upload ring,
// and where it lands (xe_vertex_base[slot], in dwords from the start of the draw's vertex data).
struct VertexSlotPlan
{
    uint32_t slot = 0;
    uint32_t guestAddress = 0;  // bytes, physical (xe_gpu_vertex_fetch_t address << 2)
    uint32_t endian = 0;        // the fetch constant's (swapped in the shader; recorded for the log)
    uint32_t sizeDwords = 0;    // the fetch constant's size
    uint32_t copyDwords = 0;    // min(size, highIndex * stride + end): what the draw can read (shader_glsl.h)
    uint32_t baseDword = 0;     // offset of the copy in the draw's vertex data
};

// The plans of `vs`'s slots for a draw whose largest vertex index (after VGT_INDX_OFFSET, as the shader
// sees it) is `highIndex`. Consecutive slots are laid out at dword granularity (`totalDwords`).
// Refuses a slot whose constant is not a vertex fetch (type bits 0-1 != 3), has size 0, or whose
// range leaves the 512 MB physical space.
KeyResult PlanVertexSlots(const DrawStateView& s, const CachedShader& vs, uint32_t highIndex, std::vector<VertexSlotPlan>& out,
                          uint32_t& totalDwords);

// ---- Descriptors -------------------------------------------------------------------------------

// Per-pair descriptor set layouts. Each layout
// declares bindings 0-5 (shader_glsl.h) and exactly the sampler bindings 8 + slot the pair fetches, visible to the
// stages that fetch them, so any texture slot 0-31 can be bound and each stage counts only its own samplers against
// the device's per-stage limits (VUID-VkPipelineLayoutCreateInfo-descriptorType-03016: a binding counts towards every
// stage in its stageFlags; the shared 32-binding VS|PS layout broke it on the M5).
// The layout key is the stage mask per slot: bit 2s = the vertex shader fetches slot s, bit 2s + 1 = the pixel shader
// does. The dimension (2D, 3D, cube) is not part of it: a VkDescriptorSetLayout binding has no image dimension (the
// image view's type carries it, DescriptorPlan::dimension), so pairs that differ only in dimensions share a layout.
struct DescriptorPlan
{
    uint8_t dimension[32] = {};  // per texture slot: 0 unused, 1 2D, 2 3D, 3 cube (ucode kDimension*)
    uint32_t vsSlots = 0, psSlots = 0;  // bitmasks
    uint64_t layoutKey = 0;      // SamplerLayoutKey(vsSlots, psSlots)
};
constexpr uint64_t SamplerLayoutKey(uint32_t vsSlots, uint32_t psSlots)
{
    uint64_t k = 0;
    for (uint32_t s = 0; s < 32; s++)
        k |= uint64_t(vsSlots >> s & 1) << (2 * s) | uint64_t(psSlots >> s & 1) << (2 * s + 1);
    return k;
}
// The VS / PS slot bitmasks of a layout key.
constexpr uint32_t LayoutVsSlots(uint64_t key)
{
    uint32_t m = 0;
    for (uint32_t s = 0; s < 32; s++)
        m |= uint32_t(key >> (2 * s) & 1) << s;
    return m;
}
constexpr uint32_t LayoutPsSlots(uint64_t key)
{
    uint32_t m = 0;
    for (uint32_t s = 0; s < 32; s++)
        m |= uint32_t(key >> (2 * s + 1) & 1) << s;
    return m;
}

// The device limits a layout must fit (VkPhysicalDeviceLimits, queried by the renderer; never assumed). A combined
// image sampler counts as a sampler and as a sampled image (Vulkan spec, "Descriptor Set Layouts" limits), so the
// per-stage sampler budget is min(maxPerStageDescriptorSamplers, maxPerStageDescriptorSampledImages), the set's
// min(maxDescriptorSetSamplers, maxDescriptorSetSampledImages); maxPerStageResources counts the stage's buffers too
// (VS: storage 0, 4, 5 and uniform 1, 2 = 5; PS: uniform 3 = 1).
struct SamplerLimits
{
    uint32_t perStageSamplers = 0, perStageSampledImages = 0, perStageResources = 0;
    uint32_t setSamplers = 0, setSampledImages = 0;
};
constexpr uint32_t kVsBufferBindings = 5, kPsBufferBindings = 1;

// The pair's sampler bindings. Refuses: a slot past 31 or a dimension not 1-3; a slot both stages read with different
// dimensions (one binding, one image view); a stage whose samplers exceed the per-stage budget, or whose samplers plus
// buffers exceed maxPerStageResources; a set whose distinct sampler bindings exceed the set budget. Each refusal names
// the stage, the count and the limit.
KeyResult PlanDescriptors(const CachedShader& vs, const CachedShader& ps, const SamplerLimits& limits, DescriptorPlan& out);

// Fixed bindings (shader_glsl.h). 6 and 7 are unused.
enum DescriptorBinding : uint32_t
{
    kBindingVertexData = 0,   // storage, VS
    kBindingSystem = 1,       // uniform, VS
    kBindingVsConstants = 2,  // uniform, VS
    kBindingPsConstants = 3,  // uniform, PS
    kBindingIndices = 4,      // storage, VS (rect lists)
    kBindingErrors = 5,       // storage, VS
    kBindingTexture0 = 8,     // combined image sampler 8 + slot for texture slot 0-31, in the layouts of the pairs that fetch it
    kTextureBindings = 32,    // Xenos's texture fetch constants; a layout declares only the pair's (DescriptorPlan::layoutKey)
};

// ---- Uniform and storage block layouts (std140 / std430, shader_glsl.h) ------------------------------

// XeSystem: uvec4 xe_fetch[48] at 0 (the 192 fetch dwords), uvec4 xe_vertex_base[24] at 768 (96
// uints), vec4 xe_viewport_scale at 1152, vec4 xe_viewport_offset at 1168, uint xe_flags at 1184;
// 1188 bytes, bound as 1200 (16-byte multiple).
constexpr uint32_t kSystemFetchOffset = 0, kSystemVertexBaseOffset = 768, kSystemViewportScaleOffset = 1152,
                   kSystemViewportOffsetOffset = 1168, kSystemFlagsOffset = 1184, kSystemBytes = 1200;
// XeVsConstants / XePsConstants: vec4 xe_c[256] at 0 (4096 bytes), uvec4 xe_bool[2] at 4096 (32
// bytes), uvec4 xe_loop[8] at 4128 (128 bytes): 4256 bytes, always bound in full (the translator
// declares xe_bool/xe_loop only when used; binding more than declared is allowed).
constexpr uint32_t kConstantsAluOffset = 0, kConstantsBoolOffset = 4096, kConstantsLoopOffset = 4128, kConstantsBytes = 4256;
// XeErrors: one uint per draw, zeroed before the draw and read after the slot's confirmed completion.
constexpr uint32_t kErrorsBytes = 4;

// ---- The upload ring ----------------------------------------------------------------------------

// One ring per submission slot, persistently mapped and host-coherent: a bump allocator whose
// offsets honour the device's minUniformBufferOffsetAlignment / minStorageBufferOffsetAlignment
// (queried, never assumed). Reset exactly once per slot reuse: in the renderer's BeginSlot, after
// GpuGuard::Retire(slot) confirmed the previous submission (the slot is Idle) and before anything of
// the next submission is recorded; the slot's descriptor pool is reset at the same point and the
// slot's xe_error dwords are read just before. Never reset while the slot is in flight: the ring
// buffer is a persistent resource the slot's commands read, so its lifetime is the device's, and only
// the reset point guards its contents. Exhaustion is a refusal the renderer turns into a stop naming
// the bytes used (a 16 MB ring, D-b; ~9.7 KB of fixed data per draw plus the vertex copies: ~1,600
// draws per slot before vertex data, so a race frame (~3,000 draws) needs the multiple submissions
// per frame of unit 5).
class UploadRing
{
public:
    UploadRing(uint64_t capacity, uint64_t uniformAlign, uint64_t storageAlign)
        : capacity_(capacity), uniformAlign_(uniformAlign), storageAlign_(storageAlign)
    {
    }
    // `uniform`: the block is bound as a uniform buffer (else storage). Returns false when the ring
    // cannot hold `bytes` more (nothing is allocated then). Alignments must be powers of two.
    // FM4 Recomp (M7 CPU cost): a ring across parts. NewPart(minFree) keeps the offset where the previous part ended unless fewer
    // than `minFree` bytes remain, in which case the next lap starts at 0. Allocations never wrap inside a part, so a block of
    // the current lap below the offset is never overwritten before the next lap, and the next lap starts only at a part
    // boundary: renderer.cpp reuses vertex windows copied by earlier parts of the same lap (Intact) instead of copying them
    // again after each flush. Reset() empties everything.
    bool Allocate(uint64_t bytes, bool uniform, uint64_t& offset)
    {
        const uint64_t align = uniform ? uniformAlign_ : storageAlign_;
        if (align == 0 || (align & (align - 1)) != 0)
            return false;
        const uint64_t start = (used_ + align - 1) & ~(align - 1);
        if (start > capacity_ || bytes > capacity_ - start)
            return false;
        offset = start;
        used_ = start + bytes;
        allocations_++;
        return true;
    }
    void NewPart(uint64_t minFree)
    {
        if (capacity_ - used_ < minFree)
            lap_++, used_ = 0;
        partStart_ = used_, allocations_ = 0, resets_++;
    }
    void Reset() { used_ = 0, lap_ = 0, partStart_ = 0, allocations_ = 0, resets_++; }
    uint64_t Available() const { return capacity_ - used_; }
    // Whether [offset, offset + bytes), allocated in `lap`, is still in the buffer: only this lap's blocks below the offset are
    // (the lap's allocations only move forward, and the next lap starts at a part boundary).
    bool Intact(uint64_t offset, uint64_t bytes, uint64_t lap) const { return lap == lap_ && offset + bytes <= used_; }
    uint64_t Used() const { return used_; }
    uint64_t Lap() const { return lap_; }
    uint64_t Capacity() const { return capacity_; }
    uint64_t Allocations() const { return allocations_; }
    uint64_t Resets() const { return resets_; }

private:
    const uint64_t capacity_, uniformAlign_, storageAlign_;
    uint64_t used_ = 0, allocations_ = 0, resets_ = 0;
    uint64_t lap_ = 0, partStart_ = 0;
};

}  // namespace runtime::video
