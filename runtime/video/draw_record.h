// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// From a decoded DrawState (draw_state.h) to what the recorder needs, on the CPU and without Vulkan
// types, so that tests/runtime/test_draw_record.cpp covers it: the pipeline key and the shader
// variants of the draw (pipeline_key.h, from the decoded fields instead of the raw registers
// DrawStateView carried before DrawState existed), the host indices (auto-index, quad lists, rect
// lists, DMA indices), the byte images of the XeSystem and constant blocks (shader_glsl.h bindings 1,
// 2, 3), and the NoWrite policy switch. Every refusal is a KeyResult the renderer stops with
// (renderer.cpp:Draw, "Require"); nothing here stops or skips on its own.
#pragma once
#include "draw_state.h"
#include "pipeline_key.h"
#include "shader_cache.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime::video {

// What a draw that writes nothing (DrawKind::NoWrite: RB_COLOR_MASK 0, no depth or stencil write;
// FM4's legal-screen point lists: 19,187 of 21,643 draws in CP trace10) does on the
// host. Owner policy pending: Skip records nothing and counts the draw in the swap line;
// Stop treats it as unimplemented. One switch, read in renderer.cpp:Draw only.
enum class NoWriteDraws
{
    Skip,
    Stop,
};
constexpr NoWriteDraws kNoWriteDraws = NoWriteDraws::Skip;

// The pipeline key of a Draw or DepthOnly state (a NoWrite or Resolve state is refused: they never
// reach a pipeline). Normalization as pipeline_key.h documents; the topology comes from
// DrawState::hostPrimitive (rect and quad lists are triangle lists on the host), `samples` from the
// target (1, 2 or 4: the EDRAM unit's images carry the sample count, pipeline_cache.cpp), the depth
// attachment is present when the depth test or the stencil test is on (a test without a write still
// needs it), colour 0 when the draw writes colour (DrawKind::Draw with a non-zero mask).
// FM4 Recomp (M7 GPU cost, FM4_SUBSET_MERGE; renderer.cpp SubsetMerge): the key of a colour-only draw (depthUsed 0) recorded in an
// open colour + depth pass whose key had `depthFormat`: the same key with that depth attachment declared and neither tested,
// written, stencilled nor biased (depthFunc 7 = always, which PipelineCache reads as "no depth test" when nothing is written).
// Render-pass compatible with the open pass when the colour formats and samples are equal (pipeline_cache.cpp RenderPass key).
PipelineKey SubsetMergeKey(const PipelineKey& colorOnly, uint8_t depthFormat);
// The same for a depth-only draw (colorUsed 0: DepthOnly, or a colour write mask of 0) in an open colour + depth pass whose key had
// colour format `colorFormat`: the colour attachment declared with write mask 0 and no blend (1, 0, add / 1, 0, add), so nothing is
// written to it; the depth/stencil state is the draw's. The pixel shader stays the depth-only variant (no colour output), valid with
// a write mask of 0.
PipelineKey SubsetMergeKeyDepthOnly(const PipelineKey& depthOnly, uint8_t colorFormat);
KeyResult KeyFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, uint64_t vsVariant, uint64_t psVariant, PipelineKey& out);

// FM4 Recomp: vkCmdSetDepthBias's constant and slope factors (clamp 0) for DrawState's polygon offset, as Xenia's host render
// target path (draw_record.cpp DepthBiasFor).
struct HostDepthBias
{
    float constant = 0, slope = 0;
};
HostDepthBias DepthBiasFor(const DrawState& d);

// The host defines (shader_glsl.h) of each stage for this draw: XE_RECT_LIST for a rect list;
// XE_TEXTURE_<slot>_GAMMA (TextureSlot::gamma: x y z sign 3, w 0), _SCALE for a non-zero exp_adjust,
// _LOD_BIAS for a non-zero lod_bias (/ 32), for the stage(s) whose translation lists the slot;
// XE_ALPHA_TEST / XE_ALPHA_REF when the alpha test is on (func != 7); XE_COLOR0_GAMMA for storage
// format 1, XE_COLOR0_7E3 for 3, when colour is written.
KeyResult VariantsFromDrawState(const DrawState& d, const CachedShader& vs, const CachedShader& ps, ShaderVariantDefines& vsDefines,
                                ShaderVariantDefines& psDefines);

