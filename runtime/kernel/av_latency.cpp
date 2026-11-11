// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4_DIAG_AV_LATENCY (av_latency.h). Diagnostic only.
#include "av_latency.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace runtime::av {
namespace {
std::mutex lock;
constexpr uint64_t kFollowNs = 2'000'000'000ull;  // follow a press for 2 s
uint64_t pressNs = 0, pressCount = 0, pressVblank = 0;
double previousRms = 0;
// The onset candidate after the current press: the submitted frame with the largest level rise over its predecessor.
uint64_t onsetSequence = 0, onsetSubmitNs = 0;
uint32_t onsetQueued = 0;
double onsetRise = 0, onsetRms = 0;
bool onsetReported = false;

double Db(double v) { return v > 0 ? 20.0 * std::log10(v) : -999.0; }
}  // namespace

bool Enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_AV_LATENCY");
        const bool enabled = v != nullptr && !std::strcmp(v, "1");
        if (enabled)
            std::fprintf(stderr, "[av] FM4_DIAG_AV_LATENCY=1: A presses, the swaps and the audio onset after each, on one clock (diagnostic)\n");
        return enabled;
    }();
    return on;
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void APressed(uint64_t vblank)
{
    if (!Enabled())
        return;
    std::lock_guard guard(lock);
    // The previous press's onset, if it was never pulled (a new press comes first), is named as such.
    if (pressNs != 0 && onsetSequence != 0 && !onsetReported)
        std::fprintf(stderr, "[av] press %llu: onset frame %llu not yet played when the next press came\n", (unsigned long long)pressCount,
                     (unsigned long long)onsetSequence);
    pressNs = NowNs(), pressVblank = vblank, pressCount++;
    onsetSequence = 0, onsetRise = 0, onsetReported = false;
    std::fprintf(stderr, "[av] press %llu: A down at vblank %llu (t 0)\n", (unsigned long long)pressCount, (unsigned long long)vblank);
}

void Swap(uint64_t swap, uint64_t draws)
{
    if (!Enabled())
        return;
    std::lock_guard guard(lock);
    const uint64_t now = NowNs();
    if (pressNs == 0 || now - pressNs > kFollowNs)
        return;
    std::fprintf(stderr, "[av] press %llu: swap %llu at +%.1f ms, %llu draws\n", (unsigned long long)pressCount, (unsigned long long)swap,
                 (now - pressNs) / 1e6, (unsigned long long)draws);
}

void AudioSubmitted(uint64_t sequence, double rms, uint32_t queuedAhead)
{
    if (!Enabled())
        return;
    std::lock_guard guard(lock);
    const uint64_t now = NowNs();
    const double rise = Db(std::max(rms, 1e-6)) - Db(std::max(previousRms, 1e-6));
    previousRms = rms;
    if (pressNs == 0 || now - pressNs > kFollowNs || onsetReported)
        return;
    if (rms > 1e-4 && rise > onsetRise)  // above -80 dBFS: the largest rise so far after this press
        onsetSequence = sequence, onsetSubmitNs = now, onsetQueued = queuedAhead, onsetRise = rise, onsetRms = rms;
}

void AudioPulled(uint64_t sequence, double deviceQueueMs)
{
    if (!Enabled())
        return;
    std::lock_guard guard(lock);
    const uint64_t now = NowNs();
    // A frame is final as the onset once it is pulled and no later candidate replaced it within the follow window; the
    // report is for the candidate standing when its own frame is pulled.
    if (onsetSequence == 0 || sequence != onsetSequence || onsetReported)
        return;
    onsetReported = true;
    std::fprintf(stderr, "[av] press %llu: audio onset frame %llu (rise %+.1f dB to %.1f dBFS) submitted at +%.1f ms with %u frame(s) = %.1f ms queued "
                 "ahead; pulled by the output clock at +%.1f ms; device queue %s -> heard no earlier than +%.1f ms (+ the device's own latency)\n",
                 (unsigned long long)pressCount, (unsigned long long)sequence, onsetRise, Db(onsetRms), (onsetSubmitNs - pressNs) / 1e6, onsetQueued,
                 onsetQueued * 256.0 / 48.0, (now - pressNs) / 1e6,
                 deviceQueueMs < 0 ? "none (mute)" : (std::to_string(deviceQueueMs).substr(0, 5) + " ms").c_str(),
                 (now - pressNs) / 1e6 + (deviceQueueMs < 0 ? 0 : deviceQueueMs));
}

}  // namespace runtime::av
