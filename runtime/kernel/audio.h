// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAudio render-driver clients: the title's mixer submits 256-sample, 6-channel float frames and is called back
// once per frame the output has consumed. REFERENCE_DERIVED from Xenia xenia-canary 67f16c4e315f
// src/xenia/apu/audio_system.cc (RegisterClient :172, SubmitFrame :202, WorkerThreadMain :89),
// apu/sdl/sdl_audio_driver.cc (frame format :55-58, SDLCallback :130), apu/conversion.h (frame layout) and
// kernel/xboxkrnl/xboxkrnl_audio.cc:55-105 (the imports):
//   - a frame is 256 samples of 6 channels, channel-major (channel * 256 + sample), big-endian IEEE floats,
//     played at 48 kHz (5.333 ms a frame);
//   - registering gives the client 64 permits (apu_max_queued_frames, :38, :52, :182); the worker calls
//     callback(wrapped argument) once per permit, and each frame the output consumes returns one permit; an
//     empty queue plays silence and returns none.
// Not modelled (each stops when reached): a second client (Xenia allows 8, each with its own device),
// unregistering, more than kMaxQueued frames waiting. XMA decoding: kernel/xma.h.
// Output: a 48 kHz clock here pulls one frame every 256 samples with ConsumeFrame in both modes (the cadence never
// depends on a device: the guest's sound runs on it). Audible mode
// also hands each frame to the SDL device (runtime/audio_sdl.cpp); mute opens no device (audio policy).
#pragma once
#include <cstdint>
#include <functional>

namespace runtime::audio {

constexpr uint32_t kSampleRate = 48000, kChannels = 6, kChannelSamples = 256;
constexpr uint32_t kFrameFloats = kChannels * kChannelSamples, kFrameBytes = kFrameFloats * 4;
constexpr uint32_t kQueuedFrames = 64;   // the most permits a client may start with (Xenia apu_max_queued_frames default)
// FM4 Recomp: the default is 16 frames (85 ms, Xenia's minimum), not
// Xenia's 64, so a new sound is not 336 ms late; a game hitch longer than ~85 ms plays silence (seen once, 8 frames).
constexpr uint32_t kDefaultQueuedFrames = 16;
// FM4 Recomp (opt-in FM4_AUDIO_QUEUED_FRAMES=<16..64>): the permits a client
// starts with and their maximum, as Xenia's apu_max_queued_frames cvar (audio_system.cc:38, :52: "Allows changing max buffered audio
// frames to reduce audio delay. Minimum is 16."). The guest keeps the queue full, so every sound waits this many frames (5.33 ms each)
// before it is played (FM4_DIAG_AV_LATENCY: 63 frames = 336 ms). Default kDefaultQueuedFrames.
uint32_t QueuedFrames();
constexpr uint32_t kMaxQueued = 256;     // frames waiting for the output before the run stops (not in Xenia)
constexpr uint32_t kDriverTag = 0x41550000;  // driver handle = tag | client index (xboxkrnl_audio.cc:77)

enum class Sink { Unset, External, Clock };
// External: a test calls ConsumeFrame. Clock: a host thread here calls it every 256/48000 s from the first
// registration on. Registering a client with no sink set stops (9).
void SetSink(Sink sink);
// Audible output: called on the clock thread with each pulled frame (interleaved, kFrameFloats), and the output's
// queued audio in milliseconds for the report. Unset: mute.
void SetOutput(std::function<void(const float*)> output, std::function<double()> queuedMs);

// One frame, guest layout (channel-major, big-endian) -> host floats interleaved per sample (sample * 6 + channel).
void Interleave(const uint8_t* guestFrame, float* out);

// The imports' work. RegisterClient returns the driver handle and starts the worker (guest service thread).
uint32_t RegisterClient(uint8_t* base, uint32_t callback, uint32_t argument);
void SubmitFrame(const uint8_t* base, uint32_t driver, uint32_t samples);

// Fills `out` (kFrameFloats, interleaved) with the oldest submitted frame and returns one permit, or with silence
// when none is waiting. Returns whether a submitted frame was played.
bool ConsumeFrame(float* out);

struct Counters
{
    uint64_t callbacks = 0, submitted = 0, consumed = 0, silent = 0;
    uint32_t permits = 0, queued = 0;
};
Counters GetCounters();
// One "[audio]" line: the counters, output time (frames pulled * 256 / 48000) against wall time and vblanks / 60
// since the first pull. ConsumeFrame prints it every 2 s of output (375 frames); the deadline report too.
void Report(const char* when);

// Tests only: wait until the worker has made `callbacks` callbacks in total (false after `ms`).
bool WaitForCallbacks(uint64_t callbacks, int ms);

}  // namespace runtime::audio