// FM4 Recomp (M7 CPU cost, opt-in FM4_VARIANT_MEMO): the variant memo. VariantsFromDrawState builds define text
// every draw and ShaderCache::Variant hashes it (~6 % of the command-processor thread in a race, ~1 us a draw).
// VariantMemoKeyFor writes every input VariantsFromDrawState reads (the fields listed in draw_record.cpp next to it), the shader
// pair (its objects: the cache never erases a shader or a variant, shader_cache.h; the renderer clears the memo at Shutdown, which
// destroys the cache) and the renderer's XE_FETCH_UNCHECKED decision
// as flat words; equal keys therefore give equal defines and the same two variants. Adapted from SpeedBreaker's POD pipeline key
// and its last-key memo (SpeedBreaker 312ce69, runtime/gpu/renderer.cpp:2839-2867, 3107-3120; GPL-3.0, as ours): there
// the key is the pipeline's, here it is the variant selection FM4 spells as define text. A new input of VariantsFromDrawState
// must be a new word here (tests/runtime/test_draw_record.cpp TestVariantMemo changes each input once).
struct VariantMemoKey
{
    static constexpr uint32_t kMaxTextures = 32;
    static constexpr uint32_t kHeaderWords = 19, kWordsPerTexture = 7;
    static constexpr uint32_t kMaxWords = kHeaderWords + kWordsPerTexture * kMaxTextures;
    uint32_t words[kMaxWords] = {};
    uint32_t count = 0;
    bool operator==(const VariantMemoKey& o) const { return count == o.count && std::memcmp(words, o.words, size_t(count) * 4) == 0; }
};
struct VariantMemoKeyHash
{
    size_t operator()(const VariantMemoKey& k) const  // FNV-1a-64 over the used words
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < k.count; i++)
            h = (h ^ k.words[i]) * 0x100000001b3ull;
        return size_t(h);
    }
};
// False (no memo for this draw: the caller takes the full path) when the draw has more texture slots than the key holds.
bool VariantMemoKeyFor(const DrawState& d, const CachedShader& vs, const CachedShader& ps, bool fetchUnchecked, VariantMemoKey& out);

// The memo: a key -> the two variants the full path chose for it. Cleared whole when it would exceed `maxEntries` (the next draws
// take the full path and refill it).
class VariantMemo
{
public:
    struct Entry
    {
        ShaderVariant* vs = nullptr;
        ShaderVariant* ps = nullptr;
    };
    explicit VariantMemo(size_t maxEntries) : max_(maxEntries) {}
    const Entry* Find(const VariantMemoKey& k) const
    {
        const auto it = map_.find(k);
        return it == map_.end() ? nullptr : &it->second;
    }
    void Insert(const VariantMemoKey& k, ShaderVariant* vs, ShaderVariant* ps)
    {
        if (vs == nullptr || ps == nullptr)
            return;
        if (map_.size() >= max_)
            map_.clear(), clears_++;
        map_[k] = Entry{vs, ps};
    }
    void Clear() { map_.clear(); }
    size_t Size() const { return map_.size(); }
    uint64_t Clears() const { return clears_; }

private:
    std::unordered_map<VariantMemoKey, Entry, VariantMemoKeyHash> map_;
    size_t max_;
    uint64_t clears_ = 0;
};

// ---- Indices --------------------------------------------------------------------------------------------

// A guest index as the GPU reads it: `i` is the element number; the result is the raw 16- or 32-bit
// value, host order (the caller applies the DMA endian: pm4 DRAW_INDX, Xenia primitive_processor.cc).
using GuestIndexReader = uint32_t (*)(const void* context, uint32_t i);

