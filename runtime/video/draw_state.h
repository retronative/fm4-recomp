// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The draw state (M4 unit 8, execution model §7): everything a Vulkan draw needs, decoded from the
// GPU register file the command processor holds and validated fail-closed, as a pure function with
// no guest-memory reads, no Vulkan, no stops. The renderer calls DecodeDrawState at the CP's draw
// hook (renderer.cpp:Draw) and stops with `error` when `kind` is Refused; nothing is skipped or
// guessed: a register value this unit does not model is a refusal naming the register and value.
//
// Register semantics: Xenia xenia-canary 67f16c4e315f src/xenia/gpu/ (registers.h for the bit
// fields, register_table.inc for the indices, xenos.h for the enums and fetch constants,
// draw_util.cc for the viewport and scissor derivations, pm4_command_processor_implement.h for
// the draw packet; "X file:line" below). Where ForzaRecomp's reviewed draw path established an
// FM4 fact or a simplification, it is cited as "FR runtime/gpu/render.cpp:line"
// (GPL-3.0-or-later, same owner; facts and reviewed units only, re-derived here).
//
// Inputs. The draw packet's words are read from the register file: Xenia's command processor
// writes VGT_DRAW_INITIATOR (0x21FC), VGT_DMA_BASE (0x21FA) and VGT_DMA_SIZE (0x21FB) from the
// DRAW_INDX / DRAW_INDX_2 packet before issuing the draw (X pm4_command_processor_implement.h:1003,
// :1025, :1034); our CP must do the same before calling the hook (runtime/kernel/command_processor.cpp,
// DRAW_INDX case). DRAW_INDX's first word, the viz-query token, is passed
// separately (0 for DRAW_INDX_2; X :1112, :1123). The two shader translations give which fetch
// slots, texture slots, bool and loop constants the draw reads (shader/shader_glsl.h).
//
// Decode order matters for FM4: its first frames' point-list draws (trace2_cp.txt lines 3-50:
// RB_SURFACE_INFO, RB_COLOR_INFO and RB_DEPTH_INFO all 0) write neither colour nor depth nor
// stencil, so they are classified NoWrite from RB_COLOR_MASK and RB_DEPTHCONTROL before the
// render target is validated (a pitch-0 surface is refused only for a draw that writes).
#pragma once
#include "shader/shader_glsl.h"
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

