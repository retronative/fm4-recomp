// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The headless command processor (command_processor.h). Line references are to Xenia 67f16c4
// src/xenia/gpu/pm4_command_processor_implement.h unless named otherwise.
#include "command_processor.h"
#include "deferred_store_parts.h"
#include "graphics.h"
#include "cp_trace.h"
#include "imports.h"
#include "virtual_memory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <pthread.h>
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
    kSetBinMask = 0x50, kSetBinSelect = 0x51, kInterrupt = 0x54, kSetConstant2 = 0x55, kEventWriteShd = 0x58, kEventWriteExt = 0x5A, kEventWriteZpd = 0x5B,
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
std::atomic<uintptr_t> cpThread{0};       // FM4 Recomp (FM4_GUEST_MEMORY_GPU): the ring thread's pthread_self
std::atomic<uintptr_t> completionThread{0};  // FM4 Recomp (FM4_ASYNC_RETIRE): the renderer's completion worker's pthread_self
void (*reapRetired)() = nullptr;             // FM4 Recomp (FM4_ASYNC_RETIRE): SetReapHook; null otherwise
void BeforeCpWrite(uint32_t physical, uint32_t bytes)
{
    if (renderer && renderer->beforeCpWrite)
        renderer->beforeCpWrite(physical & 0x1FFFFFFFu, bytes);
}
void HonourGpuReadGuard()
{
    if (renderer && renderer->honourGpuReadGuard)
        renderer->honourGpuReadGuard();
    if (reapRetired)  // FM4 Recomp (FM4_ASYNC_RETIRE): at the same points (no command buffer handle is live at any of them)
        reapRetired();
}
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
    BeforeCpWrite(address & ~3u, 4);
    const uint32_t raw = GpuSwap(value, address & 3);
    std::memcpy(base + PhysicalToGuest(address & ~3u), &raw, 4);
}

// FM4 Recomp (FM4_DEFER_FENCES): a store of the command stream (MEM_WRITE, EVENT_WRITE_SHD, COND_WRITE, REG_TO_MEM) lands when
// the GPU work recorded before it is complete, after that work's resolve write-backs, in packet order: the CPU never sees a fence
// before the data it covers (ReCarbon runtime/gpu/command_processor.cpp QueueStore, :168-180, :833-880). Queued while
// the renderer has open work; once queued, every later store queues too (order).
struct DeferredStore { uint32_t address, value; bool interrupt = false; };  // interrupt: `value` is the CPU mask
std::vector<DeferredStore> deferredStores;
uint8_t* deferredBase = nullptr;
StorePartition storeParts;  // FM4 Recomp (FM4_ASYNC_RETIRE; deferred_store_parts.h): the submission part each queued entry follows
std::atomic<uint64_t> cpIdleNs{0}, selfWaits{0}, immediateWaits{0}, memWaits{0}, syncedWaits{0}, memWaitNs{0}, immediateInterrupts{0}, graceSyncs{0};
// FM4 Recomp (FM4_DIAG_MEMWAIT=1, diagnostic): each memory WAIT_REG_MEM that is not satisfied at once, on every 50th swap (at
// most 4,000 lines): its word, reference, the value before and after the sync, what was pending, and where the time went.
bool MemWaitDiag()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_MEMWAIT");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[cp] diagnostic FM4_DIAG_MEMWAIT=1: memory waits that sync, every 50th swap\n");
        return enabled;
    }();
    return on;
}
// FM4 Recomp (opt-in FM4_IMMEDIATE_INTERRUPTS=1, with FM4_DEFER_FENCES): an INTERRUPT is raised when the command processor
// reaches it, as Xenia and ShiftGlue's split recording (ShiftGlue SDK src/graphics/command_processor.cpp:2496-
// 2499) do, unless the CPU could learn from it that earlier GPU work finished: a stream store is queued before it (fence order), or
// D3D asked for idle with a WAIT_UNTIL since the last interrupt (FM4's callback emitter writes WAIT_UNTIL 0x20000 only for its idle
// flag; reference/perf/memory_waits.md). A memory wait whose word nothing pending writes, with no store queued, polls for a
// grace period before it syncs: the CPU writes such words (the interrupt handler's acknowledgement), not the GPU.
// FM4 Recomp (FM4_DIAG_BINS=1, diagnostic): draw packets per bin select (predicated tiling: a frame replayed per tile),
// run and skipped by the predicate, tallied every 60 swaps.
bool DiagBins()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_BINS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
struct BinTally { uint64_t run = 0, skipped = 0; };
std::map<uint64_t, BinTally> binDraws;  // key: the bin select of a predicated draw; ~0 for draws that are not predicated
uint64_t binSelectChanges = 0;