struct HostIndices
{
    enum class Kind
    {
        Direct,    // vkCmdDraw(vertexCount): gl_VertexIndex is the guest index (auto-index, no offset, no clamping)
        Indexed,   // vkCmdDrawIndexed with `indices` (32-bit, host order) in the ring
        RectList,  // vkCmdDraw(vertexCount = 6 per rect) with `indices` (3 guest indices per rect) bound as xe_indices (binding 4)
    };
    Kind kind = Kind::Direct;
    std::vector<uint32_t> indices;
    uint32_t vertexCount = 0;  // what the host draws
    uint32_t highIndex = 0;    // the largest guest index the vertex shader may fetch (VertexWindowDwords)
};

// Index i of the draw -> the guest index the vertex shader sees: ((raw + VGT_INDX_OFFSET) & 0xFFFFFF)
// clamped to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX] (draw_state.h; X registers.h:299-360), raw = i for an
// auto-indexed draw (source select 2) or the DMA buffer's element for an indexed one (source 0; `read`
// is then required). Quad lists become triangle lists (q0 q1 q2)(q0 q2 q3) (X primitive_processor.h
// QuadListToTriangleList, draw_state.h); rect lists keep their 3 guest indices per rect for the vertex
// shader's XE_RECT_LIST expansion (shader_glsl.h). Refuses: 0 indices; a rect list whose count is not
// a multiple of 3, a quad list not a multiple of 4; an indexed draw without a reader; a DMA endian
// other than none / 8in16 for 16-bit or none / 8in32 for 32-bit indices (the others have no observed
// meaning: refused, not guessed); a 16-bit DMA buffer whose bytes are fewer than 2 * count (32-bit: 4 * count);
// restart on anything but a strip (DrawState never sets it so). With DrawState's IndexBuffer::restart, an element equal to
// the reset index (after the swap, before the offset and clamp) becomes 0xFFFFFFFF, the host's restart value; highIndex
// skips it.
KeyResult BuildHostIndices(const DrawState& d, GuestIndexReader read, const void* context, HostIndices& out);

// The DMA endian applied to one raw element (X xenos.h:193-198 Endian; command_processor.h GpuSwap for 32-bit).
uint32_t SwapIndex(uint32_t raw, bool is32Bit, uint32_t endian);

// ---- Uploads --------------------------------------------------------------------------------------------

// Per used vertex fetch slot: the window the host copies and where it lands (pipeline_key.h D4).
struct VertexWindow
{
    uint32_t slot = 0, physical = 0, endian = 0;
    uint32_t dwords = 0;     // VertexWindowDwords(slot, highIndex): 0 for an invalid constant (an empty window, not a refusal)
    uint32_t baseDword = 0;  // offset in the draw's vertex data block (xe_vertex_base[slot])
};
// The windows of every slot the vertex shader lists, laid out consecutively; `totalDwords` the block's size.
void PlanVertexWindows(const DrawState& d, uint32_t highIndex, std::vector<VertexWindow>& out, uint32_t& totalDwords);

// XeSystem (binding 1, kSystemBytes): xe_fetch[48] holds, for each used vertex slot, word 0 as the
// constant (address | 3) and word 1 as (window dwords << 2) | endian, so the shader's bounds check
// (xe_vertex_word) sees the copied window as the slot's size; every other fetch entry is 0 (the shader
// reads xe_fetch for vertex slots only). xe_vertex_base[slot] = the window's baseDword; the viewport
// scale/offset vec4s from FoldViewport; xe_flags = DrawState::systemFlags (shader_glsl.h kFlag*, from PA_CL_VTE_CNTL).
void FillXeSystem(const std::vector<VertexWindow>& windows, const HostViewport& viewport, uint32_t vteFlags, uint8_t out[kSystemBytes]);