// Register indices (X register_table.inc, the line of each entry).
namespace xr {
constexpr uint32_t kRbSurfaceInfo = 0x2000;        // :487
constexpr uint32_t kRbColorInfo = 0x2001;          // :488
constexpr uint32_t kRbDepthInfo = 0x2002;          // :489
constexpr uint32_t kPaScScreenScissorTl = 0x200E;  // :501
constexpr uint32_t kPaScScreenScissorBr = 0x200F;  // :502
constexpr uint32_t kPaScWindowOffset = 0x2080;     // :504
constexpr uint32_t kPaScWindowScissorTl = 0x2081;  // :505
constexpr uint32_t kPaScWindowScissorBr = 0x2082;  // :506
constexpr uint32_t kVgtMaxVtxIndx = 0x2100;        // :508
constexpr uint32_t kVgtMinVtxIndx = 0x2101;        // :509
constexpr uint32_t kVgtIndxOffset = 0x2102;        // :510
constexpr uint32_t kVgtMultiPrimIbResetIndx = 0x2103;  // :511
constexpr uint32_t kRbColorMask = 0x2104;          // :512
constexpr uint32_t kRbBlendRed = 0x2105;           // :513-516 (red, green, blue, alpha: floats)
constexpr uint32_t kRbStencilRefMaskBf = 0x210C;   // :520
constexpr uint32_t kRbStencilRefMask = 0x210D;     // :521
constexpr uint32_t kRbAlphaRef = 0x210E;           // :522 (float)
constexpr uint32_t kPaClVportXScale = 0x210F;      // :523-528 (x scale, x offset, y scale, y offset, z scale, z offset: floats)
constexpr uint32_t kSqProgramCntl = 0x2180;        // :530
constexpr uint32_t kSqContextMisc = 0x2181;        // :531
constexpr uint32_t kSqInterpolatorCntl = 0x2182;   // :532
constexpr uint32_t kVgtDmaBase = 0x21FA;           // :538
constexpr uint32_t kVgtDmaSize = 0x21FB;           // :539
constexpr uint32_t kVgtDrawInitiator = 0x21FC;     // :540
constexpr uint32_t kRbDepthControl = 0x2200;       // :543
constexpr uint32_t kRbBlendControl0 = 0x2201;      // :544
constexpr uint32_t kRbColorControl = 0x2202;       // :545
constexpr uint32_t kPaClClipCntl = 0x2204;         // :547
constexpr uint32_t kPaSuScModeCntl = 0x2205;       // :548
constexpr uint32_t kPaClVteCntl = 0x2206;          // :549
constexpr uint32_t kRbModeControl = 0x2208;        // :551
constexpr uint32_t kPaSuPointSize = 0x2280;        // :556
constexpr uint32_t kPaSuPointMinMax = 0x2281;      // :557
constexpr uint32_t kVgtOutputPathCntl = 0x2284;    // :560
constexpr uint32_t kPaScVizQuery = 0x2293;         // :575
constexpr uint32_t kPaScAaConfig = 0x2301;         // :579
constexpr uint32_t kPaSuVtxCntl = 0x2302;          // :580
constexpr uint32_t kSqVsConst = 0x2307;            // :585
constexpr uint32_t kSqPsConst = 0x2308;            // :586
constexpr uint32_t kPaScAaMask = 0x2312;           // :596
constexpr uint32_t kPaSuPolyOffsetFrontScale = 0x2380;  // :622-625 (front scale, front offset, back scale, back offset: floats)
constexpr uint32_t kAluConstants = 0x4000;         // :655 SHADER_CONSTANT_000_X; 512 float4 = 2048 words
constexpr uint32_t kFetchConstants = 0x4800;       // :2704 SHADER_CONSTANT_FETCH_00_0; 32 x 6 words
constexpr uint32_t kBoolConstants = 0x4900;        // :2897 SHADER_CONSTANT_BOOL_000_031; 8 words
constexpr uint32_t kLoopConstants = 0x4908;        // :2906 SHADER_CONSTANT_LOOP_00; 32 words
constexpr uint32_t kRegisterCount = 0x5003;        // kernel/graphics.h kRegisterCount (the CP's register file)
}  // namespace xr

// X xenos.h:41-57 PrimitiveType. Accepted: 1 (point list, only for a draw that writes nothing:
// FM4's first frames, FR milestones/M3-runtime.md "GPU work in swaps 1-3"), 4 (triangle list),
// 6 (triangle strip), 8 (rectangle list: the translator's XE_RECT_LIST expansion, shader_glsl.h),
// 13 (quad list: the host splits each quad (0 1 2)(0 2 3), X primitive_processor.h:607-624
// QuadListToTriangleList, whose order Xenia marks "TODO: find the correct order").
enum class HostPrimitive : uint32_t
{
    TriangleList,   // native
    TriangleStrip,  // native
    RectList,       // the vertex shader expands it (XE_RECT_LIST, xe_indices)
    QuadList,       // the host rewrites the indices as triangle lists
    PointNoWrite,   // a point list of a NoWrite draw: nothing to render
};

enum class DrawKind : uint32_t
{
    Refused,    // `error` names the register, its value and what is not modelled
    NoWrite,    // RB_MODECONTROL 4 or 5, no colour component, depth or stencil write: a no-op by its own state
    Draw,       // RB_MODECONTROL 4 (kColorDepth): colour 0 and depth/stencil as decoded
    DepthOnly,  // RB_MODECONTROL 5 (kDepth): as Draw with every colour write off (X xenos.h:903-924)
    Resolve,    // RB_MODECONTROL 6 (kCopy): a resolve, decoded by the resolve unit, not here
};

