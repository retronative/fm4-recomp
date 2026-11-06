// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// The fence-wait policy: what a bounded wait for a submission established, and how long it waits.
// WaitOutcome is taken from ForzaRecomp (forzarecomp-recarbon) runtime/video/submission_lifetime.h
// (GPL-3.0-or-later, same owner), reviewed and re-tested here; its use is
// runtime/video/renderer.cpp:979-980 (vkWaitForFences, then vkGetFenceStatus). ForzaRecomp's
// default was 5 s (renderer.cpp:388-390, kCompletionTimeoutNs; FM4's longest observed wait
// there was 6.7 ms); ours is 2 s (forzarecomp-recarbon
// incidents used 2000 ms without false trips). FM4 Recomp changes: the result
// is named Stranded (ForzaRecomp: Unknown); the timeout comes from an environment value, clamped to
// 100..60000 ms (new); the 2 s default.
//
// A bounded fence wait's result and the fence's status afterwards are VkResult values (0 is
// VK_SUCCESS): confirmed only when both succeeded. A timeout (VK_TIMEOUT 2), a status not yet
// signalled (VK_NOT_READY 1), a lost device (VK_ERROR_DEVICE_LOST -4) or any other error leaves the
// completion unknown: the submission is stranded (its resources are kept, never freed or retried).
#pragma once
#include <algorithm>
#include <cstdint>

namespace runtime::gpu {

enum class Completion
{
    Confirmed,  // the fence signalled: every command of the submission has finished on the GPU
    Stranded,   // the submit failed, or the wait failed or timed out: the GPU may still use it
};

inline Completion WaitOutcome(int waitResult, int fenceStatus)
{
    return waitResult == 0 && fenceStatus == 0 ? Completion::Confirmed : Completion::Stranded;
}

constexpr uint32_t kFenceTimeoutDefaultMs = 2000, kFenceTimeoutMinMs = 100, kFenceTimeoutMaxMs = 60000;

struct FenceTimeout
{
    uint32_t ms = kFenceTimeoutDefaultMs;
    bool malformed = false;  // the value was given but is not a decimal number: the default is used, the caller reports it
};

// The fence-wait limit from an environment value (the caller passes getenv's result): unset is the
// default; decimal digits only, clamped to 100..60000 ms; anything else (empty, a sign, spaces,
// trailing text) is malformed and falls back to the default. Large numbers saturate, never wrap.
inline FenceTimeout FenceTimeoutFromEnv(const char* value)
{
    if (value == nullptr)
        return {};
    if (*value == '\0')
        return {kFenceTimeoutDefaultMs, true};
    uint64_t ms = 0;
    for (const char* p = value; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return {kFenceTimeoutDefaultMs, true};
        if (ms <= kFenceTimeoutMaxMs)
            ms = ms * 10 + uint64_t(*p - '0');
    }
    return {uint32_t(std::clamp<uint64_t>(ms, kFenceTimeoutMinMs, kFenceTimeoutMaxMs)), false};
}

constexpr uint64_t FenceTimeoutNs(uint32_t ms) { return uint64_t(ms) * 1000000; }

}  // namespace runtime::gpu
