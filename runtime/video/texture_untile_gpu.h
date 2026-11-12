// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, opt-in FM4_TEXTURE_GPU_UNTILE=1): the GPU texture load's CPU side, without a Vulkan
// header so tests/runtime/test_texture_untile_gpu.cpp builds without one. texture_vk.cpp records the dispatch.
//
// The load is a pure byte permutation of guest memory: staging byte p of a level (tightly packed rows of widthBlocks blocks, as
// FillStaging writes them) is guest byte XeUntileSourceByte(p) of the level's storage. One compute invocation assembles one
// staging dword from four byte gathers, so 1- and 2-byte blocks, rows that are not a dword multiple and packed-mip origins need
// no special case. The same function body (FM4_UNTILE_GPU_BODY) is the shader's code (stringified into kUntileGpuGlsl) and the
// C++ model the test compares exhaustively with FillStaging and with Xenia's GetTiledOffset2D, so the two cannot drift.
//
// After ReCarbon runtime/gpu/renderer.cpp:3255-3257 (texture loads untiled by a compute shader from the imported guest
// memory into the ring, then the buffer -> image copy) and :3398-3519 (its kUntileGlsl), GPL-3.0-or-later. FM4 Recomp changes:
// one dword per invocation (ReCarbon: one block or one dword per invocation on an 8x8 grid), no format conversion (anything
// FillStaging converts stays on the CPU), the tiled offset is texture_untile.h's TiledOffset2D (Xenia 67f16c4e315f
// texture_util.cc:443-458) rather than ReCarbon's bit-field form, the endian as an xor of the byte index (SwapBlockEndian's
// permutations), one dispatch per level with sub-range bindings, and a CPU fallback for everything else.
#pragma once
#include "texture_info.h"
#include "texture_cache.h"
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