uint32_t WaitPollSpinUs()
{
    static const uint32_t us = [] {
        const char* v = std::getenv("FM4_WAIT_POLL_US");
        const unsigned long n = v != nullptr ? std::strtoul(v, nullptr, 10) : 0;
        if (v != nullptr && n != 0)
            std::fprintf(stderr, "[cp] FM4_WAIT_POLL_US=%lu: a wait yields for its first %lu us, then sleeps 50 us at a time\n", n, n);
        return n <= 100000 ? uint32_t(n) : 100000u;
    }();
    return us;
}
int immediateInterruptsOverride = -1;  // SetImmediateInterrupts (tests)
bool ImmediateInterruptsOn()
{
    if (immediateInterruptsOverride >= 0)
        return immediateInterruptsOverride == 1;
    static const bool on = [] {
        const char* v = std::getenv("FM4_IMMEDIATE_INTERRUPTS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[cp] immediate interrupts on (FM4_IMMEDIATE_INTERRUPTS=1): raised when reached unless a store is queued or idle was asked\n");
        return enabled;
    }();
    return on;
}
bool idleAsked = false;  // a nonzero WAIT_UNTIL since the last INTERRUPT (command-processor thread)
uint64_t packetsSeen = 0, waitUntilAt = 0;  // FM4_DIAG_MEMWAIT: packet number of the last nonzero WAIT_UNTIL
uint32_t waitUntilValue = 0;
constexpr uint64_t kCpuWordGraceNs = 2000000;
uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void RaiseInterrupts(uint32_t mask)
{
    for (int n = 0; n < 6; n++)
        if (mask & (1u << n))  // FM4_GUEST_MEMORY_GPU: the callback may store to a page a GPU read guards
            RequestInterrupt(1, true, renderer && renderer->honourGpuReadGuard ? HonourGpuReadGuard : nullptr);
}

void CpStore(uint8_t* base, uint32_t address, uint32_t value)
{
    if (!deferredStores.empty() || (renderer && renderer->deferStore && renderer->deferStore(address & ~3u)))
    {
        deferredStores.push_back({address, value}), deferredBase = base, storeParts.Queued();
        return;
    }
    StoreGpu(base, address, value);
}
// The packets whose only CPU-visible effect is such a store: no sync before them when stores are deferred.
// FM4 Recomp (opt-in FM4_DEFER_EVENTS=1, with FM4_DEFER_FENCES): EVENT_WRITE_EXT's extents and EVENT_WRITE_ZPD's sample counts go
// through the deferred store queue (CpStore, big-endian dwords) like MEM_WRITE: they land at GPU completion in packet order, after
// the resolve write-backs, with no sync before them (measured: 2 of the 5 CPU-sync flushes a countdown frame).
int deferEventsOverride = -1;  // SetDeferEvents (tests)
bool DeferEvents()
{
    if (deferEventsOverride >= 0)
        return deferEventsOverride == 1;
    static const bool on = [] {
        const char* v = std::getenv("FM4_DEFER_EVENTS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[cp] deferred event writes on (FM4_DEFER_EVENTS=1): EVENT_WRITE_EXT and EVENT_WRITE_ZPD store through the deferred queue\n");
        return enabled;
    }();
    return on;
}
bool StoreOnly(uint32_t opcode) { return opcode == 0x3D || opcode == 0x3E || opcode == 0x58 || ((opcode == 0x5A || opcode == 0x5B) && DeferEvents()); }
void SyncBefore(uint32_t opcode)
{
    if (renderer && renderer->syncGuestWrites && !(StoreOnly(opcode) && renderer->deferStore))
        renderer->syncGuestWrites(opcode);  // FM4 Recomp: pending GPU writes land before a CPU sync point
}

void CpWriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegisterCount)
        Stop(StopCode::Unimplemented, "GPU: a write to register %X, past the register file", index);
    if (index == reg::kCoherStatusHost)
        value |= 1u << 31;  // the status bit (command_processor.cc:465-467)
    if (index == reg::kWaitUntil && value != 0)
        idleAsked = true, waitUntilAt = packetsSeen, waitUntilValue = value;
    WriteRegister(base, index, value);
    if (gpu::gamma::IsStateRegister(index))  // DC_LUT_* only (command_processor.cc:469-574); the check inline: this runs per register write
        OnGammaRampRegisterWritten(base, index, value, LutWriter::CommandProcessor);
    if (index >= reg::kScratchReg0 && index < reg::kScratchReg0 + 8 &&
        (ReadRegister(base, reg::kScratchUmsk) >> (index - reg::kScratchReg0) & 1))
        BeforeCpWrite(ReadRegister(base, reg::kScratchAddr) + 4 * (index - reg::kScratchReg0), 4),
        Store32(base, PhysicalToGuest(ReadRegister(base, reg::kScratchAddr) + 4 * (index - reg::kScratchReg0)), value);
}

// FM4 Recomp (M7 CP cost, opt-in FM4_BULK_REGISTERS=1; reference/perf/draw_cost.md R2): a run of consecutive registers
// from the ring (type 0, SET_CONSTANT, SET_CONSTANT2) is one copy when no register in it has a side effect in CpWriteRegister
// (COHER_STATUS_HOST, WAIT_UNTIL, the 8 scratch registers, the gamma ramp's DC_LUT_* state) and it does not wrap the ring;
// otherwise it is written word by word as before. Both the ring and the register file are big-endian guest memory, so the copy
// is byte for byte what Load32 then Store32 per word writes. Adapts SpeedBreaker 312ce69 runtime/gpu/command_processor.cpp:281-307
// (BulkSafe, WriteRegistersBulk; GPL-3.0, technique only: it swaps into a host-order array, which FM4 does not have).
int bulkRegistersOverride = -1;  // SetBulkRegisters (tests)
bool BulkRegisters()
{
    if (bulkRegistersOverride >= 0)
        return bulkRegistersOverride == 1;
    static const bool on = [] {
        const char* v = std::getenv("FM4_BULK_REGISTERS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] bulk register runs on (FM4_BULK_REGISTERS=1): a run of plain registers is one copy\n");
        return enabled;
    }();
    return on;
}
// Whether [index, index + count) is inside the register file and holds no register with a side effect.
bool PlainRegisterRun(uint32_t index, uint32_t count)
{
    auto holds = [&](uint32_t first, uint32_t last) { return first < index + count && index <= last; };  // [first, last] meets the run
    return index < kRegisterCount && count <= kRegisterCount - index && !holds(reg::kCoherStatusHost, reg::kCoherStatusHost) &&
           !holds(reg::kWaitUntil, reg::kWaitUntil) && !holds(reg::kScratchReg0, reg::kScratchReg0 + 7) &&
           !holds(gamma::reg::kRwIndex, gamma::reg::k30Color);
}
// `count` words from the reader to the registers from `index` on (one register when `one`).
void WriteRegisterRun(uint8_t* base, Reader& r, uint32_t index, uint32_t count, bool one)
{
    if (!one && count > 1 && BulkRegisters() && PlainRegisterRun(index, count) && r.position % r.size + count <= r.size)
    {
        std::memcpy(base + RegisterAddress(index), r.base + r.start + 4 * (r.position % r.size), 4ull * count);
        r.position += count, r.left -= count;
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        CpWriteRegister(base, one ? index : index + i, r.Next());
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
        packetsSeen++;
        HonourGpuReadGuard();  // FM4_GUEST_MEMORY_GPU (nothing otherwise): between packets
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
            WriteRegisterRun(base, r, index, count, one);
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
        if (DiagBins() && (opcode == kDrawIndx || opcode == kDrawIndx2))  // FM4_DIAG_BINS
        {
            BinTally& t = binDraws[(packet & 1) ? state.binSelect : ~0ull];
            ((packet & 1) && (state.binSelect & state.binMask) == 0 ? t.skipped : t.run)++;
        }
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
                static std::vector<uint32_t> code;  // FM4 Recomp (M7 CPU cost): storage kept across loads (one allocation a load before); fully overwritten
                code.resize(sizeWords);
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
                static std::vector<uint32_t> code;  // FM4 Recomp (M7 CPU cost): as IM_LOAD_IMMEDIATE's
                code.resize(sizeWords);
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
            // FM4 Recomp (FM4_DEFER_FENCES): raised when the work before it completes, in order behind the queued stores.
            const bool idle = idleAsked;
            idleAsked = false;
            static std::atomic<uint32_t> irqLines{0};
            if (MemWaitDiag() && state.swaps % 50 == 0 && irqLines.fetch_add(1) < 2000)
            {
                char history[16 * 9 + 1] = "";
                for (uint32_t i = 0; i < 16 && i < state.lastCount; i++)
                    std::snprintf(history + 9 * i, 10, " %08X", state.lastHeaders[(state.lastCount - 1 - i) % 16]);
                std::fprintf(stderr, "[cp] interrupt swap %llu: mask %X, idle asked %d (WAIT_UNTIL %08X, %llu packet(s) before), queued %zu, "
                                     "work open %d; headers newest first:%s\n",
                             (unsigned long long)state.swaps, mask, idle ? 1 : 0, waitUntilValue,
                             (unsigned long long)(packetsSeen - waitUntilAt), deferredStores.size(),
                             renderer && renderer->workOpen && renderer->workOpen() ? 1 : 0, history);
            }
            if (renderer && renderer->workOpen && deferredStores.empty() && !idle && ImmediateInterruptsOn())
            {
                immediateInterrupts++;
                RaiseInterrupts(mask);
                break;
            }
            if (renderer && renderer->workOpen && (!deferredStores.empty() || renderer->workOpen()))
            {
                deferredStores.push_back({0, mask, true}), deferredBase = base, storeParts.Queued();
                break;
            }
            SyncBefore(opcode);
            RaiseInterrupts(mask);
            break;
        }
        case kXeSwap:  // (:644-668)
        {
            const uint32_t magic = next(), frontBuffer = next(), width = next(), height = next();
            if (magic != kSwapSignature)
                Stop(StopCode::Unimplemented, "GPU: a swap packet without its signature (%08X)", magic);
            state.swaps++;
            if (DiagBins() && state.swaps % 60 == 0)  // FM4_DIAG_BINS
            {
                std::fprintf(stderr, "[diag bins] swaps %llu-%llu, draw packets by bin select (run/skipped), mask %016llX, %llu select change(s):",
                             (unsigned long long)(state.swaps - 59), (unsigned long long)state.swaps, (unsigned long long)state.binMask,
                             (unsigned long long)binSelectChanges);
                for (const auto& [select, t] : binDraws)
                    std::fprintf(stderr, " %s%016llX %llu/%llu", select == ~0ull ? "unpredicated:" : "", (unsigned long long)select,
                                 (unsigned long long)t.run, (unsigned long long)t.skipped);
                std::fprintf(stderr, "\n");
                binDraws.clear(), binSelectChanges = 0;
            }
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
            // FM4 Recomp (FM4_DEFER_FENCES, ReCarbon command_processor.cpp:168-180): a wait on memory that a queued store of
            // this stream will satisfy (D3D's drain idiom: EVENT_WRITE_SHD, then wait for it) proceeds now: everything recorded after it
            // executes after it in queue order, and its CPU-visible effects stay queued behind it. The newest queued store to the word
            // decides, read as LoadGpu would read it after StoreGpu.
            if (info & 0x10)
            {
                const DeferredStore* last = nullptr;
                for (const DeferredStore& d : deferredStores)
                    if ((d.address & ~3u) == (poll & ~3u))
                        last = &d;
                if (last != nullptr && Matches(GpuSwap(GpuSwap(last->value, last->address & 3), poll & 3) & mask, ref, info))
                {
                    selfWaits++;
                    break;
                }
            }
            // FM4 Recomp: only a wait on memory can observe a pending GPU write or a deferred store; a register wait (MakeCoherent's
            // COHER_STATUS_HOST) cannot, so it needs no sync. A memory wait whose word nothing pending writes polls at once and syncs
            // only if it is not satisfied yet (ReCarbon command_processor.cpp:735-750: most such waits pass at once).
            if (info & 0x10)
            {
                const bool pending = !(renderer && renderer->pendingAt) || renderer->pendingAt(poll & ~3u, 4);
                if (!pending && Matches(LoadGpu(base, poll) & mask, ref, info))
                {
                    immediateWaits++;
                    break;
                }
            }
            const uint64_t waitStart = (info & 0x10) ? NowNs() : 0;
            const uint32_t entryValue = (info & 0x10) ? LoadGpu(base, poll) : 0;
            const size_t queuedAtEntry = deferredStores.size();
            const bool overlapAtEntry = (info & 0x10) && renderer && renderer->pendingAt && renderer->pendingAt(poll & ~3u, 4);
            // FM4 Recomp (FM4_IMMEDIATE_INTERRUPTS): a word only the CPU writes gets the grace period before the sync.
            const bool cpuWord = (info & 0x10) && ImmediateInterruptsOn() && renderer && renderer->pendingAt && !overlapAtEntry && deferredStores.empty();
            if ((info & 0x10) && !cpuWord)
                SyncBefore(opcode);
            bool synced = !cpuWord;
            const uint64_t syncNs = (info & 0x10) ? NowNs() - waitStart : 0;
            const uint32_t afterSync = (info & 0x10) ? LoadGpu(base, poll) : 0;
            const uint64_t pollStart = NowNs();
            uint32_t polls = 0, idleFlushes = 0;
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
                polls++;
                HonourGpuReadGuard();  // FM4_GUEST_MEMORY_GPU: the guest thread this waits for may wait for a GPU read
                // FM4_UNLOCKED_FPS (graphics.h): a memory word the guest CPU must write, the GPU drained, nothing of ours queued: the
                // vblank it may be waiting for comes now (once a wait).
                if (polls == 1 && (info & 0x10) && synced && deferredStores.empty() && UnlockedFrameRate())
                    RequestEarlyVblank();
                if (!synced && NowNs() - waitStart > kCpuWordGraceNs)
                {
                    synced = true, graceSyncs++;
                    SyncBefore(opcode);
                    continue;
                }
                // FM4 Recomp (FM4_DEFER_FENCES): the queued stores and interrupts may be what the guest CPU needs before it writes what
                // this waits for (observed: an INTERRUPT queued, then a wait for the CPU to clear 08D4D002, stop 7 after
                // 10 s). Submit and retire the open work, which applies them, before waiting (ReCarbon command_processor.cpp:745-750).
                if (!deferredStores.empty() && renderer && renderer->ringIdle)
                {
                    renderer->ringIdle();
                    idleFlushes++;
                    continue;
                }
                if (std::chrono::steady_clock::now() > give_up)
                    Stop(StopCode::Unimplemented, "GPU: WAIT_REG_MEM on %s %08X (value %08X & %08X, reference %08X, function %u) never satisfied",
                         info & 0x10 ? "memory" : "register", poll, value, mask, ref, info & 7);
                // FM4 Recomp (opt-in FM4_WAIT_POLL_US=<us>): poll by yielding for the first <us> microseconds of a wait, then sleep
                // 50 us at a time, instead of the packet's interval (FM4's: 1 ms sleeps, so up to 1 ms of latency after the
                // GPU is done and the game's handler has flipped the word; measured: memory waits 19.7 ms a swap
                // against 13.8 ms of GPU sync).
                if (const uint32_t spinUs = WaitPollSpinUs(); spinUs != 0)
                {
                    if (NowNs() - pollStart < uint64_t(spinUs) * 1000)
                        std::this_thread::yield();
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                else
                    std::this_thread::sleep_for(wait >= 0x100 ? std::chrono::microseconds(1000 * (wait / 0x100)) : std::chrono::microseconds(100));
            }
            state.waitingOn = 0;
            if (info & 0x10)
            {
                const uint64_t totalNs = NowNs() - waitStart;
                memWaits++, memWaitNs += totalNs, syncedWaits += synced ? 1 : 0;
                static std::atomic<uint32_t> lines{0};
                if (MemWaitDiag() && state.swaps % 50 == 0 && lines.fetch_add(1) < 4000)
                {
                    const uint32_t final = LoadGpu(base, poll);
                    std::fprintf(stderr, "[cp] memwait swap %llu: %08X & %08X func %u ref %08X: entry %08X, after sync %08X, final %08X; "
                                         "overlap %d, queued %zu; sync %.3f ms, total %.3f ms, %u poll(s), %u idle flush(es); %s\n",
                                 (unsigned long long)state.swaps, poll, mask, info & 7, ref, entryValue, afterSync, final, overlapAtEntry ? 1 : 0,
                                 queuedAtEntry, syncNs / 1e6, totalNs / 1e6, polls, idleFlushes,
                                 Matches(entryValue & mask, ref, info) ? "satisfied at entry"
                                 : Matches(afterSync & mask, ref, info) ? "satisfied by the sync" : "satisfied later");
                }
            }
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
            SyncBefore(opcode);
            const uint32_t index = next(), address = next();
            CpStore(base, address, Register(base, index));
            break;
        }
        case kMemWrite:  // (:803-818)
        {
            SyncBefore(opcode);
            uint32_t address = next();
            while (remaining)
            {
                CpStore(base, address, next());
                address += 4;
            }
            break;
        }
        case kCondWrite:  // (:820-862)
        {
            SyncBefore(opcode);
            const uint32_t info = next(), poll = next(), ref = next(), mask = next(), target = next(), data = next();
            const uint32_t value = (info & 0x10) ? LoadGpu(base, poll) : Register(base, poll);
            if (Matches(value & mask, ref, info))
            {
                if (info & 0x100)
                    CpStore(base, target, data);
                else
                    CpWriteRegister(base, target, data);
            }
            break;
        }
        case kVizQuery:  // (:1318-1350)
        {
            // FM4 Recomp (Car Select: packet C0002300): a visibility query's begin (VIZQUERY_START, 7) or end
            // (VIZQUERY_END, 8) as VGT_EVENT_INITIATOR; at the end the query reads visible, bit `id` of PA_SC_VIZ_QUERY_STATUS_0 / _1, as
            // Xenia (pm4_command_processor_implement.h:1318-1350 fakes "visible": no host occlusion query). One data word.
            if (count != 1)
                Stop(StopCode::Unimplemented, "GPU: VIZ_QUERY with %u words is not modelled", count);
            const uint32_t word = next(), id = word & 0x3F;
            const bool end = (word & 0x100) != 0;
            WriteRegister(base, reg::kVgtEventInitiator, end ? 8u : 7u);
            if (end)
            {
                const uint32_t status = id < 32 ? reg::kPaScVizQueryStatus0 : reg::kPaScVizQueryStatus1;
                WriteRegister(base, status, Register(base, status) | 1u << (id & 31));
            }
            break;
        }
        case kEventWrite:  // (:864-879)
            // FM4 Recomp: no sync point. The packet only sets VGT_EVENT_INITIATOR (no guest memory store, no interrupt), so the guest
            // CPU cannot learn from it that earlier GPU work finished. FM4 issues one after nearly every resolve (every
            // "CPU sync" flush, 25-39 a swap, was this packet).
            WriteRegister(base, reg::kVgtEventInitiator, next() & 0x3F);
            break;
        case kEventWriteShd:  // (:881-913)
        {
            SyncBefore(opcode);
            const uint32_t initiator = next(), address = next(), value = next();
            WriteRegister(base, reg::kVgtEventInitiator, initiator & 0x3F);
            if ((address & ~3u) > 0x1FFFFFFFu && Register(base, reg::kWritebackBase) != 0)
                Stop(StopCode::Unimplemented, "GPU: EVENT_WRITE_SHD to the write-back window (%08X) is not modelled", address);
            // Bit 31: the counter (vblanks and swaps; Xenia counter_, graphics_system.cc:292-297).
            CpStore(base, address, (initiator >> 31) ? uint32_t(VblankCount() + state.swaps) : value);
            break;
        }
        case kSetConstant:  // (:1127-1168): ALU 4000, fetch 4800, bool 4900, loop 4908, registers 2000
        {
            const uint32_t offsetType = next(), type = (offsetType >> 16) & 0xFF;
            const uint32_t bases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
            if (type > 4)
                Stop(StopCode::Unimplemented, "GPU: SET_CONSTANT of type %u", type);
            WriteRegisterRun(base, r, bases[type] + (offsetType & 0x7FF), remaining, false), remaining = 0;
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
            SyncBefore(opcode);
            // Nothing is drawn here, so the extents are Xenia's conservative answer: the whole surface (x and y
            // 0-8192 in units of 8, z 0-1), six 16-bit values stored big-endian as its byte_swap leaves them. Xenia
            // asserts the 8in16 swap; another one stops (7).
            const uint32_t initiator = next(), address = next();
            WriteRegister(base, reg::kVgtEventInitiator, initiator & 0x3F);
            if ((address & 3) != 1)
                Stop(StopCode::Unimplemented, "GPU: EVENT_WRITE_EXT with endian %u is not modelled", address & 3);
            const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
            if (DeferEvents())  // the same bytes as big-endian dwords (endian 2 swaps the host word to the guest's order)
            {
                for (uint32_t i = 0; i < 3; i++)
                    CpStore(base, ((address & ~3u) + 4 * i) | 2, uint32_t(extents[2 * i]) << 16 | extents[2 * i + 1]);
                break;
            }
            const uint32_t at = PhysicalToGuest(address & ~3u);
            for (uint32_t i = 0; i < 6; i++)
                base[at + 2 * i] = uint8_t(extents[i] >> 8), base[at + 2 * i + 1] = uint8_t(extents[i]);
            break;
        }
        case kEventWriteZpd:  // {initiator}: the z-pass-done event of an occlusion query (:951-982)
        {
            SyncBefore(opcode);
            // FM4 Recomp, APPROXIMATE (Xenia's workaround, not the hardware's count): Xenia counts no samples; it clears the guest's
            // xe_gpu_depth_sample_counts (8 little-endian dwords at RB_SAMPLE_COUNT_ADDR: Total, ZFail, ZPass, StencilFail, A and B;
            // xenos.h:1547-1560) and, when D3D has marked the query finished (0xFFFFFEED stored big-endian in both ZPass words, or in
            // both ZFail words: bytes FF FF FE ED), reports query_occlusion_fake_sample_count = 1000 passed samples (gpu_flags.cc:47-51,
            // "If set to -1 no sample counts are written, games may hang") in ZPass_A and Total_A. Real Vulkan occlusion queries: later.
            WriteRegister(base, reg::kVgtEventInitiator, next() & 0x3F);
            const uint32_t address = ReadRegister(base, reg::kRbSampleCountAddr);
            if ((address & 0x1FFFFFFFu) == 0)
                Stop(StopCode::Unimplemented, "GPU: EVENT_WRITE_ZPD with RB_SAMPLE_COUNT_ADDR %08X", address);
            uint8_t* counts = base + PhysicalToGuest(address & 0x1FFFFFFCu);
            const auto finished = [&](uint32_t word) {
                return counts[4 * word] == 0xFF && counts[4 * word + 1] == 0xFF && counts[4 * word + 2] == 0xFE && counts[4 * word + 3] == 0xED;
            };
            const bool end = (finished(4) && finished(5)) || (finished(2) && finished(3));
            if (DeferEvents())  // the same bytes as big-endian dwords: 1000 little-endian is E8 03 00 00
            {
                for (uint32_t word = 0; word < 8; word++)
                    CpStore(base, ((address & 0x1FFFFFFCu) + 4 * word) | 2, end && (word == 0 || word == 4) ? 0xE8030000u : 0u);
                break;
            }
            std::memset(counts, 0, 32);
            if (end)
                for (const uint32_t word : {4u, 0u})  // ZPass_A, Total_A, little-endian
                    counts[4 * word] = 1000 & 0xFF, counts[4 * word + 1] = 1000 >> 8;
            static bool named = false;
            if (!named)
                named = true, std::fprintf(stderr, "[gpu] occlusion queries (EVENT_WRITE_ZPD): Xenia's fixed result, 1000 samples passed (APPROXIMATE)\n");
            break;
        }
        case kSetConstant2:  // (:1170-1179)
        {
            const uint32_t index = next() & 0xFFFF;
            WriteRegisterRun(base, r, index, remaining, false), remaining = 0;
            break;
        }
        case kSetBinMaskLo: state.binMask = (state.binMask & 0xFFFFFFFF00000000ull) | next(); break;  // (:511-546)
        case kSetBinMaskHi: state.binMask = (state.binMask & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
        case kSetBinSelectLo: state.binSelect = (state.binSelect & 0xFFFFFFFF00000000ull) | next(); break;
        case kSetBinSelectHi: state.binSelect = (state.binSelect & 0xFFFFFFFFull) | (uint64_t(next()) << 32); break;
        case kSetBinMask: { const uint64_t hi = next(); state.binMask = hi << 32 | next(); break; }
        case kSetBinSelect: { const uint64_t hi = next(); state.binSelect = hi << 32 | next(); binSelectChanges++; break; }
        default:
            Stop(StopCode::Unimplemented, "GPU: PM4 opcode %02X (packet %08X, %u words) is not modelled", opcode, packet, count);
        }
        while (remaining)  // the rest of the packet
            next();
    }
}

