// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Video imports. Semantics: Xenia xenia-canary 67f16c4e315f:
// src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc, src/xenia/kernel/xam/xam_video.cc, src/xenia/xbox.h:264.
#include "imports.h"
#include "graphics.h"
#include "gpu/gpu_stop.h"
#include "command_processor.h"
#include "virtual_memory.h"
#include <cstdio>
#include "loader.h"
#include "module.h"
#include <cstring>

namespace {

// X_VIDEO_MODE (xbox.h:264, 48 bytes, big-endian): width, height, interlaced, widescreen, high definition,
// refresh rate (float), video standard, two words Xenia sets to 0x4A and 0x01, three reserved words.
// Display policy: Xenia's defaults (xboxkrnl_video.cc:172 VdQueryVideoMode with
// internal_display_resolution 8): 1280x720, progressive, widescreen (1280/4 > 720/3), high definition,
// 60 Hz, NTSC (1).
void WriteVideoMode(uint8_t* base, uint32_t mode)
{
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, 4);
    const uint32_t words[12] = {1280, 720, 0, 1, 1, refreshBits, 1, 0x4A, 0x01, 0, 0, 0};
    for (int i = 0; i < 12; i++)
        runtime::Store32(base, mode + 4 * i, words[i]);
}

}  // namespace