// The shared body: valid GLSL 450 and valid C++ once `uint` is uint32_t and XeGuestDword(uint dwordIndex) is declared before it.
// Expanded as code by the test's model; stringified (comments are gone by then) into the shader.
//  XeUntileTiledOffset2D: texture_untile.h TiledOffset2D (texture_util.cc:443-458 GetTiledOffset2D) token for token.
//  XeUntileSourceByte: block p >> log2 at (x0 + block % widthBlocks, y0 + block / widthBlocks); the byte inside it xored with
//    swapMask (0 none, 1 8in16, 3 8in32, 2 16in32 on blocks of 4 bytes and more; 1 for 8in16 on 2-byte blocks), which is
//    SwapBlockEndian's permutation of each dword ({1,0,3,2}, {3,2,1,0}, {2,3,0,1}); linear storage: y * rowPitchBytes + x bytes.
//  XeUntileOutputDword: staging dword w; bytes at or past levelBytes are 0 and read nothing (the last dword of a level whose size
//    is not a multiple of 4; it lands in the 16-byte alignment gap before the next level, or the 4 bytes reserved past the last).
#define FM4_UNTILE_GPU_BODY                                                                                                          \
    uint XeUntileTiledOffset2D(uint x, uint y, uint pitchBlocks, uint log2Bytes)                                                     \
    {                                                                                                                                \
        uint pitch = (pitchBlocks + 31u) & ~31u;                                                                                     \
        uint macroOffset = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (log2Bytes + 7u);                                             \
        uint microOffset = ((x & 7u) + ((y & 14u) << 2u)) << log2Bytes;                                                              \
        uint tiledOffset = macroOffset + ((microOffset & ~15u) << 1u) + (microOffset & 15u) + ((y & 1u) << 4u);                     \
        return ((tiledOffset & ~511u) << 3u) + ((y & 16u) << 7u) + ((tiledOffset & 448u) << 2u) +                                   \
               (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (tiledOffset & 63u);                                                 \
    }                                                                                                                                \
    uint XeUntileSourceByte(uint p, uint widthBlocks, uint x0, uint y0, uint pitch, uint log2Bytes, uint tiled, uint swapMask)       \
    {                                                                                                                                \
        uint block = p >> log2Bytes;                                                                                                 \
        uint inBlock = (p & ((1u << log2Bytes) - 1u)) ^ swapMask;                                                                    \
        uint x = x0 + block % widthBlocks;                                                                                           \
        uint y = y0 + block / widthBlocks;                                                                                           \
        uint at = tiled != 0u ? XeUntileTiledOffset2D(x, y, pitch, log2Bytes) : y * pitch + (x << log2Bytes);                        \
        return at + inBlock;                                                                                                         \
    }                                                                                                                                \
    uint XeUntileOutputDword(uint w, uint levelBytes, uint srcByte, uint widthBlocks, uint x0, uint y0, uint pitch, uint log2Bytes,  \
                             uint tiled, uint swapMask)                                                                              \
    {                                                                                                                                \
        uint v = 0u;                                                                                                                 \
        for (uint k = 0u; k < 4u; k++)                                                                                               \
        {                                                                                                                            \
            uint p = w * 4u + k;                                                                                                     \
            if (p < levelBytes)                                                                                                      \
            {                                                                                                                        \
                uint a = srcByte + XeUntileSourceByte(p, widthBlocks, x0, y0, pitch, log2Bytes, tiled, swapMask);                     \
                v |= ((XeGuestDword(a >> 2u) >> ((a & 3u) * 8u)) & 255u) << (k * 8u);                                                \
            }                                                                                                                        \
        }                                                                                                                            \
        return v;                                                                                                                    \
    }

constexpr uint32_t kUntileGpuGroupSize = 128;  // local_size_x: Vulkan's guaranteed maxComputeWorkGroupInvocations is 128

// The compute shader (GLSL 450): binding 0 the guest memory sub-range, binding 1 the ring sub-range, push constants
// GpuUntilePush. Compiled once at InitializeTextureUnit.
std::string UntileGpuGlsl();

// The push constants of one level's dispatch (std430-compatible: eleven uints, the shader's XeUntilePush in this order).
struct GpuUntilePush
{
    uint32_t srcByte = 0;      // the level's address minus the guest binding's offset
    uint32_t dstWord = 0;      // the level's staging offset minus the ring binding's offset, in dwords
    uint32_t dwords = 0;       // ceil(levelBytes / 4): invocations that write
    uint32_t levelBytes = 0;
    uint32_t widthBlocks = 0, x0 = 0, y0 = 0;
    uint32_t pitch = 0;        // tiled: the pitch in blocks; linear: the row pitch in bytes
    uint32_t log2Bytes = 0;
    uint32_t tiled = 0;
    uint32_t swapMask = 0;
};
static_assert(sizeof(GpuUntilePush) == 11 * 4);

// One level of a GPU load: where it reads, where it writes, and its push constants (srcByte / dstWord filled by GpuUntileBind).
struct GpuUntileLevel
{
    uint32_t address = 0;      // physical
    uint64_t extentBytes = 0;  // MipLevel::extentBytes: the guest bytes the level may read
    uint64_t levelOffset = 0;  // LoadPlan::levelOffsets
    GpuUntilePush push;
    uint32_t groups = 0;       // ceil(dwords / kUntileGpuGroupSize)
};

// The byte-index xor of SwapBlockEndian's permutation for `endian` on 2^log2Bytes-byte blocks (EndianSupported must hold).
uint32_t UntileSwapMask(uint32_t log2Bytes, uint32_t endian);

// Whether texture `d` loads on the GPU, and each level's dispatch. False with `why` (FillStaging then loads it) for: a 3D or cube
// texture, a converted format (k_24_8_FLOAT's 20e4 depth), a plan that does not match the levels, an unsupported endian, a level
// FillStaging would refuse (DetileBlocksAt / CopyLinearBlocksAt's conditions, checked here because the GPU cannot refuse), a
// staging offset that is not a dword multiple, or a level needing more than `maxGroupsX` workgroups.
bool PlanGpuUntile(const TextureDescription& d, const LoadPlan& plan, uint32_t maxGroupsX, std::vector<GpuUntileLevel>& levels, std::string& why);

// The descriptor ranges of one level: the guest binding at the level's address aligned down to `storageAlign`, covering its extent
// in whole dwords; the ring binding at the level's staging offset aligned down, covering its bytes in whole dwords. False when
// `storageAlign` is not a power of two, a range leaves its buffer or exceeds `maxRange`, or the staging residual is not a dword
// multiple; then nothing is recorded (the caller loads on the CPU).
struct GpuUntileBinding
{
    uint64_t guestOffset = 0, guestRange = 0, ringOffset = 0, ringRange = 0;
};
bool GpuUntileBind(GpuUntileLevel& level, uint64_t ringLevelOffset, uint64_t guestBufferBytes, uint64_t ringBytes, uint64_t storageAlign,
                   uint64_t maxRange, GpuUntileBinding& b);

// The ring bytes a GPU load reserves: the staging rounded up to a dword (the last level's last dword is written whole).
constexpr uint64_t GpuUntileStagingBytes(uint64_t stagingBytes) { return (stagingBytes + 3) & ~uint64_t(3); }

// The guard on guest memory the GPU reads at execution time (the guest-memory draft's gpu_read_guard.h, behind texture_vk.cpp's
// adapter): true when [physical, physical + bytes) is write-protected and any later guest store or host write to it is held until
// the part `serial` completes on the GPU; false when the range cannot be guarded.
using GuestReadGuardFn = bool (*)(void* context, uint32_t physical, uint32_t bytes, uint64_t serial);

// The load path of one texture upload: the GPU only when the switch is on, the texture reads no pending resolve write-back (its
// guest bytes are not final until that part retires: today's refusal / flush applies), PlanGpuUntile accepted it, and the guard
// took every guest range it reads (level 0, then the mips). Anything else: FillStaging, a snapshot at the bind.
struct GpuLoadChoice
{
    bool gpu = false;
    const char* why = "";
};
GpuLoadChoice ChooseTextureLoad(bool enabled, bool readsPending, bool planned, const TextureDescription& d, uint64_t serial, GuestReadGuardFn guard,
                                void* context);

}  // namespace runtime::video
