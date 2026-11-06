// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/apu/xma.h (GPL-3.0-or-later, same owner), reviewed and re-tested here
// (tests/runtime/test_runtime.cpp, XMA section). FM4 Recomp changes: BuildFramePacket removed (the decoder here is
// FFmpeg's "xmaframes", fed one frame at a time: runtime/xma_ffmpeg.cpp); the frame decoder's contract states that.
//
// XMA hardware context data and packet headers, and one decoding pass. Pure: no guest memory, no codec.
//
// Context layout: REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f, BSD-3-Clause:
// apu/xma_context.h XMA_CONTEXT_DATA), re-written: 16 words, big-endian in guest memory; within a word the fields
// run from bit 0 up. Packet header: 2048-byte packets whose first 32 bits are, from the most significant: XMA2 a
// 6-bit frame count (XMA1: a 4-bit sequence number and 2 more bits), a 15-bit offset in bits of the first frame that
// starts in the packet (counted from the end of the header), 3 bits of metadata and an 8-bit skip count (Xenia
// xma_context.cc GetPacketFrameOffset/GetPacketSkipCount; SpeedBreaker/ReCarbon xma.cpp; the meaning of the
// metadata bits is UNKNOWN beyond Xenia's check "1 for XMA2, 0 for XMA", xma_context.cc:940).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apu {

constexpr uint32_t kXmaPacketBytes = 2048, kXmaPacketHeaderBits = 32, kXmaPacketBits = kXmaPacketBytes * 8;
constexpr uint32_t kXmaFrameLengthBits = 15, kXmaNoFrame = 0x7FFF;  // a frame starts with its length in bits; all ones: none
constexpr uint32_t kXmaSamplesPerFrame = 512, kXmaBlockBytes = 256;

struct XmaContext
{
    uint32_t inputBuffer0PacketCount, loopCount, inputBuffer0Valid, inputBuffer1Valid, outputBufferBlockCount,
        outputBufferWriteOffset;                                                                   // word 0
    uint32_t inputBuffer1PacketCount, loopSubframeStart, loopSubframeEnd, loopSubframeSkip, subframeDecodeCount,
        subframeSkipCount, sampleRate, isStereo, unknown1, outputBufferValid;                      // word 1
    uint32_t inputBufferReadOffset, unknown2;                                                      // word 2
    uint32_t loopStart, parserErrorStatus;                                                         // word 3
    uint32_t loopEnd, packetMetadata, currentBuffer;                                               // word 4
    uint32_t inputBuffer0, inputBuffer1, outputBuffer, workBuffer;                                 // words 5-8 (physical)
    uint32_t outputBufferReadOffset, unknown9, stopWhenDone, interruptWhenDone;                    // word 9
    uint32_t reserved[6];                                                                          // words 10-15
};

constexpr uint32_t Bits(uint32_t word, unsigned low, unsigned count) { return (word >> low) & ((1u << count) - 1); }

// From the 16 words as the guest stores them, already read as big-endian values.
constexpr XmaContext DecodeContext(const uint32_t (&w)[16])
{
    XmaContext c{};
    c.inputBuffer0PacketCount = Bits(w[0], 0, 12);
    c.loopCount = Bits(w[0], 12, 8);
    c.inputBuffer0Valid = Bits(w[0], 20, 1);
    c.inputBuffer1Valid = Bits(w[0], 21, 1);
    c.outputBufferBlockCount = Bits(w[0], 22, 5);
    c.outputBufferWriteOffset = Bits(w[0], 27, 5);
    c.inputBuffer1PacketCount = Bits(w[1], 0, 12);
    c.loopSubframeStart = Bits(w[1], 12, 2);
    c.loopSubframeEnd = Bits(w[1], 14, 3);
    c.loopSubframeSkip = Bits(w[1], 17, 3);
    c.subframeDecodeCount = Bits(w[1], 20, 4);
    c.subframeSkipCount = Bits(w[1], 24, 3);
    c.sampleRate = Bits(w[1], 27, 2);
    c.isStereo = Bits(w[1], 29, 1);
    c.unknown1 = Bits(w[1], 30, 1);
    c.outputBufferValid = Bits(w[1], 31, 1);
    c.inputBufferReadOffset = Bits(w[2], 0, 26);
    c.unknown2 = Bits(w[2], 26, 6);
    c.loopStart = Bits(w[3], 0, 26);
    c.parserErrorStatus = Bits(w[3], 26, 6);
    c.loopEnd = Bits(w[4], 0, 26);
    c.packetMetadata = Bits(w[4], 26, 5);
    c.currentBuffer = Bits(w[4], 31, 1);
    c.inputBuffer0 = w[5];
    c.inputBuffer1 = w[6];
    c.outputBuffer = w[7];
    c.workBuffer = w[8];
    c.outputBufferReadOffset = Bits(w[9], 0, 5);
    c.unknown9 = Bits(w[9], 5, 25);
    c.stopWhenDone = Bits(w[9], 30, 1);
    c.interruptWhenDone = Bits(w[9], 31, 1);
    for (unsigned i = 0; i < 6; i++)
        c.reserved[i] = w[10 + i];
    return c;
}

