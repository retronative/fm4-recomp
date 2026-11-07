// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The XMA decoder's registers, context array and imports (xma.h). Register handling after Xenia xenia-canary
// 67f16c4e315f apu/xma_decoder.cc, xma_context.cc.
#include "xma.h"
#include "graphics.h"
#include "imports.h"
#include "virtual_memory.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace runtime::xma {
namespace {
constexpr uint32_t kContextArrayAddress = 0x600, kLockBracket = 0x601, kCurrentContext = 0x606;
constexpr uint32_t kKick0 = 0x650, kLock0 = 0x690, kClear0 = 0x6A0, kGroups = kContextCount / 32;

std::mutex lock;  // the whole model; a decoding pass runs under it
apu::XmaDecoderFactory decoders;
uint32_t contextArray = 0, contextArrayPhysical = 0;
bool allocated[kContextCount] = {};
apu::XmaContextState states[kContextCount];
Counters counters;

// The context array (Xenia XmaDecoder::Setup: 320 x 64 bytes, physical, read-write, uncached). The 4 KB physical
// view (E0000000) is not modelled (virtual_memory.h), so it comes from the 64 KB view. Under `lock`.
void EnsureContextArray()
{
    if (contextArray)
        return;
    Heap* heap = PhysicalHeap(0x10000);
    contextArray = heap ? heap->Alloc(0x10000, 0x10000, mem::kReserve | mem::kCommit, page::kReadWrite | page::kNoCache, true) : 0;  // top-down: never physical 0, which a guest could read as "none"
    if (contextArray == 0 || !PhysicalAddress(contextArray, contextArrayPhysical))
        Stop(StopCode::Setup, "XMA: no physical memory for the context array");
    std::fprintf(stderr, "[xma] context array at %08X (physical %08X), %u contexts of %u bytes\n", contextArray, contextArrayPhysical,
                 kContextCount, kContextBytes);
}

uint32_t AllocatedContext(uint32_t group, uint32_t bit, const char* what)
{
    const uint32_t id = group * 32 + bit;
    if (id >= kContextCount || !allocated[id])
        Stop(StopCode::Unimplemented, "XMA %s of context %u, which is not allocated", what, id);
    return id;
}

// The host bytes of a physical range, if one view's allocation holds all of it contiguously; null otherwise.
uint8_t* Physical(uint8_t* base, uint32_t physical, uint32_t bytes)
{
    uint32_t first = 0;
    if (bytes == 0 || !PhysicalToGuest(physical, first))
        return nullptr;
    for (uint64_t at = (uint64_t(physical) & ~0xFFFull) + 0x1000; at < uint64_t(physical) + bytes; at += 0x1000)
    {
        uint32_t guest = 0;
        if (!PhysicalToGuest(uint32_t(at), guest) || guest - first != uint32_t(at) - physical)
            return nullptr;
    }
    return base + first;
}

void Kick(uint8_t* base, uint32_t group, uint32_t bits)
{
    for (uint32_t i = 0; i < 32; i++)
    {
        if (!(bits >> i & 1))
            continue;
        const uint32_t id = AllocatedContext(group, i, "kick");
        const uint32_t data = contextArray + id * kContextBytes;
        uint32_t words[16];
        for (uint32_t w = 0; w < 16; w++)
            words[w] = Load32(base, data + 4 * w);
        const apu::XmaContext c = apu::DecodeContext(words);
        counters.kicks++;
        if ((!c.outputBufferValid || (!c.inputBuffer0Valid && !c.inputBuffer1Valid)) && !c.stopWhenDone && !c.interruptWhenDone)
        {
            counters.idleKicks++;  // nothing to decode (Xenia XmaContext::Decode returns at once)
            continue;
        }
        const bool first = states[id].framesDecoded == 0;
        const apu::XmaPassResult r =
            apu::XmaPass(words, states[id], [base](uint32_t physical, uint32_t bytes) { return Physical(base, physical, bytes); }, decoders);
        if (!r.ok)
        {
            std::fprintf(stderr, "[xma] kick of context %u: %s\n", id, apu::DescribeContext(c).c_str());
            const uint32_t packets = c.currentBuffer ? c.inputBuffer1PacketCount : c.inputBuffer0PacketCount;
            if (const uint8_t* in = packets ? Physical(base, c.currentBuffer ? c.inputBuffer1 : c.inputBuffer0, packets * apu::kXmaPacketBytes) : nullptr)
            {
                const apu::XmaPacketHeader h = apu::DecodePacketHeader(in);
                std::fprintf(stderr, "[xma]   input %u packet 0: header %02X%02X%02X%02X (frame count %u, first frame at bit %u, metadata %u, skip %u)\n",
                             c.currentBuffer, in[0], in[1], in[2], in[3], h.frameCount, h.frameOffsetBits, h.metadata, h.skipCount);
            }
            Stop(StopCode::Unimplemented, "XMA decoding pass of context %u: %s", id, r.error.c_str());
        }
        for (uint32_t w : {0u, 1u, 2u, 4u})  // the words holding the fields the pass owns
            Store32(base, data + 4 * w, words[w]);
        counters.passes++;
        counters.framesDecoded += r.frames;
        counters.framesWritten += r.framesWritten;
        if (first && r.frames)
        {
            const apu::XmaContext after = apu::DecodeContext(words);
            const uint32_t input = c.currentBuffer ? c.inputBuffer1 : c.inputBuffer0;
            const uint8_t* in = Physical(base, input, apu::kXmaPacketBytes);
            std::fprintf(stderr, "[xma] context %u first pass: %u Hz %s, %u frames decoded, %u written; packet header %02X%02X%02X%02X "
                         "(metadata %u); read offset %u, output write %u valid %u\n", id, apu::SampleRateHz(c.sampleRate),
                         c.isStereo ? "stereo" : "mono", r.frames, r.framesWritten, in ? in[0] : 0, in ? in[1] : 0, in ? in[2] : 0,
                         in ? in[3] : 0, in ? in[2] & 7 : 0, after.inputBufferReadOffset, after.outputBufferWriteOffset, after.outputBufferValid);
        }
    }
}

// Lock (Xenia XmaContext::Disable): decoding finishes inside the kick, so nothing is in progress to stop.
void LockContexts(uint32_t group, uint32_t bits)
{
    for (uint32_t i = 0; i < 32; i++)
        if (bits >> i & 1)
            (void)AllocatedContext(group, i, "lock");
    counters.locks++;
}

// Clear (Xenia XmaContext::Clear, xma_context.cc:139-160): both input buffers and the output buffer invalid, the
// input read, output read and output write offsets 0, the frame in progress dropped (the decoder is kept).
void Clear(uint8_t* base, uint32_t group, uint32_t bits)
{
    for (uint32_t i = 0; i < 32; i++)
        if (bits >> i & 1)
        {
            const uint32_t id = AllocatedContext(group, i, "clear");
            const uint32_t data = contextArray + id * kContextBytes;
            Store32(base, data, Load32(base, data) & ~((3u << 20) | (0x1Fu << 27)));
            Store32(base, data + 4, Load32(base, data + 4) & ~(1u << 31));
            Store32(base, data + 8, Load32(base, data + 8) & ~0x03FFFFFFu);
            Store32(base, data + 36, Load32(base, data + 36) & ~0x1Fu);
            states[id].partial.clear();
            states[id].partialBits = 0;
        }
    counters.clears++;
}
}  // namespace

