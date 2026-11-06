// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Audio: the speaker query and the XAudio render-driver client (kernel/audio.h). Output is audible (SDL) or muted
// with the same pacing and callbacks (fm4recomp --audio, audio policy). Unregistering, the ducker and
// voice-category change mask stay strict stops until a run reaches them. XMA contexts: kernel/xma.h. Semantics: Xenia xenia-canary
// 67f16c4e315f src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc.
#include "audio.h"
#include "imports.h"
#include "xma.h"
#include <atomic>

namespace {
constexpr uint32_t kInvalidArg = (1u << 31) | (7u << 16) | 0x57;  // X_E_INVALIDARG (xbox.h)
}  // namespace

GUEST_IMPORT(XAudioGetSpeakerConfig, "REFERENCE_DERIVED: Xenia xboxkrnl_audio.cc:22 (0x00010001, success)")
{
    // (config*): Xenia's value 0x00010001; its meaning (stereo, no digital output) is a HYPOTHESIS.
    runtime::Store32(base, ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioRegisterRenderDriverClient, "REFERENCE_DERIVED: Xenia xboxkrnl_audio.cc:55-79, audio_system.cc:172-200")
{
    // (callback_ptr* = {callback, argument}, driver*): both words are read now (the block may be on the caller's
    // stack); a null block or callback is X_E_INVALIDARG; otherwise *driver = 0x41550000 | index, 0.
    const uint32_t block = ctx.r3.u32, driverOut = ctx.r4.u32;
    if (block == 0 || runtime::Load32(base, block) == 0)
    {
        ctx.r3.u64 = kInvalidArg;
        return;
    }
    const uint32_t driver = runtime::audio::RegisterClient(base, runtime::Load32(base, block), runtime::Load32(base, block + 4));
    runtime::Store32(base, driverOut, driver);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioSubmitRenderDriverFrame, "REFERENCE_DERIVED: Xenia xboxkrnl_audio.cc:94-103, audio_system.cc:202-209, sdl_audio_driver.cc:89-108")
{
    // (driver, samples*): copies one frame (256 x 6 big-endian floats) now; 0.
    runtime::audio::SubmitFrame(base, ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolume, "REFERENCE_DERIVED: Xenia xboxkrnl_audio.cc:41-48 (1.0f, success; a Xenia stub)")
{
    // (category, float* volume): compatibility policy (audio policy): there is no system mixer, so every
    // voice category is at full volume, 1.0f (big-endian 3F800000). What the categories are is UNRESOLVED; Xenia
    // ignores the category too.
    runtime::Store32(base, ctx.r4.u32, 0x3F800000);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XMACreateContext, "REFERENCE_DERIVED: Xenia xboxkrnl_audio_xma.cc:61-69, xma_decoder.cc:220-231 (lowest free context)")
{
    // (context**): the guest address of a free context of the kernel's array, 0; or 0 and STATUS_NO_MEMORY when all
    // 320 are taken.
    const uint32_t context = runtime::xma::CreateContext(base);
    runtime::Store32(base, ctx.r3.u32, context);
    ctx.r3.u64 = context ? 0 : ((3u << 30) | 0x17);
}

GUEST_IMPORT(XMAReleaseContext, "REFERENCE_DERIVED: Xenia xboxkrnl_audio_xma.cc:72-76, xma_context.cc:168-176 (zeroed)")
{
    // (context*): zeroes and frees it; 0. Anything but an allocated context stops (Xenia asserts).
    runtime::xma::ReleaseContext(base, ctx.r3.u32);
    ctx.r3.u64 = 0;
}

namespace {
std::atomic<uint32_t> duckerSetting{0};
}  // namespace

GUEST_IMPORT(XAudioEnableDucker, "REFERENCE_DERIVED: Xenia xboxkrnl_audio.cc:52-53 (X_ERROR_SUCCESS); no voice chat to duck under")
{
    // (setting): enables lowering the title's audio under voice chat. No voice chat or system audio exists here
    // (XamVoiceIsActiveProcess 0), so there is nothing to duck: the setting is recorded and the call succeeds, as Xenia.
    duckerSetting = ctx.r3.u32;
    ctx.r3.u64 = 0;
}