// The index source (X registers.h:282-296 VGT_DRAW_INITIATOR, xenos.h:784-788 SourceSelect).
struct IndexBuffer
{
    bool indexed = false;      // false: auto-indexed (source 2): index i is i
    uint32_t physical = 0;     // VGT_DMA_BASE with the low bits of the index size cleared (X pm4:1042)
    uint32_t bytes = 0;        // VGT_DMA_SIZE num_words (bits 0-23) times the index size (X pm4:1045)
    bool is32Bit = false;      // initiator bit 11 (X registers.h:290; xenos.h:209-213 IndexFormat)
    uint32_t endian = 0;       // VGT_DMA_SIZE bits 30-31 (X registers.h:277): GpuSwap of each index (kernel/command_processor.h)
    uint32_t count = 0;        // initiator bits 16-31 num_indices
};

// PA_CL_VPORT_* with the PA_CL_VTE_CNTL enables applied (X draw_util.cc:306-319: a disabled scale is
// 1, a disabled offset 0), plus the integer.5 and window offsets Xenia adds (:320-332).
struct GuestViewport
{
    float scale[3] = {1, 1, 1};   // x, y, z
    float offset[3] = {0, 0, 0};  // x, y, z: the register values (PA_CL_VPORT_*OFFSET) only
    float offsetAddXy[2] = {0, 0};  // + PA_SC_WINDOW_OFFSET when vtx_window_offset_enable (:324-328), + 0.5 unless pix_center (:329-332)
    bool clipDisabled = false;    // PA_CL_CLIP_CNTL bit 16 (X registers.h:502): offsetting in the shader, no clipping (:349-375)
    bool dxClipSpace = false;     // PA_CL_CLIP_CNTL bit 19 dx_clip_space_def (:505): Z clip space 0..W, else -W..W (:437-494)
    // PA_CL_VTE_CNTL bits 8-10 vtx_xy_fmt, vtx_z_fmt, vtx_w0_fmt (X registers.h:530-532): the exported position's
    // format, undone by the translated vertex shader's xe_fixup from DrawState::systemFlags (shader/shader_glsl.h
    // kFlagXyDividedByW, kFlagZDividedByW, kFlagWNotReciprocal; X vulkan_command_processor.cc:3539-3555,
    // spirv_shader_translator.cc:1482-1554). FM4 (CP trace10): 0x300 (xy and z divided, w the
    // reciprocal) for its point draws and clears, 0x43F / 0x400 (w not the reciprocal) for its UI draws.
    bool xyDividedByW = false, zDividedByW = false, wNotReciprocal = false;
};

// The viewport as the translated vertex shader expects it (shader_glsl.h: gl_Position =
// vec4(q.xyz * scale.xyz + offset.xyz * q.w, q.w) for the fixed-up position q, whose w is the real
// W) for a host target `width` x `height` pixels,
// with the host VkViewport covering the whole target: Vulkan clip x, y in -1..1 over the target
// and depth zMin..zMax. XY: FR render.cpp:1199-1222 (guest pixels -> clip: scale * 2 / size,
// (offset + offsetAdd) * 2 / size - 1). Z: X draw_util.cc:434-518: with clipping, dx_clip_space_def
// maps NDC z 0..1 to offset..offset + scale (:437-441), else -1..1 is first remapped to 0..1
// (ndc z * 0.5 + 0.5) with the range offset - scale..offset + scale (:442-494), both saturated
// (:508-509); a reversed range stays reversed (Vulkan allows minDepth > maxDepth; Xenia swaps it
// for Direct3D 12, :513-517). With clipping disabled the shader applies the z scale and offset
// and the depth range is 0..1 (:371-375). APPROXIMATE: with clipping enabled
// Xenia clips to the viewport rectangle (:380-432), here to the whole target; FM4's Direct3D 9
// scissor equals its viewport, so covered pixels agree (the guest's PA_SC_WINDOW_SCISSOR).
struct HostViewport
{
    float scale[4] = {1, 1, 1, 1}, offset[4] = {0, 0, 0, 0};
    float zMin = 0, zMax = 1;
};
HostViewport FoldViewport(const GuestViewport& v, uint32_t width, uint32_t height);