void SetDecoderFactory(apu::XmaDecoderFactory factory)
{
    std::lock_guard guard(lock);
    decoders = std::move(factory);
}

uint32_t LoadRegister(uint8_t*, uint32_t address)
{
    const uint32_t index = (address - kRegisterBase) / 4;
    if (!(address & 3) && index == kCurrentContext)
        return 0;  // processing nothing (bit 9 clear)
    if (address & 3 || index != kContextArrayAddress)
        Stop(StopCode::Unimplemented, "XMA register %03X (%08X) read: not modelled", index, address);
    std::lock_guard guard(lock);
    EnsureContextArray();
    return __builtin_bswap32(contextArrayPhysical);  // the device's little-endian word, as PPC_LOAD_U32 returns it
}

void StoreRegister(uint8_t* base, uint32_t address, uint32_t value)
{
    const uint32_t index = (address - kRegisterBase) / 4, word = __builtin_bswap32(value);  // the device's word
    std::lock_guard guard(lock);
    if (!(address & 3) && index >= kKick0 && index < kKick0 + kGroups)
        return Kick(base, index - kKick0, word);
    if (!(address & 3) && index >= kLock0 && index < kLock0 + kGroups)
        return LockContexts(index - kLock0, word);
    if (!(address & 3) && index >= kClear0 && index < kClear0 + kGroups)
        return Clear(base, index - kClear0, word);
    if (address & 3 || index != kLockBracket || (value != 0x02000000 && value != 0x03000000))
        Stop(StopCode::Unimplemented, "XMA register %03X (%08X) written with %08X: not modelled", index, address, value);
    counters.bracketWrites++;
    counters.lastBracket = value;
}