// Sample rate field: 24, 32, 44.1 or 48 kHz (Xenia XmaContext::GetSampleRate).
constexpr uint32_t SampleRateHz(uint32_t field) { return field == 0 ? 24000 : field == 1 ? 32000 : field == 2 ? 44100 : 48000; }

struct XmaPacketHeader { uint32_t frameCount, frameOffsetBits, metadata, skipCount; };

// The first 4 bytes of a packet, in stream order.
constexpr XmaPacketHeader DecodePacketHeader(const uint8_t* p)
{
    return {uint32_t(p[0] >> 2), (uint32_t(p[0] & 3) << 13) | (uint32_t(p[1]) << 5) | (p[2] >> 3), uint32_t(p[2] & 7), p[3]};
}

// One line for a diagnostic report.
inline std::string DescribeContext(const XmaContext& c)
{
    char text[512];
    std::snprintf(text, sizeof text,
                  "input 0 %08X x%u packets valid %u, input 1 %08X x%u valid %u, current %u, read offset %u bits; output %08X %u blocks "
                  "valid %u, read %u write %u; %u Hz %s, subframes decode %u skip %u; loop %u (%u-%u, subframes %u/%u/%u); work %08X; "
                  "metadata %u, parser error %u; stop when done %u, interrupt when done %u; unknown %u/%u/%u",
                  c.inputBuffer0, c.inputBuffer0PacketCount, c.inputBuffer0Valid, c.inputBuffer1, c.inputBuffer1PacketCount, c.inputBuffer1Valid,
                  c.currentBuffer, c.inputBufferReadOffset, c.outputBuffer, c.outputBufferBlockCount, c.outputBufferValid,
                  c.outputBufferReadOffset, c.outputBufferWriteOffset, SampleRateHz(c.sampleRate), c.isStereo ? "stereo" : "mono",
                  c.subframeDecodeCount, c.subframeSkipCount, c.loopCount, c.loopStart, c.loopEnd, c.loopSubframeStart, c.loopSubframeEnd,
                  c.loopSubframeSkip, c.workBuffer, c.packetMetadata, c.parserErrorStatus, c.stopWhenDone, c.interruptWhenDone, c.unknown1,
                  c.unknown2, c.unknown9);
    return text;
}

// --- One decoding pass (a kick) -------------------------------------------------------------
//
// REFERENCE_DERIVED from Xenia's XmaContext::Decode (re-written; see above) with these choices:
//  - the pass runs synchronously at the kick (APPROXIMATE: the hardware decodes asynchronously);
//  - frames are written to the output ring whole, as big-endian 16-bit samples (Xenia
//    ConvertFrame: x 32767, rounded to nearest, saturated), and only when the decoder returns
//    them: a frame that yields no samples (the decoder's first frame) writes nothing (APPROXIMATE:
//    the hardware's latency is UNKNOWN). Nothing is ever written that the decoder did not produce;
//  - the ring: block offsets into block_count x 256 bytes; equal read and write offsets mean
//    empty while the output buffer is valid, and once written full (the output buffer becomes
//    invalid);
//  - a used-up input buffer is swapped at once (its valid bit cleared, current_buffer flipped,
//    read offset 32), not on the next pass as Xenia does; a frame that continues past the end of
//    a buffer is kept (host state, as the hardware keeps it internally) and completed from the
//    other buffer;
//  - subframe_decode_count is not used: a pass decodes whole frames until the ring is full or the
//    input runs out (Xenia does the same; APPROXIMATE). The header's frame count is not checked
//    against the frames found: framing rests on the 15-bit lengths and all-ones padding, and any
//    other padding fails the pass;
//  - only contexts without loops, subframe skips, completion flags or interrupts are implemented; anything
//    else, an inconsistent stream, or a decoder error, fails the pass with a reason (the caller
//    stops). Context fields the pass does not own are left as they are.