// X draw_util.cc:639-713 GetScissor with clamp_to_surface_pitch: the window scissor (14-bit fields,
// X registers.h:572-593), moved by the window offset (signed 15-bit, :561-569) unless TL bit 31
// window_offset_disable, intersected with the screen scissor (signed 15-bit, :539-559), x clamped
// to RB_SURFACE_INFO surface_pitch, both clamped at 0, BR at least TL (an empty rectangle, never a
// reordered one).
struct Scissor
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
};

// RB_DEPTHCONTROL (X registers.h:744-765), RB_STENCILREFMASK(_BF) (:767-777). Every field is a total
// enum (xenos.h:726-746 CompareFunction, StencilOp), so nothing here is refused. The back-face
// fields apply when backfaceEnable, else the front ones do (FR render.cpp DecodeDepthStencil).
struct DepthStencil
{
    bool stencilEnable = false, zEnable = false, zWriteEnable = false, backfaceEnable = false;
    uint32_t zFunc = 0;  // CompareFunction: 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always
    struct Face
    {
        uint32_t func = 0, fail = 0, zPass = 0, zFail = 0;  // CompareFunction; StencilOp 0 keep, 1 zero, 2 replace, 3 inc clamp, 4 dec clamp, 5 invert, 6 inc wrap, 7 dec wrap
        uint32_t ref = 0, compareMask = 0, writeMask = 0;   // RB_STENCILREFMASK bits 0-7, 8-15, 16-23
    } front, back;
};

// RB_BLENDCONTROL0 (X registers.h:726-742; xenos.h:749-774 BlendFactor, BlendOp) and the constant
// colour RB_BLEND_RED..ALPHA for factors 12-15. `enabled` is false exactly for one/zero/add on both
// colour and alpha (0x00010001), which is Xenos's "no blending" (there is no enable bit).
struct Blend
{
    bool enabled = false;
    uint32_t srcColor = 1, dstColor = 0, colorOp = 0, srcAlpha = 1, dstAlpha = 0, alphaOp = 0;
    float constant[4] = {0, 0, 0, 0};
};

// RB_COLORCONTROL bits 0-3 (X registers.h:630-635) and RB_ALPHA_REF: the translated pixel shader's
// XE_ALPHA_TEST / XE_ALPHA_REF (shader_glsl.h). `enabled` is false when the function is 7 (always).
struct AlphaTest
{
    bool enabled = false;
    uint32_t func = 7;
    float ref = 0;
};

// RB_SURFACE_INFO (X registers.h:618-628), RB_COLOR_INFO (:684-700), RB_DEPTH_INFO (:779-792).
// Xenos surfaces have no height: `edramRows` is how many pixel rows fit between the surface's base
// and the next surface's base or the end of EDRAM (FR render.cpp:136-150 SurfaceHeight, INFERRED
// there; tile arithmetic X xenos.h:455-480: 80x16 samples a tile, 2048 tiles, 4X MSAA doubles the
// pitch in samples, 2X and 4X halve the rows a tile holds). `targetHeight` = min(edramRows, the
// scissor's bottom), the host target rows this draw can touch.
struct RenderTarget
{
    uint32_t pitch = 0;        // surface_pitch, pixels (bits 0-13)
    uint32_t msaa = 0;         // msaa_samples (bits 16-17): 0 1X, 1 2X, 2 4X
    uint32_t samples = 1;      // 1 << msaa
    uint32_t hizPitch = 0;     // bits 18-31
    uint32_t colorBase = 0;    // RB_COLOR_INFO bits 0-10, tiles
    uint32_t colorFormat = 0;  // bits 16-19 (xenos.h:299-318 ColorRenderTargetFormat), the storage format (xenos.h:394-404: 10 -> 2, 12 -> 3)
    uint32_t colorFormatRaw = 0;
    int32_t colorExpBias = 0;  // bits 20-25, signed
    uint32_t depthBase = 0;    // RB_DEPTH_INFO bits 0-10, tiles
    uint32_t depthFormat = 0;  // bit 16: 0 D24S8, 1 D24FS8 (xenos.h:406-409)
    uint32_t pitchTiles = 0;   // the surface's width in tiles
    uint32_t edramRows = 0, targetHeight = 0;
    bool colorWritten = false, depthWritten = false;  // which surfaces this draw touches (NoWrite: neither)
};

