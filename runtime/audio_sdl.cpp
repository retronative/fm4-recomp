// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The audible audio sink (fm4recomp --audio audible): the default playback device through SDL3. The 48 kHz clock in
// kernel/audio.cpp paces the guest; each frame it pulls is queued to an SDL audio stream, which the device drains
// on its own clock (the queue depth, in the "[audio]" reports, shows any drift). Kept out of kernel/ so the runtime
// tests build without SDL. Device setup as Xenia xenia-canary 67f16c4e315f apu/sdl/sdl_audio_driver.cc:55-84
// (48 kHz, float, 6 channels), on SDL3's stream API (SDL_audio.h of the installed SDL3); SDL converts the
// 6 channels to the device's layout.
#include "audio_sdl.h"
#include "kernel/audio.h"
#include "stops.h"
#include <SDL3/SDL.h>
#include <cstdio>

namespace runtime::audio {

void StartSdlAudio()
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // the runtime owns SIGTERM (deadline) and the fault handlers
    if (!SDL_Init(SDL_INIT_AUDIO))
        Stop(StopCode::Setup, "audio: SDL_Init(AUDIO) failed: %s", SDL_GetError());
    const SDL_AudioSpec spec{SDL_AUDIO_F32, int(kChannels), int(kSampleRate)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
        Stop(StopCode::Setup, "audio: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
    SDL_AudioSpec deviceSpec{};
    int deviceFrames = 0;
    SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames);
    const int version = SDL_GetVersion();
    std::fprintf(stderr, "[audio] audible: SDL %d.%d.%d, driver %s, device \"%s\" (%d Hz, %d channels, %d-frame buffer); "
                 "input 48000 Hz float, 6 channels, paced by the runtime's 48 kHz clock\n", SDL_VERSIONNUM_MAJOR(version),
                 SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version), SDL_GetCurrentAudioDriver(),
                 SDL_GetAudioDeviceName(device), deviceSpec.freq, deviceSpec.channels, deviceFrames);
    if (!SDL_ResumeAudioStreamDevice(stream))
        Stop(StopCode::Setup, "audio: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
    SetOutput([stream](const float* frame) { SDL_PutAudioStreamData(stream, frame, int(kFrameBytes)); },
              [stream] { return double(SDL_GetAudioStreamQueued(stream)) / (kChannels * 4) * 1000.0 / kSampleRate; });
    SetSink(Sink::Clock);
}

}  // namespace runtime::audio
