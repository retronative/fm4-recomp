// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Cross-check of the XMA path on one of the disc's XMA2 files (local game data, never committed): its packets go
// through apu::XmaPass (kernel/xma_pass.h) with the FFmpeg xmaframes adapter (runtime/xma_ffmpeg.cpp) into a
// 31-block output ring that is drained after every pass as a guest would (read offset = write offset, output valid
// set again). The PCM is compared with an independent decode of the same file (FFmpeg's xma2 packet decoder, run by
// tests/xma/run_crosscheck.sh as a separate tool). Agreement checks our framing, adapter and ring handling; it is
// not a Xenon oracle.
//
//   crosscheck <file.xma> <reference.f32le> <frames>
#include "xma_ffmpeg.h"
#include "stops.h"
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

// The adapter's stops (stops.h), without the runtime.
void Stop(StopCode code, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[stop %d] ", int(code));
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::exit(int(code));
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return std::fprintf(stderr, "usage: crosscheck <file.xma> <reference.f32le> <frames>\n"), 2;
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    std::ifstream rf(argv[2], std::ios::binary);
    const std::vector<char> refBytes((std::istreambuf_iterator<char>(rf)), {});
    const size_t want = size_t(std::atoi(argv[3]));
    // RIFF chunks: fmt (0x166 XMA2, channels at +2, rate at +4) and data (2048-byte packets).
    uint32_t channels = 0, rate = 0;
    size_t data = 0, dataSize = 0;
    for (size_t at = 12; at + 8 <= file.size();)
    {
        uint32_t size;
        std::memcpy(&size, &file[at + 4], 4);
        if (!std::memcmp(&file[at], "fmt ", 4))
        {
            const uint16_t tag = uint16_t(file[at + 8] | file[at + 9] << 8);
            if (tag != 0x166)
                return std::fprintf(stderr, "format tag %04X is not XMA2\n", tag), 1;
            channels = file[at + 10] | file[at + 11] << 8;
            std::memcpy(&rate, &file[at + 12], 4);
        }
        if (!std::memcmp(&file[at], "data", 4))
            data = at + 8, dataSize = size;
        at += 8 + size + (size & 1);
    }
    if (!data || channels < 1 || channels > 2)
        return std::fprintf(stderr, "no data chunk, or %u channels\n", channels), 1;
    const uint32_t packets = uint32_t(std::min<size_t>(dataSize / 2048, 4095));
    // Physical layout of the test: input at 0x100000, the ring at 0x10000 (31 blocks).
    std::vector<uint8_t> ring(31 * 256);
    auto memory = [&](uint32_t physical, uint32_t bytes) -> uint8_t* {
        if (physical == 0x100000 && bytes <= packets * 2048u) return const_cast<uint8_t*>(&file[data]);
        if (physical == 0x10000 && bytes <= ring.size()) return ring.data();
        return nullptr;
    };
    uint32_t w[16] = {};
    w[0] = packets | 1u << 20 | 31u << 22;
    const uint32_t rateField = rate == 24000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : 3;
    w[1] = rateField << 27 | (channels == 2 ? 1u << 29 : 0) | 1u << 31;
    w[2] = 32;
    w[5] = 0x100000, w[7] = 0x10000;
    apu::XmaContextState state;
    const apu::XmaDecoderFactory factory = runtime::xma::FfmpegDecoderFactory();
    std::vector<int16_t> ours;
    uint32_t passes = 0;
    while (ours.size() < want * 512 * channels)
    {
        const apu::XmaPassResult r = apu::XmaPass(w, state, memory, factory);
        if (!r.ok)
            return std::fprintf(stderr, "pass %u: %s\n", passes, r.error.c_str()), 1;
        passes++;
        const apu::XmaContext c = apu::DecodeContext(w);
        const uint32_t read = c.outputBufferReadOffset * 256, write = c.outputBufferWriteOffset * 256;
        const uint32_t bytes = r.framesWritten ? (write + ring.size() - read) % ring.size() + (write == read ? ring.size() : 0) : 0;
        for (uint32_t i = 0; i < bytes; i += 2)
        {
            const uint8_t* s = &ring[(read + i) % ring.size()];
            ours.push_back(int16_t(uint16_t(s[0] << 8 | s[1])));
        }
        w[9] = (w[9] & ~0x1Fu) | c.outputBufferWriteOffset;  // the guest has read everything
        w[1] |= 1u << 31;                                       // and hands the ring back
        if (!c.inputBuffer0Valid && !c.inputBuffer1Valid)
            break;
        if (r.frames == 0 && !r.framesWritten)
            return std::fprintf(stderr, "pass %u decoded nothing\n", passes), 1;
    }
    // The reference as 16-bit with the same conversion (x 32767, nearest, saturated: apu::StoreSample).
    std::vector<int16_t> ref(refBytes.size() / 4);
    for (size_t i = 0; i < ref.size(); i++)
    {
        float f;
        std::memcpy(&f, &refBytes[i * 4], 4);
        uint8_t b[2];
        apu::StoreSample(b, f);
        ref[i] = int16_t(uint16_t(b[0] << 8 | b[1]));
    }
    // Best alignment within +-4 frames (whole samples of all channels), then the largest difference.
    const long n = long(std::min<size_t>(ours.size(), want * 512 * channels));
    long bestShift = 0, bestMax = 1 << 30;
    for (long shift = -4L * 512 * channels; shift <= 4L * 512 * channels; shift += long(channels))
    {
        long worst = 0, compared = 0;
        for (long i = 0; i < n && worst < bestMax; i++)
        {
            const long j = i + shift;
            if (j < 0 || j >= long(ref.size())) continue;
            worst = std::max(worst, std::labs(long(ours[size_t(i)]) - long(ref[size_t(j)])));
            compared++;
        }
        if (compared > n / 2 && worst < bestMax)
            bestMax = worst, bestShift = shift;
    }
    long nonzero = 0;
    for (long i = 0; i < n; i++) nonzero += ours[size_t(i)] != 0;
    std::printf("crosscheck: %s: XMA2 %u Hz, %u channels, %u packets; %u passes, %ld samples ours (%ld non-zero), %zu reference; "
                "best alignment %ld samples (%ld frames), largest difference %ld (16-bit)\n", argv[1], rate, channels, packets, passes, n,
                nonzero, ref.size(), bestShift, bestShift / long(512 * channels), bestMax);
    return bestMax <= 1 && nonzero > n / 2 ? 0 : 1;
}