// A vertex fetch constant the vertex shader uses (X xenos.h:1135-1146 xe_gpu_vertex_fetch_t): the
// host copies [0, VertexWindowDwords(slot, high)) dwords of it for a draw whose largest index is
// `high` and binds that window as the slot's size (shader_glsl.h bindings 0 and 1). A constant whose
// type is not 3 (kVertex) or whose size is 0 is an empty window and not a refusal: the shader lists
// slots it may never fetch (conditional execs, loops), and FM4 leaves such slots as 00000001
// 00000000 (kInvalidVertex; FR render.cpp:541-548); an executed fetch of an empty window fails the
// shader's bounds check and the host stops. A type-3 constant at address 0 or past physical memory
// is refused.
struct VertexSlot
{
    uint32_t slot = 0;         // 0-95
    bool valid = false;        // type 3 with a size
    uint32_t physical = 0;     // bits 2-31 of word 0 (an address in bytes: the field is the dword address << 2, i.e. the low two bits are the type)
    uint32_t sizeWords = 0;    // word 1 bits 2-25
    uint32_t endian = 0;       // word 1 bits 0-1 (the shader swaps)
    uint32_t stride = 0, end = 0;  // the translation's largest stride and element end for the slot, dwords
};
uint32_t VertexWindowDwords(const VertexSlot& s, uint32_t highIndex);