// XeVsConstants / XePsConstants (bindings 2 / 3, kConstantsBytes): the stage's 256 float4 (DrawState
// aluConstants words 0-1023 for the VS, 1024-2047 for the PS, as the register file holds them), then
// the 8 bool and 32 loop dwords (both stages get the same).
void FillConstants(const DrawState& d, bool pixel, uint8_t out[kConstantsBytes]);

// FM4 Recomp (M7 CPU cost, opt-in FM4_CONSTANTS_PER_STAGE=1 in the renderer): whether FillConstants(d, pixel) writes the bytes the
// stage's shader reads (its first `words` alu words, then the bool and loop tail) exactly as it did for a previous draw whose stage
// had `lastWords` words, alu words `lastAlu` (2048) and bool / loop dwords `lastBools` / `lastLoops`. Conservative: false unless both
// alu vectors have the 2048 words FillConstants copies from, the word counts are equal, and every word compared is equal (the bytes
// past `words` up to 4096 are never read by the shader: DrawState::vsConstantWords / psConstantWords).
bool SameStageConstants(const DrawState& d, bool pixel, uint32_t lastWords, const std::vector<uint32_t>& lastAlu, const uint32_t lastBools[8],
                        const uint32_t lastLoops[32]);

// ---- The draw extent from the vertices (FM4 Recomp) -----------------------------------------

// The rows the EDRAM bind may claim for this draw (edram::Bind's heightUsed): d.target.targetHeight, lowered to
// min(edramRows, edram::EstimateMaxY(d.extent, &vertexMaxY)) when d.extentNeedsVertices (X render_target_cache.cc:
// 757-767; draw_extent_estimator.cc:199-264) and the vertex shader's position is a plain vertex fetch
// (ShaderTranslation::positionFetch). The positions are read from `window`, the guest memory of that fetch's slot
// as 32-bit loads (`windowDwords` long, from the fetch constant's address; the bytes the renderer copies), at each
// guest index the draw references (`indices`: every index of Direct, else its index list) with the slot's
// `endian` (edram::PassThroughPositions, w = 1). When the vertices are needed but the shader does not qualify, the
// window is null or too short, `height` stays the scissor bound (today's behaviour: the bind may then refuse) and
// `note` says why, for the stop line. Returns true when the vertex bound was applied.
bool BoundTargetHeight(const DrawState& d, const shader::ShaderTranslation& vs, const uint32_t* window, size_t windowDwords, uint32_t endian,
                       const HostIndices& indices, uint32_t& height, std::string& note);

// ---- Overwriting draws (FM4 Recomp, FM4_OVERWRITE_CLAIM) ------------------------------------------------------------

// The rectangle of target pixels a draw provably overwrites in every sample, so the EDRAM bind can claim the tiles inside it
// without transferring the previous owner's content (edram::Bind cutouts; the idea is ShiftGlue's depth-overwrite claim,
// ShiftGlue SDK include/rex/graphics/fh1_depth_overwrite.h, which runs the vertex shader on a CPU
// interpreter; this is an independent, narrower implementation on what the host can prove without one). Fail closed: `depth` /
// `color` are set only when every condition holds, else `why` names the first that does not.
//   - one rectangle: a rect-list draw (primitive 8) of exactly 3 auto-indexed vertices, no culling;
//   - screen-space positions: the vertex shader's position is a plain vertex fetch (ShaderTranslation::positionFetch; FM4's
//     clear VS), PA_CL_VTE_CNTL vtx_xy_fmt (kFlagXyDividedByW) with no viewport scale or offset, so the fetched x, y plus the
//     pixel-centre / window offset (GuestViewport::offsetAddXy) are the screen position; the three vertices at three corners of
//     an axis-aligned rectangle; pixel i is covered when its centre i + 0.5 lies in [min, max): [ceil(min - 0.5), ceil(max - 0.5)),
//     intersected with the scissor;
//   - nothing discards a fragment: no kill op in the pixel shader (`psKills`), no alpha test, no alpha-to-mask;
//   - depth: z test on and ALWAYS with z write, stencil on with ALWAYS, zpass REPLACE and write mask 0xFF on both faces (so both
//     aspects of every sample are rewritten: FM4's clear, RB_DEPTHCONTROL 8777);
//   - colour: all four channels written (colour mask 0xF for target 0), no blending, and the depth and stencil tests cannot
//     reject (off or ALWAYS).
struct OverwriteRect
{
    bool depth = false, color = false;
    edram::Rect rect;
    std::string why;
};
OverwriteRect FindOverwriteRect(const DrawState& d, const shader::ShaderTranslation& vs, bool psKills, const uint32_t* window, size_t windowDwords,
                                uint32_t endian);

