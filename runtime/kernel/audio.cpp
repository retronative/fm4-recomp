// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAudio render-driver clients (audio.h).
#include "audio.h"
#include "arena.h"
#include "graphics.h"
#include "imports.h"
#include "indirect.h"
#include "loader.h"
#include "threads.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace runtime::audio {
namespace {
std::mutex lock;
std::condition_variable permitWake, callbackDone;
Sink sink = Sink::Unset;
bool registered = false;
uint32_t callback = 0, wrappedArgument = 0, permits = 0;
std::deque<std::vector<uint8_t>> frames;  // submitted, guest layout
Counters counters;
bool clockStarted = false;
std::chrono::steady_clock::time_point firstPull;
uint64_t firstVblank = 0;

// The worker: one guest callback per permit (audio_system.cc:96-145). CPU 0 is a HYPOTHESIS: Xenia's host thread
// sets no affinity.
void Worker(uint8_t* base, PPCContext& initial)
{
    for (;;)
    {
        {
            std::unique_lock guard(lock);
            permitWake.wait(guard, [] { return permits > 0; });
            permits--;
        }
        PPCContext ctx = initial;
        ctx.r3.u64 = wrappedArgument;
        static bool entered = false;
        if (!entered)
            entered = true, std::fprintf(stderr, "[audio] first callback entered (%08X)\n", callback);
        PpcCallIndirect(ctx, base, callback);
        uint64_t made;
        {
            std::lock_guard guard(lock);
            made = ++counters.callbacks;
        }
        callbackDone.notify_all();
        if (made == 1 || made == kQueuedFrames)
            Report(made == 1 ? "first callback returned" : "the 64 initial callbacks returned");
    }
}

std::function<void(const float*)> output;  // audible: the SDL device; empty: mute
std::function<double()> outputQueuedMs;

// The clock: one frame every 256 / 48000 s on absolute deadlines (frame n is due at n * 256 / 48000 s), in both
// modes; audible hands the frame on.
void Clock()
{
    using namespace std::chrono;
    const auto origin = steady_clock::now();
    std::vector<float> frame(kFrameFloats);
    for (uint64_t n = 1;; n++)
    {
        std::this_thread::sleep_until(origin + nanoseconds(n * kChannelSamples * 1000000000ull / kSampleRate));
        ConsumeFrame(frame.data());
        if (output)
            output(frame.data());
    }
}
}  // namespace

void SetOutput(std::function<void(const float*)> out, std::function<double()> queuedMs)
{
    output = std::move(out), outputQueuedMs = std::move(queuedMs);
}

void SetSink(Sink s)
{
    std::lock_guard guard(lock);
    sink = s;
}

void Interleave(const uint8_t* guestFrame, float* out)
{
    for (uint32_t channel = 0; channel < kChannels; channel++)
        for (uint32_t sample = 0; sample < kChannelSamples; sample++)
        {
            const uint8_t* b = guestFrame + (channel * kChannelSamples + sample) * 4;
            const uint32_t bits = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
            std::memcpy(&out[sample * kChannels + channel], &bits, 4);
        }
}

uint32_t RegisterClient(uint8_t* base, uint32_t routine, uint32_t argument)
{
    std::unique_lock guard(lock);
    if (sink == Sink::Unset)
        Stop(StopCode::Setup, "XAudioRegisterRenderDriverClient: no audio sink (fm4recomp --audio audible|mute)");
    if (registered)
        Stop(StopCode::Unimplemented, "XAudioRegisterRenderDriverClient: a second render client is not implemented");
    if (FindFunction(base, routine) == nullptr)
        Stop(StopCode::Unimplemented, "XAudioRegisterRenderDriverClient: callback %08X is no translated function", routine);
    // The wrapped argument: a guest word holding `argument`, passed to the callback (audio_system.cc:192-193).
    wrappedArgument = ArenaAlloc(base, 4);
    Store32(base, wrappedArgument, argument);
    callback = routine;
    registered = true;
    permits = kQueuedFrames;
    const bool startClock = sink == Sink::Clock && !clockStarted;
    clockStarted = clockStarted || startClock;
    guard.unlock();
    std::fprintf(stderr, "[audio] render client registered: callback %08X, argument %08X (wrapped at %08X), %u permits\n", routine,
                 argument, wrappedArgument, kQueuedFrames);
    StartServiceThread(base, 0, [base](PPCContext& ctx) { Worker(base, ctx); });
    if (startClock)
        std::thread(Clock).detach();
    permitWake.notify_one();
    return kDriverTag | 0;
}