uint32_t CreateContext(uint8_t* base)
{
    std::lock_guard guard(lock);
    EnsureContextArray();
    for (uint32_t i = 0; i < kContextCount; i++)
        if (!allocated[i])
        {
            allocated[i] = true;
            counters.allocated++;
            std::memset(base + contextArray + i * kContextBytes, 0, kContextBytes);
            return contextArray + i * kContextBytes;
        }
    return 0;
}

void ReleaseContext(uint8_t* base, uint32_t context)
{
    std::lock_guard guard(lock);
    const uint32_t offset = context - contextArray, index = offset / kContextBytes;
    if (contextArray == 0 || offset % kContextBytes || index >= kContextCount || !allocated[index])
        Stop(StopCode::Unimplemented, "XMAReleaseContext(%08X): not an allocated XMA context", context);
    std::memset(base + context, 0, kContextBytes);  // Xenia XmaContext::Release zeroes it
    allocated[index] = false;
    counters.allocated--;
    states[index].Reset();
}

Counters GetCounters()
{
    std::lock_guard guard(lock);
    return counters;
}

void Report(const char* when)
{
    std::unique_lock guard(lock, std::try_to_lock);
    if (!guard.owns_lock())
    {
        std::fprintf(stderr, "[xma] %s: a decoding pass is in progress\n", when);
        return;
    }
    const Counters& c = counters;
    std::fprintf(stderr, "[xma] %s: %u contexts allocated; 601 written %u times (last %08X); %u kicks (%u idle), %u locks, %u clears, "
                 "%u passes: %llu frames decoded, %llu written\n", when, c.allocated, c.bracketWrites, c.lastBracket, c.kicks, c.idleKicks,
                 c.locks, c.clears, c.passes, (unsigned long long)c.framesDecoded, (unsigned long long)c.framesWritten);
}

}  // namespace runtime::xma

// recomp/mmio.h: device accesses. Addresses outside the XMA block are plain guest memory, as the unmarked macros; a
// store to a DC_LUT register of the GPU also updates the gamma ramp (graphics.h OnGammaRampRegisterWritten).
uint32_t PpcMmioLoad32(uint8_t* base, uint32_t address)
{
    if (address - runtime::xma::kRegisterBase < runtime::xma::kRegisterBytes)
        return runtime::xma::LoadRegister(base, address);
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + address));
}

void PpcMmioStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address - runtime::xma::kRegisterBase < runtime::xma::kRegisterBytes)
        return runtime::xma::StoreRegister(base, address, value);
    *reinterpret_cast<volatile uint32_t*>(base + address) = __builtin_bswap32(value);
    // The GPU's 64 KB register window (graphics.h): a store to a DC_LUT register also runs the gamma ramp's state machine.
    if (address - runtime::gpu::kRegisterBase < 0x10000 && !(address & 3))
        runtime::gpu::OnGammaRampRegisterWritten(base, (address - runtime::gpu::kRegisterBase) / 4, value, runtime::gpu::LutWriter::Cpu);
}