// ---- Clear-class draws (FM4 Recomp, M7 GPU cost, FM4_CLEAR_DRAWS) ---------------------------------------------------------

// An overwriting draw (FindOverwriteRect) whose every bound attachment is written with one constant the host can read without
// running the shaders: the renderer hands it to the EDRAM unit as a pending clear of its targets (render_hooks.h ClearRequest, the
// deferred resolve clears' path) instead of recording a one-draw render pass. The values are what the host draw itself would write,
// so the deferred clear stands in for the draw.
// Fail closed: `ok` only when all of these hold, else `why` names the first that does not:
//   - FindOverwriteRect proved the rectangle (`o`); the colour attachment is bound (colour mask != 0) only when it proved `color`
//     and the depth attachment (z or stencil test on) only when it proved `depth`; no texture slot is bound; no pixel parameter
//     generation (the pixel shader's register would be xe_param_gen, not the interpolator);
//   - on a multisampled target the rectangle's edges (the three vertices' screen x and y) are whole pixels, so the draw writes every
//     sample of the pixels it covers, as vkCmdClearAttachments does (at 1x the pixel-centre rule is exact and fractional edges pass);
//   - depth: the three vertices' z (the position fetch's third dword) are one finite value, not -0; PA_CL_CLIP_CNTL clip_disable
//     (the pipeline's depth clamp with the depth range 0..1) with the z viewport scale 1 and offset 0, so the fold is the identity
//     (draw_state.h FoldViewport; xe_fixup's w is the fetch's constant 1) and the depth written is clamp(z, 0, 1); no polygon offset;
//     the stencil written is the front reference (REPLACE, write mask 0xFF), equal to the back reference when back-face state is on;
//   - colour: the pixel shader passes interpolator 0 through (shader_glsl.h ShaderTranslation::colorPassThrough) and the vertex
//     shader exports interpolator 0 from a 4-dword vertex fetch of the position's slot (interpolator0Fetch); the three vertices' four
//     dwords are bitwise equal and finite; the target's storage format is 0 or 2 (the attachment's UNORM conversion clamps to [0, 1]:
//     applied here too, so the value does not depend on whether the clear path clamps) or 3 (shader_glsl.h XE_COLOR0_7E3: rgb to
//     [0, 31.875], alpha to [0, 1], NaN and negatives to 0, applied here). Format 1 (the PWL gamma encode), 4 and 14 (cleared
//     through the uint transfer view) and the rest are refused.
// `rect` is FindOverwriteRect's cut to `targetHeight` rows: the rows the bind claims and the host scissor stops at (renderer.cpp
// scissorBottom), so the clear touches nothing the draw could not.
struct ClearDraw
{
    bool ok = false;
    bool color = false, depth = false;  // the attachments the draw binds; the clear writes exactly these
    edram::Rect rect;
    float rgba[4] = {0, 0, 0, 0};
    float depthValue = 0;
    uint32_t stencil = 0;
    std::string why;
};
ClearDraw FindClearDraw(const DrawState& d, const OverwriteRect& o, const shader::ShaderTranslation& vs, const shader::ShaderTranslation& ps,
                        const uint32_t* window, size_t windowDwords, uint32_t endian, uint32_t targetHeight);

}  // namespace runtime::video
