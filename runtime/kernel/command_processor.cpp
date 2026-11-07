// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The headless command processor (command_processor.h). Line references are to Xenia 67f16c4
// src/xenia/gpu/pm4_command_processor_implement.h unless named otherwise.
#include "command_processor.h"
#include "graphics.h"
#include "cp_trace.h"
#include "imports.h"
#include "virtual_memory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace runtime::gpu {
namespace {

constexpr uint32_t kSwapSignature = 0x53574150;  // "SWAP" (xenos.h:28)

// PM4 type-3 opcodes (xenos.h:1594-1655).
enum : uint32_t {
    kNop = 0x10, kRegRmw = 0x21, kDrawIndx = 0x22, kVizQuery = 0x23, kWaitForIdle = 0x26, kImLoad = 0x27, kImLoadImmediate = 0x2B,
    kSetConstant = 0x2D, kLoadAluConstant = 0x2F, kDrawIndx2 = 0x36, kIndirectBufferPfd = 0x37, kInvalidateState = 0x3B, kWaitRegMem = 0x3C,
    kMemWrite = 0x3D, kRegToMem = 0x3E, kIndirectBuffer = 0x3F, kCondWrite = 0x45, kEventWrite = 0x46, kMeInit = 0x48,
    kSetBinMask = 0x50, kSetBinSelect = 0x51, kInterrupt = 0x54, kSetConstant2 = 0x55, kEventWriteShd = 0x58, kEventWriteExt = 0x5A,
    kContextUpdate = 0x5E, kSetBinMaskLo = 0x60, kSetBinMaskHi = 0x61, kSetBinSelectLo = 0x62, kSetBinSelectHi = 0x63,
    kXeSwap = 0x64,
};

uint32_t Register(const uint8_t* base, uint32_t index)
{
    if (index >= kRegisterCount)
        Stop(StopCode::Unimplemented, "GPU: register %X is past the register file", index);
    return ReadRegister(base, index);
}

// MatchValueAndRef (:681-693): wait_info & 7 selects never, <, <=, ==, !=, >=, >, always.
bool Matches(uint32_t value, uint32_t ref, uint32_t waitInfo)
{
    switch (waitInfo & 7)
    {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

struct Reader
{
    const uint8_t* base;
    uint32_t start, size, position, left;
    uint32_t Next()
    {
        const uint32_t word = Load32(base, start + 4 * (position % size));
        position++, left--;
        return word;
    }
};

std::atomic<uint32_t> ringStart{0}, ringWords{0}, readIndex{0}, writeBack{0}, stopAtSwap{1};
const RendererHooks* renderer = nullptr;  // set once by main before the ring starts
std::mutex stateLock;
CpState ringState;

}  // namespace

uint32_t PhysicalToGuest(uint32_t physical)
{
    uint32_t address = 0;
    if (!runtime::PhysicalToGuest(physical & 0x1FFFFFFFu, address))
        Stop(StopCode::Unimplemented, "GPU: physical address %08X is in no allocation of any view", physical);
    return address;
}

uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)  // xenos.h:1070-1087
    {
    case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(value);
    case 3: return (value >> 16) | (value << 16);
    default: return value;
    }
}

uint32_t LoadGpu(const uint8_t* base, uint32_t address)
{
    uint32_t raw;
    std::memcpy(&raw, base + PhysicalToGuest(address & ~3u), 4);  // host (little-endian) order, as Xenia's xe::load
    return GpuSwap(raw, address & 3);
}

void StoreGpu(uint8_t* base, uint32_t address, uint32_t value)
{
    const uint32_t raw = GpuSwap(value, address & 3);
    std::memcpy(base + PhysicalToGuest(address & ~3u), &raw, 4);
}

void CpWriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegisterCount)
        Stop(StopCode::Unimplemented, "GPU: a write to register %X, past the register file", index);
    if (index == reg::kCoherStatusHost)
        value |= 1u << 31;  // the status bit (command_processor.cc:465-467)
    WriteRegister(base, index, value);
    OnGammaRampRegisterWritten(base, index, value, LutWriter::CommandProcessor);  // DC_LUT_* only (command_processor.cc:469-574)
    if (index >= reg::kScratchReg0 && index < reg::kScratchReg0 + 8 &&
        (ReadRegister(base, reg::kScratchUmsk) >> (index - reg::kScratchReg0) & 1))
        Store32(base, PhysicalToGuest(ReadRegister(base, reg::kScratchAddr) + 4 * (index - reg::kScratchReg0)), value);
}

void Execute(uint8_t* base, uint32_t start, uint32_t size, uint32_t from, uint32_t words, CpState& state)
{
    Reader r{base, start, size, from, words};
    while (r.left > 0)
    {
        const uint32_t packet = r.Next();
        const uint32_t type = packet >> 30;
        state.lastHeaders[state.lastCount++ % 16] = packet;
        state.packets++;
        if (packet == 0 || packet == 0x0BADF00D || type == 2)  // skipped (:274-312)
            continue;
        if (type == 1)  // two registers (:357-368)
        {
            if (r.left < 2)
                Stop(StopCode::Unimplemented, "GPU: a type-1 packet %08X past the end of its buffer", packet);
            const uint32_t first = r.Next(), second = r.Next();
            CpWriteRegister(base, packet & 0x7FF, first);
            CpWriteRegister(base, (packet >> 11) & 0x7FF, second);
            continue;
        }
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (r.left < count)
            Stop(StopCode::Unimplemented, "GPU: packet %08X needs %u words, %u are left", packet, count, r.left);
        if (type == 0)  // count registers from base_index, or one register count times (:328-355)
        {
            const uint32_t index = packet & 0x7FFF;
            const bool one = (packet >> 15) & 1;
            for (uint32_t i = 0; i < count; i++)
                CpWriteRegister(base, one ? index : index + i, r.Next());
            continue;
        }
        const uint32_t opcode = (packet >> 8) & 0x7F;
        uint32_t remaining = count;
        auto next = [&] {
            if (remaining == 0)
                Stop(StopCode::Unimplemented, "GPU: opcode %02X with %u words is too short", opcode, count);
            remaining--;
            return r.Next();
        };
        if ((packet & 1) && ((state.binSelect & state.binMask) == 0 || opcode == kXeSwap))  // predicated (:405-413)
        {
            while (remaining) next();
            continue;
        }
        switch (opcode)
        {
        case kMeInit: case kNop: case kInvalidateState: case kContextUpdate: case kWaitForIdle:
            break;  // nothing to do headless (:612-628)
        case kImLoadImmediate:  // {type, start << 16 | size in words}, then the microcode inline (:1274-1308)
        {
            const uint32_t type = next(), sizeWords = next() & 0xFFFF;
            if (sizeWords > remaining)
                Stop(StopCode::Unimplemented, "GPU: IM_LOAD_IMMEDIATE of %u words in a %u-word packet", sizeWords, count);
            if (TraceEnabled() || renderer)
            {
                std::vector<uint32_t> code(sizeWords);
                for (uint32_t i = 0; i < sizeWords; i++)
                    code[i] = next();
                if (TraceEnabled())
                    TraceShader(type, code.data(), sizeWords, ShaderHash(type, code.data(), sizeWords), true);
                if (renderer)
                    renderer->loadShader(type, code.data(), sizeWords);
            }
            break;  // the shader upload itself is not used headless
        }
        case kImLoad:  // {address | type, start << 16 | size in words} (:1244-1272)
        {
            const uint32_t addressType = next(), sizeWords = next() & 0xFFFF;
            if (TraceEnabled() || renderer)
            {
                std::vector<uint32_t> code(sizeWords);
                const uint32_t at = PhysicalToGuest(addressType & ~3u);
                for (uint32_t i = 0; i < sizeWords; i++)
                    code[i] = Load32(base, at + 4 * i);
                if (TraceEnabled())
                    TraceShader(addressType & 3, code.data(), sizeWords, ShaderHash(addressType & 3, code.data(), sizeWords), false);
                if (renderer)
                    renderer->loadShader(addressType & 3, code.data(), sizeWords);
            }
            break;
        }
        case kDrawIndx: case kDrawIndx2:  // (:985-1125): the packet's words go to the registers Xenia writes (:1003, :1025, :1034)
        {
            const uint32_t vizQueryToken = opcode == kDrawIndx ? next() : 0;  // (:1112; DRAW_INDX_2 has no token, :1123)
            const uint32_t initiator = next();
            CpWriteRegister(base, reg::kVgtDrawInitiator, initiator);
            if (((initiator >> 6) & 3) == 0)  // source select kDMA: VGT_DMA_BASE and VGT_DMA_SIZE follow (:1011-1047)
            {
                CpWriteRegister(base, reg::kVgtDmaBase, next());
                CpWriteRegister(base, reg::kVgtDmaSize, next());
            }
            TraceDraw(base, state.draws, opcode, vizQueryToken);
            state.draws++;  // counted; drawn only with a renderer (M4 unit 8)
            if (renderer)
                renderer->draw(base, opcode, vizQueryToken);
            break;
        }
        case kInterrupt:  // per CPU bit, interrupt source 1 (:630-641)
        {
            const uint32_t mask = next();
            for (int n = 0; n < 6; n++)
                if (mask & (1u << n))
                    RequestInterrupt(1, true);
            break;
        }
        case kXeSwap:  // (:644-668)
        {
            const uint32_t magic = next(), frontBuffer = next(), width = next(), height = next();
            if (magic != kSwapSignature)
                Stop(StopCode::Unimplemented, "GPU: a swap packet without its signature (%08X)", magic);
            state.swaps++;
            if (renderer)  // presented before the stop at swap n, so swap n is captured
            {
                uint32_t fetch[6];
                for (uint32_t i = 0; i < 6; i++)
                    fetch[i] = Register(base, reg::kShaderConstantFetch00 + i);
                renderer->present(base, fetch, frontBuffer, width, height, state.swaps);
            }
            if (state.swaps == 1 && stopAtSwap.load() == 1)
                Stop(StopCode::FirstSwap, "first swap: front buffer %08X, %ux%u", frontBuffer, width, height);
            if (state.swaps >= stopAtSwap.load())
                Stop(StopCode::FirstSwap, "swap %llu: front buffer %08X, %ux%u", (unsigned long long)state.swaps, frontBuffer, width, height);
            break;
        }
        case kIndirectBuffer: case kIndirectBufferPfd:  // (:670-678, ExecuteIndirectBuffer)
        {
            const uint32_t address = next() & 0x1FFFFFFFu, length = next() & 0xFFFFF;
            if (length != 0)
                Execute(base, PhysicalToGuest(address), length, 0, length, state);
            break;
        }
        case kWaitRegMem:  // (:700-753), bounded at 10 s
        {
            const uint32_t info = next(), poll = next(), ref = next(), mask = next(), wait = next();
            state.waitingOn = poll;
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;)
            {
                uint32_t value;
                if (info & 0x10)
                    value = LoadGpu(base, poll);
                else
                {
                    if (poll == reg::kCoherStatusHost)  // MakeCoherent: clear the status bit (command_processor.cc:695-730)
                        WriteRegister(base, poll, Register(base, poll) & ~(1u << 31));
                    value = Register(base, poll);
                }
                if (Matches(value & mask, ref, info))
                    break;
                if (std::chrono::steady_clock::now() > give_up)
                    Stop(StopCode::Unimplemented, "GPU: WAIT_REG_MEM on %s %08X (value %08X & %08X, reference %08X, function %u) never satisfied",
                         info & 0x10 ? "memory" : "register", poll, value, mask, ref, info & 7);
                std::this_thread::sleep_for(wait >= 0x100 ? std::chrono::microseconds(1000 * (wait / 0x100)) : std::chrono::microseconds(100));
            }
            state.waitingOn = 0;
            break;
        }
        case kRegRmw:  // (:755-779)
        {
            const uint32_t info = next(), andMask = next(), orMask = next();
            uint32_t value = Register(base, info & 0x1FFF);
            value &= (info >> 31) ? Register(base, andMask & 0x1FFF) : andMask;
            value |= ((info >> 30) & 1) ? Register(base, orMask & 0x1FFF) : orMask;
            CpWriteRegister(base, info & 0x1FFF, value);
            break;
        }
        case kRegToMem:  // (:781-801)
        {
            const uint32_t index = next(), address = next();
            StoreGpu(base, address, Register(base, index));
            break;
        }
        case kMemWrite:  // (:803-818)
        {
            uint32_t address = next();
            while (remaining)
            {
                StoreGpu(base, address, next());
                address += 4;
            }
            break;
        }
        case kCondWrite:  // (:820-862)
        {
            const uint32_t info = next(), poll = next(), ref = next(), mask = next(), target = next(), data = next();
            const uint32_t value = (info & 0x10) ? LoadGpu(base, poll) : Register(base, poll);
            if (Matches(value & mask, ref, info))
            {
                if (info & 0x100)
                    StoreGpu(base, target, data);
                else
                    CpWriteRegister(base, target, data);
            }
            break;
        }
        case kEventWrite:  // (:864-879)
            WriteRegister(base, reg::kVgtEventInitiator, next() & 0x3F);
            break;
        case kEventWriteShd:  // (:881-913)
        {
            const uint32_t initiator = next(), address = next(), value = next();
            WriteRegister(base, reg::kVgtEventInitiator, initiator & 0x3F);
            if ((address & ~3u) > 0x1FFFFFFFu && Register(base, reg::kWritebackBase) != 0)
                Stop(StopCode::Unimplemented, "GPU: EVENT_WRITE_SHD to the write-back window (%08X) is not modelled", address);
            // Bit 31: the counter (vblanks and swaps; Xenia counter_, graphics_system.cc:292-297).
            StoreGpu(base, address, (initiator >> 31) ? uint32_t(VblankCount() + state.swaps) : value);
            break;
        }
        case kSetConstant:  // (:1127-1168): ALU 4000, fetch 4800, bool 4900, loop 4908, registers 2000
        {
            const uint32_t offsetType = next(), type = (offsetType >> 16) & 0xFF;
            const uint32_t bases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
            if (type > 4)
                Stop(StopCode::Unimplemented, "GPU: SET_CONSTANT of type %u", type);
            for (uint32_t index = bases[type] + (offsetType & 0x7FF); remaining; index++)
                CpWriteRegister(base, index, next());
            break;
        }
        case kLoadAluConstant:  // {physical address, offset | type << 16, size in words}: from memory (:1181-1232)
        {
            const uint32_t address = next() & 0x3FFFFFFF, offsetType = next(), size = next() & 0xFFF, type = (offsetType >> 16) & 0xFF;
            const uint32_t bases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};  // as SET_CONSTANT (command_processor.cc:657-690)
            if (type > 4)
                Stop(StopCode::Unimplemented, "GPU: LOAD_ALU_CONSTANT of type %u", type);
            for (uint32_t i = 0; i < size; i++)  // big-endian words (WriteRegistersFromMem: load_and_swap, :609-616)
                CpWriteRegister(base, bases[type] + (offsetType & 0x7FF) + i, Load32(base, PhysicalToGuest(address + 4 * i)));
            break;
        }
        case kEventWriteExt:  // {initiator, address | endian}: the screen extents of earlier draws (:915-949)
        {
            // Nothing is drawn here, so the extents are Xenia's conservative answer: the whole surface (x and y
            // 0-8192 in units of 8, z 0-1), six 16-bit values stored big-endian as its byte_swap leaves them. Xenia
            // asserts the 8in16 swap; another one stops (7).
            const uint32_t initiator = next(), address = next();
            WriteRegister(base, reg::kVgtEventInitiator, initiator & 0x3F);
            if ((address & 3) != 1)
                Stop(StopCode::Unimplemented, "GPU: EVENT_WRITE_EXT with endian %u is not modelled", address & 3);
            const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
            const uint32_t at = PhysicalToGuest(address & ~3u);
            for (uint32_t i = 0; i < 6; i++)
                base[at + 2 * i] = uint8_t(extents[i] >> 8), base[at + 2 * i + 1] = uint8_t(extents[i]);
            break;
        }
        case kSetConstant2:  // (:1170-1179)
        {
            for (uint32_t index = next() & 0xFFFF; remaining; index++)
                CpWriteRegister(base, index, next());
            break;
        }
        case kSetBinMaskLo: state.binMask = (state.binMask & 0xFFFFFFFF00000000ull) | next(); break;  // (:511-546)
        case kSetBinMaskHi: state.binMask = (state.binMask & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
        case kSetBinSelectLo: state.binSelect = (state.binSelect & 0xFFFFFFFF00000000ull) | next(); break;
        case kSetBinSelectHi: state.binSelect = (state.binSelect & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
        case kSetBinMask: { const uint64_t hi = next(); state.binMask = hi << 32 | next(); break; }
        case kSetBinSelect: { const uint64_t hi = next(); state.binSelect = hi << 32 | next(); break; }
        default:
            Stop(StopCode::Unimplemented, "GPU: PM4 opcode %02X (packet %08X, %u words) is not modelled", opcode, packet, count);
        }
        while (remaining)  // the rest of the packet
            next();
    }
}