// A texture fetch constant a stage samples (X xenos.h:1206-1307 xe_gpu_texture_fetch_t), decoded in
// full; the host's image and sampler come from it. `magFilter` / `minFilter` / `mipFilter` are the
// resolved filters: the fetch instruction's unless it says 3 (use the constant). Refusals keep to
// what the texture unit models: the accepted formats, dimensions 2D / 3D / cube as the
// shader fetches them, repeat / mirror / clamp-to-edge / border (black or white) clamps, point or
// linear filters, no anisotropy, a single level unless the resolved mip filter is 2 (base map),
// component swizzles of X Y Z W 0 1, unsigned or gamma (x y z sign 3, w 0) components, signed only
// for the float and int16 formats ForzaRecomp established (FR render.cpp:586-636).
struct TextureSlot
{
    uint32_t slot = 0;        // 0-31
    uint32_t words[6] = {};   // the raw constant (for the error text and the texture cache key)
    uint32_t type = 0;        // dword 0 bits 0-1 (2 kTexture)
    uint32_t sign[4] = {};    // bits 2-9, x y z w (xenos.h:121-129 TextureSign)
    uint32_t clamp[3] = {};   // bits 10-18, x y z (xenos.h:104-113 ClampMode)
    uint32_t signedRfMode = 0;  // bit 19
    uint32_t pitchTexels = 0;   // bits 22-30 << 5
    bool tiled = false;         // bit 31
    uint32_t format = 0;        // dword 1 bits 0-5 (xenos.h:491-581 TextureFormat)
    uint32_t endian = 0;        // bits 6-7
    uint32_t requestSize = 0;   // bits 8-9
    bool stacked = false;       // bit 10
    uint32_t nearestClampPolicy = 0;  // bit 11
    uint32_t physical = 0;      // bits 12-31 << 12: the base level
    uint32_t width = 0, height = 0, depth = 0;  // dword 2 per the dimension (+1 applied): 2D/cube 13+13 bits, 3D 11+11+10, 1D 24
    uint32_t stackDepth = 0;    // 2D: bits 26-31 (+1)
    uint32_t numFormat = 0;     // dword 3 bit 0 (0 fraction, 1 integer)
    uint32_t swizzle = 0;       // bits 1-12: component i = bits 3i-3i+2 (0-3 X-W, 4 zero, 5 one)
    int32_t expAdjust = 0;      // bits 13-18 signed: XE_TEXTURE_<slot>_SCALE
    uint32_t magFilter = 0, minFilter = 0, mipFilter = 0;  // resolved (see above)
    uint32_t anisoFilter = 0;   // bits 25-27
    uint32_t arbitraryFilter = 0;  // bits 28-30
    uint32_t borderSize = 0;    // bit 31
    uint32_t volMagFilter = 0, volMinFilter = 0;  // dword 4 bits 0, 1 (resolved as the mag/min filters)
    uint32_t mipMinLevel = 0, mipMaxLevel = 0;    // bits 2-5, 6-9
    int32_t lodBias = 0;        // bits 12-21 signed, 1/32 LOD: XE_TEXTURE_<slot>_LOD_BIAS
    uint32_t borderColor = 0;   // dword 5 bits 0-1 (xenos.h:150-164)
    uint32_t dimension = 0;     // bits 9-10 (xenos.h:97-102 DataDimension: 0 1D, 1 2D or stacked, 2 3D, 3 cube)
    bool packedMips = false;    // bit 11
    uint32_t mipPhysical = 0;   // bits 12-31 << 12
    uint32_t fetchDimension = 0;  // the shader's (ucode.h kDimension2D 1, 3DOrStacked 2, Cube 3)
    bool gamma = false;         // x y z sign 3: XE_TEXTURE_<slot>_GAMMA
};

// SQ_PROGRAM_CNTL (X registers.h:104-124), SQ_CONTEXT_MISC (:126-215), SQ_INTERPOLATOR_CNTL
// (:217-227), SQ_VS_CONST / SQ_PS_CONST (:229-252).
struct Program
{
    uint32_t vsNumReg = 0, psNumReg = 0;  // bits 0-7, 8-15: temporaries (0x80: none; X registers.h:107-110)
    bool vsResource = false, psResource = false;  // bits 16, 17 (:113-114); FM4 sets vs_resource on every writing draw (trace10)
    uint32_t vsExportCount = 0;           // bits 20-23
    uint32_t vsExportMode = 0;            // bits 24-26 (xenos.h:832-842): only 0, position as one vector, is modelled
    uint32_t psExportMode = 0;            // bits 27-30
    bool paramGen = false;                // bit 18: pixel parameter generation (refused: not in the translator)
    uint32_t paramGenPos = 0;             // SQ_CONTEXT_MISC bits 8-15
    bool scOutputScreenXy = false;        // SQ_CONTEXT_MISC bit 1
    uint32_t sampleControl = 0;           // SQ_CONTEXT_MISC bits 2-3 (xenos.h:846-850)
    // SQ_INTERPOLATOR_CNTL bits 0-15 (flat shading per interpolator), 16-31 (centroid / centre per interpolator, X
    // registers.h:220-223). Recorded, not applied (APPROXIMATE): Xenia never reads param_shade (FM4 sets 00010001 on
    // every UI draw, trace13; the decoder logs the first non-zero value once); Xenia uses sampling_pattern for
    // centroid interpolation on multisampled targets (vulkan_pipeline_cache.cc:190-195), which is not modelled.
    uint32_t paramShade = 0, samplingPattern = 0;
    uint32_t vsConstBase = 0, vsConstSize = 0, psConstBase = 0, psConstSize = 0;  // SQ_VS_CONST / SQ_PS_CONST bits 0-8, 12-20
};