GUEST_IMPORT(VdQueryVideoMode, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:172 VdQueryVideoMode (display policy: Xenia defaults)")
{
    WriteVideoMode(base, ctx.r3.u32);
}

GUEST_IMPORT(XGetVideoMode, "REFERENCE_DERIVED: Xenia xam_video.cc:21 XGetVideoMode (VdQueryVideoMode)")
{
    WriteVideoMode(base, ctx.r3.u32);
}

GUEST_IMPORT(VdInitializeEngines, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:235 VdInitializeEngines (HLE: the runtime's command processor is the engine)")
{
    // (r3 unknown, callback, its argument, PFP microcode*, ME microcode*) -> 1 (Xenia's comment: r3 = 4F810000;
    // FM4 passes 511C0000, recorded, not checked). HLE: the microcode is recorded, not run (graphics.h). Shape
    // checks: the callback is a translated function and both microcode pointers lie in the executable's image;
    // a second call stops (7).
    using namespace runtime;
    // The first GPU entry point: refuse while the GPU stop flag is set (or cannot be checked; gpu/gpu_stop.h fails closed).
    const gpu::GpuStopCheck stopFlag = gpu::CheckGpuStop(gpu::GpuStopPath());
    if (stopFlag.stopped)
        Stop(StopCode::Setup, "VdInitializeEngines: GPU work is stopped (%s): %s", gpu::GpuStopPath() ? gpu::GpuStopPath() : "no --gpu-stop path",
             stopFlag.reason.c_str());
    gpu::Engines& e = gpu::EngineState();
    if (e.initialized)
        Stop(StopCode::Unimplemented, "VdInitializeEngines called a second time");
    const Module& exe = ExecutableModule();
    const uint32_t imageBase = Load32(base, exe.ldr + ldr::kImageBase), imageSize = Load32(base, exe.ldr + ldr::kFullImageSize);
    auto inImage = [&](uint32_t a) { return a >= imageBase && a - imageBase < imageSize; };
    if (!inImage(ctx.r6.u32) || !inImage(ctx.r7.u32) || (ctx.r4.u32 != 0 && FindFunction(base, ctx.r4.u32) == nullptr))
        Stop(StopCode::Unimplemented, "VdInitializeEngines(%08X, callback %08X, %08X, PFP %08X, ME %08X): an unexpected shape",
             ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    e = gpu::Engines{true, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(VdSetGraphicsInterruptCallback, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:267 VdSetGraphicsInterruptCallback, graphics_system.cc:262")
{
    // (callback, user data): the callback runs as callback(source, user data) on the interrupt thread (graphics.h).
    // A callback that is not a translated function stops (7); 0 turns dispatch off (as Xenia).
    if (ctx.r3.u32 != 0 && runtime::FindFunction(base, ctx.r3.u32) == nullptr)
        Stop(StopCode::Unimplemented, "VdSetGraphicsInterruptCallback: %08X is no translated function", ctx.r3.u32);
    runtime::gpu::SetInterruptCallback(base, ctx.r3.u32, ctx.r4.u32);
}

// The display queries report the accepted video-mode policy (720p, 60 Hz).
namespace {
void Store16(uint8_t* base, uint32_t at, uint16_t v) { base[at] = uint8_t(v >> 8), base[at + 1] = uint8_t(v); }
uint32_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
}  // namespace

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:301 (a no-op there; recorded here)")
{
    (void)base;
    runtime::gpu::EngineState().gpuIdentifierAddress = ctx.r3.u32;
}

GUEST_IMPORT(VdGetSystemCommandBuffer, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:294 (0x94 bytes zeroed, tokens BEEF0000 / BEEF0001)")
{
    // (buffer*, token*): HLE: no system command buffer exists; Xenia's opaque tokens, which the guest only hands
    // back to VdSwap.
    std::memset(base + ctx.r3.u32, 0, 0x94);
    runtime::Store32(base, ctx.r3.u32, 0xBEEF0000);
    runtime::Store32(base, ctx.r4.u32, 0xBEEF0001);
}

GUEST_IMPORT(VdGetCurrentDisplayGamma, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:89 (defaults: type 2 BT.709, power 2.22222233)")
{
    runtime::Store32(base, ctx.r3.u32, 2);
    runtime::Store32(base, ctx.r4.u32, FloatBits(2.22222233f));
}

GUEST_IMPORT(VdGetCurrentDisplayInformation, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:145 X_DISPLAY_INFO (0x58 bytes)")
{
    // Zeroed, then: front buffer 1280x720 (+0, +2); scaler source rect x2/y2 (+0x10, +0x14), scaled output 1280x720
    // (+0x18, +0x1C), vertical and horizontal filter type 1 (+0x20, +0x30); overscan 320, 180, 320, 180 (+0x40..);
    // display 1280x720 (+0x48, +0x4A); refresh 60.0f (+0x4C); actual display width 1280 (+0x56).
    const uint32_t info = ctx.r3.u32;
    std::memset(base + info, 0, 0x58);
    Store16(base, info, 1280), Store16(base, info + 2, 720);
    runtime::Store32(base, info + 0x10, 1280), runtime::Store32(base, info + 0x14, 720);
    runtime::Store32(base, info + 0x18, 1280), runtime::Store32(base, info + 0x1C, 720);
    runtime::Store32(base, info + 0x20, 1), runtime::Store32(base, info + 0x30, 1);
    Store16(base, info + 0x40, 320), Store16(base, info + 0x42, 180), Store16(base, info + 0x44, 320), Store16(base, info + 0x46, 180);
    Store16(base, info + 0x48, 1280), Store16(base, info + 0x4A, 720);
    runtime::Store32(base, info + 0x4C, FloatBits(60.0f));
    Store16(base, info + 0x56, 1280);
}

GUEST_IMPORT(VdQueryVideoFlags, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:195 (widescreen 1 | width >= 1024 2 | width >= 1920 4)")
{
    (void)base;
    ctx.r3.u64 = 1 | 2;  // 1280x720 widescreen
}

GUEST_IMPORT(VdSetDisplayMode, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:208 (returns 0); ForzaRecomp: only HD RGB")
{
    // (flags): bit 1 = a 720x480/576 mode, bits 28-29 = the colour space. Only the accepted mode (HD, RGB) is
    // modelled; anything else stops (7).
    (void)base;
    if (ctx.r3.u32 & 0x30000002)
        Stop(StopCode::Unimplemented, "VdSetDisplayMode(%08X): only a high-definition RGB mode is modelled", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdInitializeScalerCommandBuffer, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:312 (type-2 NOP fill, returns the count)")
{
    // (12 arguments; destination* is argument 10, its word count argument 11): headless, so the scaler commands
    // are type-2 no-op packets (2 << 30), as Xenia.
    const uint32_t dest = runtime::Argument(ctx, base, 10), count = runtime::Argument(ctx, base, 11);
    for (uint32_t i = 0; i < count; i++)
        runtime::Store32(base, dest + 4 * i, 2u << 30);  // a type-2 packet
    ctx.r3.u64 = count;
}

GUEST_IMPORT(VdCallGraphicsNotificationRoutines, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:352 (asserts the first argument is 1; returns 0)")
{
    // HYPOTHESIS (as Xenia): no notification routine is called. A first argument other than 1 stops (7).
    (void)base;
    if (ctx.r3.u32 != 1)
        Stop(StopCode::Unimplemented, "VdCallGraphicsNotificationRoutines(%u, %08X): only the first argument 1 is modelled", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdIsHSIOTrainingSucceeded, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:363 (1)")
{
    (void)base;
    ctx.r3.u64 = 1;  // the headless GPU link has nothing to train
}

GUEST_IMPORT(VdRetrainEDRAM, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:387 (0)")
{
    (void)base;
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdRetrainEDRAMWorker, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:384 (0)")
{
    (void)base;
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdInitializeRingBuffer, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:277, command_processor.cc:340 InitializeRingBuffer")
{
    // (physical address, log2 of the size in quadwords: 1 << (log2 + 3) bytes).
    runtime::gpu::InitializeRing(base, ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:286, command_processor.cc:349")
{
    (void)base;
    runtime::gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdSwap, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:394-476 VdSwap (writes the swap packets into the guest's ring slot)")
{
    // (ring slot*, fetch constant*, system write-back*, system buffer*, token*, front buffer*, format*, colour space*,
    // width*, height*). The fetch constant from the Direct3D texture header (6 big-endian words) holds the front
    // buffer's virtual address >> 12 in dword 1 bits 12-31 and its size - 1 in dword 2 (width bits 0-12, height
    // 13-25; xenos.h:1206-1307). Checks Xenia asserts (a mismatch stops, 7): the address equals *front buffer and
    // has a physical address, the format is 8_8_8_8 (6) or 2_10_10_10_AS_16_16_16_16 (54), colour space 0, the
    // sizes match. The 64-word slot becomes: a type-0 packet writing the 6 fetch words (base = physical >> 12) to
    // SHADER_CONSTANT_FETCH_00_0 (4800), XE_SWAP {"SWAP", physical, width, height}, then type-2 no-ops.
    using namespace runtime;
    const uint32_t slot = ctx.r3.u32, fetchAt = ctx.r4.u32;
    const uint32_t frontAt = ctx.r8.u32, formatAt = ctx.r9.u32, colourAt = ctx.r10.u32;
    const uint32_t width = Load32(base, Argument(ctx, base, 8)), height = Load32(base, Argument(ctx, base, 9));
    std::fprintf(stderr, "[runtime] VdSwap entered (slot %08X)\n", slot);
    uint32_t fetch[6];
    for (int i = 0; i < 6; i++)
        fetch[i] = Load32(base, fetchAt + 4 * i);
    const uint32_t virtualAddress = fetch[1] & 0xFFFFF000u, format = Load32(base, formatAt);
    uint32_t physical = 0;
    if (virtualAddress != Load32(base, frontAt) || !PhysicalAddress(virtualAddress, physical) || (format != 6 && format != 54) ||
        Load32(base, colourAt) != 0 || width != (fetch[2] & 0x1FFF) + 1 || height != ((fetch[2] >> 13) & 0x1FFF) + 1)
        Stop(StopCode::Unimplemented, "VdSwap: an unexpected front buffer (fetch %08X %08X, *front %08X, format %u, %ux%u)",
             fetch[1], fetch[2], Load32(base, frontAt), format, width, height);
    fetch[1] = (fetch[1] & 0xFFFu) | (physical & 0xFFFFF000u);
    uint32_t words[64];
    uint32_t n = 0;
    words[n++] = (5u << 16) | gpu::reg::kShaderConstantFetch00;  // type 0: 6 registers from 4800 (MakePacketType0)
    for (int i = 0; i < 6; i++)
        words[n++] = fetch[i];
    words[n++] = (3u << 30) | (3u << 16) | (0x64u << 8);  // type 3: XE_SWAP, 4 words (MakePacketType3)
    words[n++] = 0x53574150, words[n++] = physical, words[n++] = width, words[n++] = height;
    while (n < 64)
        words[n++] = 2u << 30;  // type 2: no-op
    for (uint32_t i = 0; i < 64; i++)
        Store32(base, slot + 4 * i, words[i]);
}

GUEST_IMPORT(VdPersistDisplay, "REFERENCE_DERIVED: Xenia xboxkrnl_video.cc:369 VdPersistDisplay (a 64-byte no-access physical block, 1)")
{
    // (unknown, block*): *block receives a physical allocation the guest later frees with MmFreePhysicalMemory(1, *block);
    // Xenia allocates 64 bytes, no access, from the physical heap it finds for 16 KB pages: here the 64 KB-page view
    // (A0000000), one page. Returns 1.
    using namespace runtime;
    if (ctx.r4.u32 != 0)
    {
        const uint32_t block = PhysicalHeap(64 * 1024)->Alloc(0x10000, 0x10000, mem::kReserve | mem::kCommit, page::kNoAccess, false);
        if (block == 0)
            Stop(StopCode::Unimplemented, "VdPersistDisplay: no physical page left");
        Store32(base, ctx.r4.u32, block);
    }
    ctx.r3.u64 = 1;
}