void SetStopAtSwap(uint32_t n) { stopAtSwap = n; }
void SetRendererHooks(const RendererHooks* hooks) { renderer = hooks; }

void InitializeRing(uint8_t* base, uint32_t physical, uint32_t sizeLog2)
{
    if (ringStart.load() != 0)
        Stop(StopCode::Unimplemented, "VdInitializeRingBuffer: a second ring (%08X) is not modelled", physical);
    const uint32_t bytes = 1u << (sizeLog2 + 3);
    std::memset(base + PhysicalToGuest(physical), 0, bytes);
    readIndex = 0;
    ringWords = bytes / 4;
    ringStart = PhysicalToGuest(physical);
    std::thread([base] {
        for (;;)
        {
            const uint32_t size = ringWords.load(), write = ReadRegister(base, reg::kCpRbWptr) % size, read = readIndex.load();
            if (write == read)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            {
                std::lock_guard guard(stateLock);
                Execute(base, ringStart.load(), size, read, (write + size - read) % size, ringState);
            }
            readIndex = write;
            if (const uint32_t back = writeBack.load())
                Store32(base, PhysicalToGuest(back), write);  // big-endian (command_processor.cc ExecutePrimaryBuffer)
        }
    }).detach();
}

void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockSizeLog2)
{
    (void)blockSizeLog2;  // the read index is written after every batch, more often than RB_BLKSZ asks
    writeBack = physical;
}

void ReportCommandProcessor()
{
    if (ringStart.load() == 0)
        return;
    std::fprintf(stderr, "[hang] GPU ring %08X (%u words): read %u, write-back %08X; %llu packets, %llu draws, waiting on %08X; last headers",
                 ringStart.load(), ringWords.load(), readIndex.load(), writeBack.load(), (unsigned long long)ringState.packets,
                 (unsigned long long)ringState.draws, ringState.waitingOn);
    for (uint32_t i = 0; i < 16 && i < ringState.lastCount; i++)
        std::fprintf(stderr, " %08X", ringState.lastHeaders[(ringState.lastCount - 1 - i) % 16]);
    std::fprintf(stderr, "\n");
}

}  // namespace runtime::gpu