void SubmitFrame(const uint8_t* base, uint32_t driver, uint32_t samples)
{
    std::vector<uint8_t> frame(base + samples, base + samples + kFrameBytes);
    std::lock_guard guard(lock);
    if (!registered || driver != (kDriverTag | 0))
        Stop(StopCode::Unimplemented, "XAudioSubmitRenderDriverFrame: driver %08X is not the registered client", driver);
    if (frames.size() >= kMaxQueued)
        Stop(StopCode::Unimplemented, "XAudioSubmitRenderDriverFrame: %u frames wait for the output (more than permits allow)", kMaxQueued);
    frames.push_back(std::move(frame));
    if (++counters.submitted == 1)
        std::fprintf(stderr, "[audio] first frame submitted\n");
}

bool ConsumeFrame(float* out)
{
    std::vector<uint8_t> frame;
    const char* report = nullptr;
    {
        std::lock_guard guard(lock);
        if (!registered)
        {
            std::memset(out, 0, kFrameBytes);
            return false;
        }
        if (counters.consumed + counters.silent == 0)
            firstPull = std::chrono::steady_clock::now(), firstVblank = gpu::VblankCount();
        if (frames.empty())
            counters.silent++;
        else
        {
            frame = std::move(frames.front());
            frames.pop_front();
            counters.consumed++;
            if (permits < kQueuedFrames)  // the semaphore's maximum (audio_system.cc:52)
                permits++;
        }
        if (counters.consumed == 1 && !frame.empty())
            report = "first frame played";
        else if ((counters.consumed + counters.silent) % 375 == 0)  // 375 * 256 / 48000 = 2 s
            report = "periodic";
    }
    if (frame.empty())
        std::memset(out, 0, kFrameBytes);
    else
    {
        Interleave(frame.data(), out);
        permitWake.notify_one();
    }
    if (report)
        Report(report);
    return !frame.empty();
}

Counters GetCounters()
{
    std::lock_guard guard(lock);
    Counters c = counters;
    c.permits = permits, c.queued = uint32_t(frames.size());
    return c;
}

void Report(const char* when)
{
    const Counters c = GetCounters();
    std::lock_guard guard(lock);
    if (!registered)
    {
        std::fprintf(stderr, "[audio] %s: no render client registered\n", when);
        return;
    }
    const uint64_t pulled = c.consumed + c.silent;
    const double outputSeconds = double(pulled) * kChannelSamples / kSampleRate;
    const double wall = pulled ? std::chrono::duration<double>(std::chrono::steady_clock::now() - firstPull).count() : 0.0;
    const double video = pulled ? double(gpu::VblankCount() - firstVblank) / 60.0 : 0.0;
    char device[48] = "mute";
    if (outputQueuedMs)
        std::snprintf(device, sizeof device, "device queue %.1f ms", outputQueuedMs());
    std::fprintf(stderr, "[audio] %s: callbacks %llu, submitted %llu, consumed %llu, silent %llu, permits %u, queued %u; "
                 "output %.3f s, wall %.3f s, vblanks/60 %.3f s since the first pull; %s\n", when,
                 (unsigned long long)c.callbacks, (unsigned long long)c.submitted, (unsigned long long)c.consumed,
                 (unsigned long long)c.silent, c.permits, c.queued, outputSeconds, wall, video, device);
}

bool WaitForCallbacks(uint64_t target, int ms)
{
    std::unique_lock guard(lock);
    return callbackDone.wait_for(guard, std::chrono::milliseconds(ms), [&] { return counters.callbacks >= target; });
}

}  // namespace runtime::audio