void SetStopAtSwap(uint32_t n) { stopAtSwap = n; }

void ApplyDeferredStores()
{
    // Taken first: an interrupt's guest callback may run on the interrupt thread while this thread goes on.
    const std::vector<DeferredStore> queued = std::move(deferredStores);
    deferredStores.clear();
    storeParts.TakenAll();
    for (const DeferredStore& d : queued)
        if (!d.interrupt)
            StoreGpu(deferredBase, d.address, d.value);
        else
            RaiseInterrupts(d.value);
}
bool DeferredStoresPending() { return !deferredStores.empty(); }
void MarkDeferredStoresPart(uint64_t serial) { storeParts.Submitted(serial); }
size_t ApplyDeferredStoresThrough(uint64_t serial, bool (*canStore)(uint32_t physical))
{
    return ApplyStoresThrough(deferredStores, storeParts, serial, [&](const DeferredStore& d) { return canStore == nullptr || canStore(d.address & 0x1FFFFFFCu); },
                              [](const DeferredStore& d) { StoreGpu(deferredBase, d.address, d.value); });
}
void SetReapHook(void (*reap)()) { reapRetired = reap; }
uint64_t CpIdleNs() { return cpIdleNs.load(); }
uint64_t SelfWaits() { return selfWaits.load(); }
uint64_t ImmediateWaits() { return immediateWaits.load(); }
uint64_t SyncedWaits() { return syncedWaits.load(); }
uint64_t MemWaits() { return memWaits.load(); }
uint64_t MemWaitNs() { return memWaitNs.load(); }
uint64_t ImmediateInterrupts() { return immediateInterrupts.load(); }
uint64_t GraceSyncs() { return graceSyncs.load(); }
void SetImmediateInterrupts(bool on) { immediateInterruptsOverride = on ? 1 : 0; }
void SetDeferEvents(bool on) { deferEventsOverride = on ? 1 : 0; }
void SetBulkRegisters(bool on) { bulkRegistersOverride = on ? 1 : 0; }
void SetRendererHooks(const RendererHooks* hooks) { renderer = hooks; }
bool OnCommandProcessorThread()
{
    const uintptr_t t = cpThread.load(std::memory_order_acquire);
    return t != 0 && pthread_equal(reinterpret_cast<pthread_t>(t), pthread_self());
}
void SetCompletionWorkerThread() { completionThread.store(reinterpret_cast<uintptr_t>(pthread_self()), std::memory_order_release); }
bool OnCompletionWorkerThread()
{
    const uintptr_t t = completionThread.load(std::memory_order_acquire);
    return t != 0 && pthread_equal(reinterpret_cast<pthread_t>(t), pthread_self());
}

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
        pthread_setname_np("fm4 gpu cp");  // FM4 Recomp: thread_cpu.h roles
        cpThread.store(reinterpret_cast<uintptr_t>(pthread_self()), std::memory_order_release);
        for (;;)
        {
            const uint32_t size = ringWords.load(), write = ReadRegister(base, reg::kCpRbWptr) % size, read = readIndex.load();
            if (write == read)
            {
                if (renderer && renderer->honourGpuReadGuard)  // FM4_GUEST_MEMORY_GPU: idle, a guest thread may wait for a GPU read
                {
                    std::lock_guard guard(stateLock);
                    renderer->honourGpuReadGuard();
                }
                if (reapRetired)  // FM4 Recomp (FM4_ASYNC_RETIRE): idle, the parts the worker retired land their stores
                {
                    std::lock_guard guard(stateLock);
                    reapRetired();
                }
                if (!deferredStores.empty() && renderer && renderer->ringIdle)
                {
                    std::lock_guard guard(stateLock);
                    renderer->ringIdle();  // FM4_DEFER_FENCES: a guest thread may poll a queued fence
                    continue;
                }
                const auto idleFrom = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                cpIdleNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idleFrom).count());
                continue;
            }
            {
                std::lock_guard guard(stateLock);
                Execute(base, ringStart.load(), size, read, (write + size - read) % size, ringState);
            }
            readIndex = write;
            if (const uint32_t back = writeBack.load())
            {
                if (renderer && renderer->beforeCpWrite)  // FM4 Recomp (FM4_GUEST_MEMORY_GPU): it may flush the renderer
                {
                    std::lock_guard guard(stateLock);
                    BeforeCpWrite(back, 4);
                }
                Store32(base, PhysicalToGuest(back), write);
            }  // big-endian (command_processor.cc ExecutePrimaryBuffer)
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