struct DrawState
{
    DrawKind kind = DrawKind::Refused;
    std::string error;

    // The packet.
    uint32_t opcode = 0, initiator = 0, vizQueryToken = 0;
    uint32_t primitiveType = 0;  // initiator bits 0-5
    HostPrimitive hostPrimitive = HostPrimitive::TriangleList;
    uint32_t sourceSelect = 0, majorMode = 0;  // bits 6-7, 8-9
    IndexBuffer indices;
    uint32_t indexOffset = 0, minIndex = 0, maxIndex = 0;  // VGT_INDX_OFFSET, VGT_MIN/MAX_VTX_INDX, 24-bit (X registers.h:299-360):
                                                           // index = ((i + indexOffset) & 0xFFFFFF) clamped to [minIndex, maxIndex]
    uint32_t edramMode = 0;      // RB_MODECONTROL bits 0-2

    // Fixed function.
    GuestViewport viewport;
    uint32_t systemFlags = 0;     // XeSystem.xe_flags (shader_glsl.h kFlag*): the position format from PA_CL_VTE_CNTL bits 8-10
    Scissor scissor;
    bool cullFront = false, cullBack = false, frontClockwise = false, msaaEnable = false;  // PA_SU_SC_MODE_CNTL bits 0, 1, 2, 15
    bool provokingVertexLast = false;  // bit 19 (flat shading only, which is refused)
    DepthStencil depthStencil;
    Blend blend;
    AlphaTest alphaTest;
    uint32_t colorWriteMask = 0;  // RB_COLOR_MASK bits 0-3 for colour 0 (the translator exports colour 0 only); 0 in DepthOnly
    uint32_t aaMask = 0;          // PA_SC_AA_MASK (recorded; not modelled)
    RenderTarget target;

    // Shader inputs.
    Program program;
    std::vector<VertexSlot> vertexSlots;    // vs.vertexFetchSlots order
    std::vector<TextureSlot> textureSlots;  // every slot either stage samples, ascending; both stages agree
    std::vector<uint32_t> aluConstants;     // 2048 host-order words: VS float4 0-255 at 0, PS 256-511 at 1024 (shader_glsl.h bindings 2/3)
    uint32_t boolConstants[8] = {};         // SHADER_CONSTANT_BOOL_000_031.. (binding 2/3 xe_bool)
    uint32_t loopConstants[32] = {};        // SHADER_CONSTANT_LOOP_00.. (xe_loop)
    std::vector<uint32_t> vsLoopRegisters, psLoopRegisters;  // the loop registers each stage reads (gpu/loop_work.h LoopWorkAllowed)
    uint64_t pixelInvocations = 0;          // the scissor's area (loop_work.h PixelInvocations; the same rule, from the decoded scissor)
};

// Reads register `index` (host order) as the command processor holds it (kernel/graphics.h ReadRegister).
using RegisterReader = uint32_t (*)(const void* context, uint32_t index);

struct DrawInputs
{
    RegisterReader read = nullptr;
    const void* context = nullptr;
    uint32_t opcode = 0;          // 0x22 DRAW_INDX or 0x36 DRAW_INDX_2
    uint32_t vizQueryToken = 0;   // DRAW_INDX's first word; 0 for DRAW_INDX_2
    uint64_t physicalBytes = 0;   // the guest's physical memory size: every address range the draw reads must fit (512 MB on the console)
    const shader::ShaderTranslation* vs = nullptr;
    const shader::ShaderTranslation* ps = nullptr;
};

DrawState DecodeDrawState(const DrawInputs& in);

// Names for messages (X xenos.h:41-57).
const char* PrimitiveTypeName(uint32_t type);

}  // namespace runtime::video