// Decodes one XMA frame (its bits, the 15-bit length first) to float samples, one array per
// channel. Implemented by the FFmpeg adapter (runtime/xma_ffmpeg.cpp); tests use fakes.
class XmaFrameDecoder
{
public:
    virtual ~XmaFrameDecoder() = default;
    // `samples` receives 0 or kXmaSamplesPerFrame per channel; false and `error` on failure.
    virtual bool Decode(const uint8_t* bits, uint32_t bitCount, std::vector<float> (&channels)[2], uint32_t& samples, std::string& error) = 0;
};
using XmaDecoderFactory = std::function<std::unique_ptr<XmaFrameDecoder>(uint32_t sampleRate, uint32_t channels, std::string& error)>;

// What the hardware keeps per context between passes: a frame begun at the end of one buffer,
// and the decoder.
struct XmaContextState
{
    std::vector<uint8_t> partial;  // the bits gathered so far, MSB first
    uint32_t partialBits = 0;
    std::unique_ptr<XmaFrameDecoder> decoder;
    uint32_t decoderRate = 0, decoderChannels = 0;
    uint64_t framesDecoded = 0, framesOutput = 0;
    void Reset() { partial.clear(); partialBits = 0; decoder.reset(); decoderRate = decoderChannels = 0; }
};

// Guest memory for a pass: the host bytes of a physical range, or null if it is not all mapped.
using XmaMemory = std::function<uint8_t*(uint32_t physical, uint32_t bytes)>;

constexpr uint32_t ReadBits(const uint8_t* data, uint32_t position, uint32_t count)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; i++)
        value = value << 1 | ((data[(position + i) >> 3] >> (7 - ((position + i) & 7))) & 1);
    return value;
}

inline void AppendBits(std::vector<uint8_t>& out, uint32_t& outBits, const uint8_t* data, uint32_t position, uint32_t count)
{
    out.resize((outBits + count + 7) / 8 + 1, 0);
    for (uint32_t i = 0; i < count; i++, outBits++)
    {
        const uint32_t bit = (data[(position + i) >> 3] >> (7 - ((position + i) & 7))) & 1;
        const uint8_t mask = uint8_t(0x80 >> (outBits & 7));
        out[outBits >> 3] = uint8_t(bit ? out[outBits >> 3] | mask : out[outBits >> 3] & ~mask);
    }
}

// A float sample as the hardware's 16-bit output (Xenia ConvertFrame), big-endian.
inline void StoreSample(uint8_t* out, float value)
{
    const float scaled = std::nearbyint(value * 32767.0f);
    const int32_t clamped = scaled >= 32767.0f ? 32767 : scaled <= -32768.0f ? -32768 : int32_t(scaled);
    out[0] = uint8_t(uint16_t(clamped) >> 8);
    out[1] = uint8_t(clamped);
}

struct XmaPassResult { bool ok = true; std::string error; uint32_t frames = 0, framesWritten = 0; };

inline XmaPassResult XmaPass(uint32_t (&w)[16], XmaContextState& state, const XmaMemory& memory, const XmaDecoderFactory& factory)
{
    XmaPassResult result;
    auto fail = [&](std::string error) { result.ok = false; result.error = std::move(error); return result; };
    XmaContext c = DecodeContext(w);
    if (c.stopWhenDone || c.interruptWhenDone)  // completion signalling: not implemented, even with nothing to decode
        return fail("stop-when-done and interrupt-when-done are not implemented");
    if (!c.outputBufferValid || (!c.inputBuffer0Valid && !c.inputBuffer1Valid))
        return result;  // nothing to do (Xenia)
    if (c.loopCount || c.loopSubframeStart || c.loopSubframeEnd || c.loopSubframeSkip)
        return fail("loops are not implemented");
    if (c.subframeSkipCount)
        return fail("a subframe skip count is not implemented");
    if (c.outputBufferBlockCount == 0)
        return fail("an output buffer of 0 blocks");
    // Stereo: one stream of two channels, written interleaved, left first (Xenia ConvertFrame and
    // SpeedBreaker/ReCarbon agree: REFERENCE_DERIVED).
    const uint32_t channels = c.isStereo ? 2 : 1, frameBytes = kXmaSamplesPerFrame * 2 * channels;
    const uint32_t capacity = c.outputBufferBlockCount * kXmaBlockBytes;
    uint8_t* output = memory(c.outputBuffer, capacity);
    if (!output)
        return fail("the output buffer is not mapped");
    uint32_t write = c.outputBufferWriteOffset * kXmaBlockBytes, read = c.outputBufferReadOffset * kXmaBlockBytes;
    if (write >= capacity || read >= capacity)
        return fail("an output offset past the output buffer");
    uint32_t space = write == read ? capacity : (read + capacity - write) % capacity;  // valid: equal means empty
    const uint32_t rate = SampleRateHz(c.sampleRate);
    if (!state.decoder || state.decoderRate != rate || state.decoderChannels != channels)
    {
        std::string error;
        if (!factory)
            return fail("no XMA decoder is built in");
        state.decoder = factory(rate, channels, error);
        if (!state.decoder)
            return fail("no XMA decoder: " + error);
        state.decoderRate = rate;
        state.decoderChannels = channels;
    }
    auto buffer = [&](uint32_t which, uint32_t& packets) -> uint8_t* {
        packets = which ? c.inputBuffer1PacketCount : c.inputBuffer0PacketCount;
        return packets ? memory(which ? c.inputBuffer1 : c.inputBuffer0, packets * kXmaPacketBytes) : nullptr;
    };
    // Swaps to the other input buffer: this one is used up.
    auto swap = [&] {
        (c.currentBuffer ? c.inputBuffer1Valid : c.inputBuffer0Valid) = 0;
        c.currentBuffer ^= 1;
        c.inputBufferReadOffset = kXmaPacketHeaderBits;
    };
    std::vector<uint8_t> frame;
    std::vector<float> samples[2];
    while (space >= frameBytes)
    {
        if (!(c.currentBuffer ? c.inputBuffer1Valid : c.inputBuffer0Valid))
            break;  // out of input: the pass ends; a partial frame waits for the next buffer
        uint32_t packets = 0;
        const uint8_t* in = buffer(c.currentBuffer, packets);
        if (!in)
            return fail("the current input buffer is empty or not mapped");
        const uint32_t bits = packets * kXmaPacketBits, position = c.inputBufferReadOffset;
        if (position < kXmaPacketHeaderBits || position >= bits || position % kXmaPacketBits < kXmaPacketHeaderBits)
            return fail("the input read offset " + std::to_string(position) + " is not in a packet's payload");
        const uint32_t packet = position / kXmaPacketBits, packetEnd = (packet + 1) * kXmaPacketBits;
        const XmaPacketHeader header = DecodePacketHeader(in + packet * kXmaPacketBytes);
        // The next packet of this stream (skip count), at the start of its payload (checked there:
        // without a frame in progress it must not begin with bits of a previous frame); at the end
        // of the buffer, a swap.
        bool skipPastBuffer = false;
        auto nextPacket = [&]() -> bool {
            const uint32_t next = packet + 1 + header.skipCount;
            if (next > packets)
            {
                skipPastBuffer = true;  // the rest of the skip would continue in the next buffer
                return false;
            }
            if (next == packets)
            {
                swap();
                return false;
            }
            c.inputBufferReadOffset = next * kXmaPacketBits + kXmaPacketHeaderBits;
            return true;
        };
        uint32_t frameBits = 0;
        if (state.partialBits)
        {
            // Complete a frame begun in an earlier packet: the length is in its first 15 bits, of
            // which the earlier packet may hold only some (the rest begin this packet's payload).
            if (position % kXmaPacketBits != kXmaPacketHeaderBits)
                return fail("a split frame does not continue at a packet's start");
            uint32_t length;
            if (state.partialBits >= kXmaFrameLengthBits)
                length = ReadBits(state.partial.data(), 0, kXmaFrameLengthBits);
            else
            {
                const uint32_t missing = kXmaFrameLengthBits - state.partialBits;
                length = ReadBits(state.partial.data(), 0, state.partialBits) << missing | ReadBits(in, position, missing);
            }
            if (length <= kXmaFrameLengthBits || length > kXmaPacketBits - kXmaPacketHeaderBits || length <= state.partialBits)
                return fail("a split frame of " + std::to_string(length) + " bits");
            const uint32_t need = length - state.partialBits;
            if (need > packetEnd - position)
                return fail("a frame spanning more than two packets is not implemented");
            if (header.frameOffsetBits != need)
                return fail("the packet header's previous-frame bits (" + std::to_string(header.frameOffsetBits) + ") differ from the rest of the split frame (" + std::to_string(need) + ")");
            AppendBits(state.partial, state.partialBits, in, position, need);
            frame = state.partial;
            frameBits = state.partialBits;
            state.partial.clear();
            state.partialBits = 0;
            c.inputBufferReadOffset = position + need;
        }
        else
        {
            // At a packet's start with no frame in progress (a new buffer, or the guest's offset),
            // the packet must not begin with bits of a frame this pass never saw (Xenia skips to
            // its first frame; not done until observed).
            if (position % kXmaPacketBits == kXmaPacketHeaderBits && header.frameOffsetBits != 0)
                return fail("the packet at the read offset begins with " + std::to_string(header.frameOffsetBits) +
                            " bits of a frame that was never started");
            const uint32_t left = packetEnd - position;
            if (left < kXmaFrameLengthBits ? ReadBits(in, position, left) == (1u << left) - 1
                                           : ReadBits(in, position, kXmaFrameLengthBits) == kXmaNoFrame)
            {
                nextPacket();  // padding: no further frame in this packet
                if (skipPastBuffer)
                    return fail("a skip count past the end of the input buffer is not implemented");
                continue;
            }
            if (left < kXmaFrameLengthBits)
            {
                // A frame whose length field itself continues in the stream's next packet (not
                // padding: a length is below 2^14, so its first bit is 0).
                AppendBits(state.partial, state.partialBits, in, position, left);
                nextPacket();
                if (skipPastBuffer)
                    return fail("a skip count past the end of the input buffer is not implemented");
                continue;
            }
            const uint32_t length = ReadBits(in, position, kXmaFrameLengthBits);
            if (length <= kXmaFrameLengthBits)
                return fail("a frame of " + std::to_string(length) + " bits");
            if (length > kXmaPacketBits - kXmaPacketHeaderBits)
                return fail("a frame longer than a packet's payload is not implemented");
            if (position + length > packetEnd)
            {
                // The frame continues in the stream's next packet (perhaps in the other buffer).
                AppendBits(state.partial, state.partialBits, in, position, packetEnd - position);
                nextPacket();
                if (skipPastBuffer)
                    return fail("a skip count past the end of the input buffer is not implemented");
                continue;
            }
            frame.clear();
            frameBits = 0;
            AppendBits(frame, frameBits, in, position, length);
            c.inputBufferReadOffset = position + length;
        }
        uint32_t produced = 0;
        std::string error;
        for (auto& channel : samples)
            channel.clear();
        if (!state.decoder->Decode(frame.data(), frameBits, samples, produced, error))
            return fail("the decoder failed: " + error);
        result.frames++;
        state.framesDecoded++;
        if (produced != 0 && produced != kXmaSamplesPerFrame)
            return fail("the decoder returned " + std::to_string(produced) + " samples, not a whole frame");
        if (produced)
        {
            for (uint32_t ch = 0; ch < channels; ch++)
            {
                if (samples[ch].size() != kXmaSamplesPerFrame)
                    return fail("the decoder returned no samples for channel " + std::to_string(ch));
                for (float v : samples[ch])
                    if (std::isnan(v))
                        return fail("the decoder returned a NaN sample");
            }
            for (uint32_t i = 0; i < kXmaSamplesPerFrame; i++)
                for (uint32_t ch = 0; ch < channels; ch++)
                    StoreSample(output + (write + 2 * (i * channels + ch)) % capacity, samples[ch][i]);
            write = (write + frameBytes) % capacity;
            space -= frameBytes;
            result.framesWritten++;
            state.framesOutput++;
        }
        // After the frame: the packet's end, or (all ones) no further frame, moves to the next packet.
        const uint32_t after = c.inputBufferReadOffset;
        if (after >= packetEnd)
        {
            nextPacket();
            if (skipPastBuffer)
                return fail("a skip count past the end of the input buffer is not implemented");
        }
    }
    c.outputBufferWriteOffset = write / kXmaBlockBytes;
    if (result.framesWritten && write == read)
        c.outputBufferValid = 0;  // full
    // Store the fields the pass owns: input valid bits, current buffer, read offset, output write
    // offset and valid bit.
    w[0] = (w[0] & ~((1u << 20) | (1u << 21) | (0x1Fu << 27))) | c.inputBuffer0Valid << 20 | c.inputBuffer1Valid << 21 |
           c.outputBufferWriteOffset << 27;
    w[1] = (w[1] & ~(1u << 31)) | c.outputBufferValid << 31;
    w[2] = (w[2] & ~0x03FFFFFFu) | (c.inputBufferReadOffset & 0x03FFFFFFu);
    w[4] = (w[4] & ~(1u << 31)) | c.currentBuffer << 31;
    return result;
}

}  // namespace apu
