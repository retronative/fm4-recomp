// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// T0 tests of the renderer's integration logic, CPU only: no Vulkan header is included, no instance
// or device is created, and every failure transition runs on a fake backend (GPU
// safety: lifetime transitions are tested with CPU-only models). What is tested is the guard's
// composition of the runtime/gpu models (GPU_STOP before work, gate, per-slot lifetime, bounded
// waits, GPU_STOP written on a stranding, loop-work cap), the capped capture store with real files,
// the front-buffer decode and the PNG encoder. Expected values are computed by hand in the comment
// next to them (call orders and counts follow from the guard's documented contract); "CONTROL"
// names a negative control (a wrong behaviour that must not happen).
//
//   tests/run_renderer_guard_tests.sh
#include "capture_store.h"
#include "capture_keep.h"
#include "front_buffer.h"
#include "gpu_guard.h"
#include "async_submit.h"
#include "png.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace runtime;
using namespace runtime::video;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}

std::string work;  // argv[1]: a fresh directory for flags and captures

std::string Path(const char* name) { return work + "/" + name; }
bool Exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }
std::string FirstLines(const std::string& p, int n)
{
    std::string out;
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f)
        return out;
    char line[512];
    for (int i = 0; i < n && std::fgets(line, sizeof line, f); i++)
        out += line;
    std::fclose(f);
    return out;
}
void Plant(const std::string& p, const char* text)
{
    FILE* f = std::fopen(p.c_str(), "w");
    std::fputs(text, f);
    std::fclose(f);
}

// A fake device: records every call; the wait and status results are set per test (VkResult values:
// 0 VK_SUCCESS, 1 VK_NOT_READY, 2 VK_TIMEOUT, -4 VK_ERROR_DEVICE_LOST).
struct Fake final : SubmissionBackend<char>
{
    int submitResult = 0, waitResult = 0, statusResult = 0;
    int submits = 0, waits = 0, statuses = 0;
    std::vector<uint64_t> waitTimeouts;
    std::string destroyed;  // the resources destroyed, in order
    int Submit(uint32_t) override { return submits++, submitResult; }
    int WaitFence(uint32_t, uint64_t ns) override { return waits++, waitTimeouts.push_back(ns), waitResult; }
    int FenceStatus(uint32_t) override { return statuses++, statusResult; }
    void Destroy(char& r) override { destroyed += r; }
    int Calls() const { return submits + waits + statuses + int(destroyed.size()); }
};

constexpr uint64_t k2s = 2000000000ull;  // D-g: 2 s = 2 000 000 000 ns
using Guard = GpuGuard<char, 2>;
using State = gpu::SubmissionLifetime<char>::State;

void TestGpuStopRefusesBeforeAnyWork()
{
    const std::string flag = Path("stop_present");
    Plant(flag, "GPU_STOP written by the test; only the owner clears it.\nsecond line\n");
    Fake fake;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, flag, k2s);
    const Verdict b = g.Begin("present");
    // CheckGpuStop reports the first line without its line end.
    Expect(!b.ok && !b.stranded && b.reason.find("GPU_STOP written by the test; only the owner clears it.") != std::string::npos &&
               b.reason.find("second line") == std::string::npos,
           "GPU_STOP present: Begin refuses, naming the flag's first line only");
    Expect(g.Hold(0, 'a'), "GPU_STOP present: recording bookkeeping is still allowed (no GPU work)");
    const Verdict s = g.Submit(0);
    Expect(!s.ok && fake.Calls() == 0 && gate.Load().inFlight == 0 && g.SlotState(0) == State::Idle,
           "GPU_STOP present: Submit refused before the backend; nothing entered, the slot stays idle");
    Expect(FirstLines(flag, 1) == "GPU_STOP written by the test; only the owner clears it.\n", "CONTROL: a refusal never rewrites the flag");
}

void TestGpuStopAbsentAndUncheckable()
{
    Fake fake;
    gpu::SubmissionGate gate;
    Guard ok(fake, gate, Path("stop_absent"), k2s);
    Expect(ok.Begin("present").ok, "GPU_STOP absent: Begin allowed");
    // The parent of the path is a regular file: the check cannot establish absence (ENOTDIR): refuse.
    Plant(Path("a_file"), "x");
    Guard bad(fake, gate, Path("a_file") + "/GPU_STOP", k2s);
    Expect(!bad.Begin("present").ok && !bad.Submit(0).ok && fake.Calls() == 0, "GPU_STOP uncheckable (parent not a directory): refused, fail closed");
    Guard none(fake, gate, "", k2s);
    Expect(!none.Begin("present").ok, "GPU_STOP path empty: refused (no silent default)");
}

void TestFlagPlantedMidRun()
{
    const std::string flag = Path("stop_mid");
    Fake fake;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, flag, k2s);
    Expect(g.Submit(0).ok && g.Retire(0).ok && fake.submits == 1, "first submission and retire before the flag");
    Plant(flag, "lock-up evidence\n");
    const Verdict v = g.Submit(0);
    Expect(!v.ok && fake.submits == 1 && v.reason.find("lock-up evidence") != std::string::npos,
           "a flag planted between submissions refuses the next one before the backend");
}

void TestDestructionDefersUntilConfirmed()
{
    Fake fake;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, Path("stop_defer"), k2s);
    Expect(g.Hold(0, 'a') && g.Hold(0, 'b'), "two resources held by idle slot 0");
    Expect(g.Submit(0).ok && fake.submits == 1 && gate.Load().inFlight == 1 && g.SlotState(0) == State::InFlight,
           "submitted: one in flight, slot 0 in flight");
    Expect(fake.destroyed.empty() && g.Held(0) == 2, "in flight: nothing destroyed");
    Expect(!g.Hold(0, 'c') && g.Held(0) == 2, "CONTROL: a resource cannot join a slot in flight");
    Expect(g.Hold(1, 'x') && g.Submit(1).ok && gate.Load().inFlight == 2, "slot 1 is independent: held and submitted");
    Expect(!g.Submit(0).ok && fake.submits == 2, "CONTROL: slot 0 cannot be submitted again while in flight");
    const Verdict r = g.Retire(0);
    // Confirmed (wait 0, status 0): a then b, oldest first; slot 1's x untouched.
    Expect(r.ok && fake.destroyed == "ab" && g.Held(0) == 0 && g.Held(1) == 1 && gate.Load().inFlight == 1,
           "confirmed: slot 0's resources destroyed oldest first, slot 1's kept, one left in flight");
    Expect(fake.waitTimeouts.size() == 1 && fake.waitTimeouts[0] == k2s, "the wait was bounded by exactly 2 000 000 000 ns");
    Expect(!g.Retire(0).ok && fake.waits == 1, "CONTROL: retiring an idle slot is refused without a wait");
    Expect(g.Retire(1).ok && fake.destroyed == "abx" && gate.Load().inFlight == 0, "slot 1 retired: x destroyed, nothing in flight");
    Expect(g.Shutdown() == 0 && fake.waits == 2, "shutdown when idle: no wait, nothing kept");
}

// A completion that is not confirmed: nothing destroyed, GPU_STOP written, nothing more submitted.
void Strands(int waitResult, int statusResult, const char* flagName, const char* name)
{
    const std::string flag = Path(flagName);
    Fake fake;
    fake.waitResult = waitResult, fake.statusResult = statusResult;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, flag, k2s);
    g.Hold(0, 'a');
    g.Submit(0);
    const Verdict r = g.Retire(0);
    const std::string lines = FirstLines(flag, 2);
    const bool ok = !r.ok && r.stranded && fake.destroyed.empty() && g.Held(0) == 1 && g.SlotState(0) == State::Stranded &&
                    gate.Load().stranded && gpu::CheckGpuStop(flag.c_str()).stopped && lines.rfind("GPU_STOP written ", 0) == 0 &&
                    lines.find("by fm4recomp (in-process); only the owner clears it.\n") != std::string::npos &&
                    lines.find("completion unknown") != std::string::npos && !g.Submit(1).ok && fake.submits == 1;
    Expect(ok, name);
}

void TestStranding()
{
    Strands(2, 1, "stop_timeout", "wait timed out (VK_TIMEOUT, VK_NOT_READY): stranded, kept, GPU_STOP written, no further submit");
    Strands(0, 1, "stop_status", "wait succeeded but status not ready: stranded (both must succeed)");
    Strands(-4, -4, "stop_lost", "device lost: stranded");
    Strands(0, -4, "stop_status_lost", "wait succeeded, status device lost: stranded");

    // Shutdown after a stranding: the final wait is made (the fence reached the queue) and, still
    // unknown, keeps the resource; the gate stays stranded.
    Fake fake;
    fake.waitResult = 2, fake.statusResult = 1;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, Path("stop_shutdown"), k2s);
    g.Hold(0, 'a');
    g.Submit(0);
    g.Retire(0);
    Expect(g.Shutdown() == 1 && fake.waits == 2 && fake.destroyed.empty() && gate.Load().stranded, "shutdown after a timeout: final wait, still unknown, kept");
    // The same, but the final wait confirms: released (submission_lifetime.h Shutdown).
    Fake late;
    late.waitResult = 2, late.statusResult = 1;
    gpu::SubmissionGate gate2;
    Guard g2(late, gate2, Path("stop_shutdown2"), k2s);
    g2.Hold(0, 'a');
    g2.Submit(0);
    g2.Retire(0);
    late.waitResult = 0, late.statusResult = 0;
    Expect(g2.Shutdown() == 0 && late.destroyed == "a" && gate2.Load().stranded, "shutdown after a timeout: a confirmed final wait releases; the gate stays stranded");
}

void TestSubmitFailure()
{
    const std::string flag = Path("stop_submit");
    Fake fake;
    fake.submitResult = -4;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, flag, k2s);
    g.Hold(0, 'a');
    const Verdict s = g.Submit(0);
    Expect(!s.ok && s.stranded && fake.waits == 0 && gate.Load().stranded && Exists(flag) && s.reason.find("VkResult -4") != std::string::npos,
           "submit failed: stranded at once, no wait, GPU_STOP written");
    Expect(!g.Retire(0).ok && fake.waits == 0, "CONTROL: no fence of a failed submit is waited on");
    Expect(g.Shutdown() == 1 && fake.waits == 0 && fake.destroyed.empty(), "shutdown after a failed submit: no wait, kept");
}

void TestStopping()
{
    Fake fake;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, Path("stop_stopping"), k2s);
    gate.BeginStop();
    const Verdict s = g.Submit(0);
    Expect(!s.ok && !s.stranded && fake.Calls() == 0 && s.reason.find("stopping") != std::string::npos, "a stop has begun: refused before the backend");
}

void TestTimeoutFromEnvironment()
{
    // FenceTimeoutFromEnv("2000") = 2000 ms -> 2 000 000 000 ns; "50" clamps to 100 ms.
    Fake fake;
    gpu::SubmissionGate gate;
    Guard g(fake, gate, Path("stop_env"), gpu::FenceTimeoutNs(gpu::FenceTimeoutFromEnv("2000").ms));
    g.Submit(0);
    g.Retire(0);
    Expect(fake.waitTimeouts == std::vector<uint64_t>{k2s}, "the backend receives the configured limit (2000 ms -> 2e9 ns)");
    Expect(gpu::FenceTimeoutNs(gpu::FenceTimeoutFromEnv("50").ms) == 100000000ull, "a limit below 100 ms is clamped to 1e8 ns");
}

void TestWriteGpuStopAppends()
{
    const std::string flag = Path("stop_append");
    Plant(flag, "GPU_STOP written 2026-11-06 12:00:00 by scripts/gpu_run.sh; only the owner clears it.\nevidence\n");
    std::string error;
    Expect(WriteGpuStop(flag.c_str(), "second reason", error), "an existing flag accepts a reason");
    Expect(FirstLines(flag, 3) == "GPU_STOP written 2026-11-06 12:00:00 by scripts/gpu_run.sh; only the owner clears it.\nevidence\nsecond reason\n",
           "CONTROL: an existing flag is appended to, never truncated");
    Expect(!WriteGpuStop("", "x", error) && !error.empty(), "an empty path is an error");
}

void TestLoopWork()
{
    // Two loops of 64 (low bytes of 0x01020340 and 0x40) = 2^12 per invocation: 2^14 invocations
    // reach 2^26 exactly (allowed), 2^14 + 1 exceed it.
    const uint32_t loops[] = {0x01020340u, 0x40u};
    Expect(Guard::DrawLoopWork({}, 1ull << 40, loops, 16384).ok, "loop work 2^26 exactly: allowed; a stage without loops is unbounded");
    const Verdict ps = Guard::DrawLoopWork({}, 0, loops, 16385);
    Expect(!ps.ok && ps.reason.find("pixel") != std::string::npos, "loop work 2^26 + 2^12 (pixel): refused, naming the stage");
    const Verdict vs = Guard::DrawLoopWork(loops, 16385, {}, 0);
    Expect(!vs.ok && vs.reason.find("vertex") != std::string::npos, "loop work over the cap (vertex): refused, naming the stage");
}

void TestKeepEvery()
{
    // FM4_CAPTURE_KEEP_EVERY: unset or empty = off; "300" = every 300th; 1 and 1000000 accepted; 0, 1000001, "3x", "-1" malformed.
    const KeepEvery off = KeepEveryFromEnv(nullptr), empty = KeepEveryFromEnv(""), n = KeepEveryFromEnv("300");
    Expect(off.every == 0 && !off.malformed && empty.every == 0 && !empty.malformed && n.every == 300 && !n.malformed &&
               KeepEveryFromEnv("1").every == 1 && KeepEveryFromEnv("1000000").every == 1000000,
           "FM4_CAPTURE_KEEP_EVERY: off unless set; a decimal interval from 1 to 1000000");
    Expect(KeepEveryFromEnv("0").malformed && KeepEveryFromEnv("1000001").malformed && KeepEveryFromEnv("3x").malformed &&
               KeepEveryFromEnv("-1").malformed && KeepEveryFromEnv("99999999999999999999999").malformed,
           "FM4_CAPTURE_KEEP_EVERY: 0, too large, non-digits and overflow are malformed");
}

void TestCaptureStore()
{
    // Records of 10 bytes, at most 2: the third evicts (and deletes) the first.
    CaptureStore<2> ring(work, "cap_", 100);
    const auto write10 = [](const std::string& p) {
        FILE* f = std::fopen(p.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite("0123456789", 1, 10, f) == 10;
        return std::fclose(f) == 0 && ok;
    };
    std::string error;
    bool all = true;
    for (uint64_t id = 1; id <= 3; id++)
        all = ring.Add(id, 10, write10, error) && all;
    Expect(all && !Exists(ring.PathOf(1)) && Exists(ring.PathOf(2)) && Exists(ring.PathOf(3)) && ring.Count() == 2 && ring.Bytes() == 20,
           "capacity 2: the third capture deletes the first's file; 2 files, 20 bytes");
    // Byte cap 25: a 20-byte record after two of 10 evicts both (10 + 10 + 20 > 25, 10 + 20 > 25).
    CaptureStore<4> bytes(work, "bytes_", 25);
    bytes.Add(1, 10, write10, error);
    bytes.Add(2, 10, write10, error);
    bytes.Add(3, 20, write10, error);
    Expect(!Exists(bytes.PathOf(1)) && !Exists(bytes.PathOf(2)) && Exists(bytes.PathOf(3)) && bytes.Count() == 1, "byte cap 25: a 20-byte capture evicts both 10-byte ones");
    Expect(!bytes.Add(4, 26, write10, error) && !Exists(bytes.PathOf(4)) && Exists(bytes.PathOf(3)),
           "a capture over the whole cap is refused, not written, and evicts nothing");
    CaptureStore<2> unwritable(work + "/no_such_dir", "x_", 100);
    Expect(!unwritable.Add(1, 10, write10, error) && error.find("cannot write") != std::string::npos, "a failed write is an error");
}

void TestFrontBufferDecode()
{
    // fetch[0]: tiled (bit 31) | pitch 1280 / 32 = 40 at bits 22-30 -> bit 31 | 40 << 22.
    // fetch[1]: base 0x1B441000 | endian 0 << 6 | format 54 (0x36) -> 0x1B441036.
    // fetch[3]: swizzle XYZW 0x688 at bits 1-12 -> 0xD10.
    uint32_t fetch[6] = {1u << 31 | 40u << 22, 0x1B441036u, 0, 0xD10u, 0, 0};
    FrontBuffer f = DecodeFrontBuffer(fetch, 0x1B441000u, 1280, 720);
    Expect(f.ok && f.pitch == 1280 && f.format == 54 && f.physical == 0x1B441000u && !f.swapRedBlue, "front buffer: tiled, pitch 1280, format 54, XYZW");
    fetch[3] = 0x60Au << 1;  // ZYXW: 0xC14
    Expect(DecodeFrontBuffer(fetch, 0x1B441000u, 1280, 720).swapRedBlue, "swizzle ZYXW: red and blue exchanged");
    fetch[3] = 0xA88u << 1;  // XYZ1
    Expect(DecodeFrontBuffer(fetch, 0x1B441000u, 1280, 720).ok, "swizzle XYZ1 accepted");
    fetch[3] = 0x111u << 1;  // (1,2,4,0): not a front-buffer swizzle
    Expect(!DecodeFrontBuffer(fetch, 0x1B441000u, 1280, 720).ok, "another swizzle: refused");
    fetch[3] = 0xD10u;
    uint32_t linear[6] = {0x0A000000u, 0x1B441036u, 0, 0xD10u, 0, 0};  // tiled bit clear
    Expect(!DecodeFrontBuffer(linear, 0x1B441000u, 1280, 720).ok, "a linear front buffer: refused");
    uint32_t swapped[6] = {1u << 31 | 40u << 22, 0x1B441000u | 2u << 6 | 54u, 0, 0xD10u, 0, 0};  // endian 2 (8in32)
    Expect(!DecodeFrontBuffer(swapped, 0x1B441000u, 1280, 720).ok, "a byte-swapped front buffer: refused");
    uint32_t eight[6] = {1u << 31 | 40u << 22, 0x1B441006u, 0, 0xD10u, 0, 0};  // format 6, k_8_8_8_8
    Expect(!DecodeFrontBuffer(eight, 0x1B441000u, 1280, 720).ok, "format 6: refused until presented");
    Expect(!DecodeFrontBuffer(fetch, 0x1B442000u, 1280, 720).ok, "fetch base and swap packet differ: refused");
    uint32_t narrow[6] = {1u << 31 | 32u << 22, 0x1B441036u, 0, 0xD10u, 0, 0};  // pitch 1024 < 1280
    Expect(!DecodeFrontBuffer(narrow, 0x1B441000u, 1280, 720).ok, "pitch below the width: refused");
    // R = 0x3FF, A = 3: 0xC00003FF -> B = 0x3FF: 0xFFF00000 (bits 20-29 set, 0-9 clear, alpha kept).
    Expect(SwapRedBlue1010102(0xC00003FFu) == 0xFFF00000u && SwapRedBlue1010102(0x000FFC00u) == 0x000FFC00u, "red/blue exchange keeps G and A");
}

void TestDetileProperties()
{
    // Formula-independent: within one 32x32 tile of 4-byte texels, the 1024 offsets are distinct,
    // 4-byte aligned and below 4096 (a permutation of the tile's texels).
    std::set<uint32_t> seen;
    bool inside = true;
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
        {
            const uint32_t o = TiledOffset(x, y, 32, 2);
            inside = inside && o < 4096 && o % 4 == 0;
            seen.insert(o);
        }
    Expect(inside && seen.size() == 1024, "one tile: 1024 distinct aligned offsets below 4096");
    // 1280 x 720 at pitch 1280: 720 rows padded to 736 (23 tiles) -> 1280 * 736 * 4 = 3 768 320 bytes.
    Expect(TiledSize32(1280, 720) == 3768320u, "tiled size of 1280x720 is 3 768 320 bytes");
    std::vector<uint8_t> tiled(4096 - 4);
    uint32_t out[1024];
    Expect(!Detile32(tiled.data(), tiled.size(), out, 32, 32, 32), "CONTROL: a source shorter than the padded tile is refused");
}

// Expected offsets from Xenia xenia-canary 67f16c4e315f src/xenia/gpu/texture_util.cc:443-459
// GetTiledOffset2D (the formula Xenia's swap path executes, shaders/texture_address.xesli:15-30),
// evaluated by an independent transcription of that function only; two rows are also computed by
// hand.
// 4 bytes per texel (log2 2). Xenia aligns the pitch up to 32 texels first (xe::align).
struct TiledPoint
{
    uint32_t x, y, pitch, offset;
};
constexpr TiledPoint kXeniaAligned[] = {
    {0, 0, 32, 0},         {31, 0, 32, 236},       {0, 31, 32, 3984},     {31, 31, 32, 3964},     {1, 0, 32, 4},
    {0, 1, 32, 16},        {7, 0, 32, 44},         {8, 0, 32, 64},        {0, 7, 32, 784},        {0, 8, 32, 1152},
    {0, 16, 32, 2048},     {8, 8, 32, 1216},       {16, 16, 32, 2176},    {32, 0, 1280, 4096},    {0, 32, 1280, 163840},
    {33, 1, 1280, 4116},   {0, 719, 1280, 3606416}, {1279, 0, 1280, 159980}, {1279, 719, 1280, 3766140}, {640, 360, 1280, 1885312},
};
// A pitch that is not a multiple of 32: Xenia rounds 1000 up to 1024 (32 tiles per row).
constexpr TiledPoint kXeniaUnaligned[] = {{0, 32, 1000, 131072}, {999, 32, 1000, 258092}};

template <size_t N>
bool Matches(const TiledPoint (&points)[N], const char* what)
{
    bool ok = true;
    for (const TiledPoint& p : points)
    {
        const uint32_t got = TiledOffset(p.x, p.y, p.pitch, 2);
        if (got != p.offset)
            std::printf("  %s: (%u, %u) pitch %u: TiledOffset %u, Xenia %u\n", what, p.x, p.y, p.pitch, got, p.offset);
        ok = ok && got == p.offset;
    }
    return ok;
}

void TestTiledOffsetAgainstXenia()
{
    Expect(Matches(kXeniaAligned, "aligned"), "TiledOffset equals Xenia GetTiledOffset2D at 20 points (pitch 32 and 1280)");
    // Xenia's upper bound of a 1000x64 surface at pitch 1000: 32 tiles per row, 2 tile rows: 262 144.
    Expect(Matches(kXeniaUnaligned, "unaligned") && TiledSize32(1000, 64) == 262144u, "TiledOffset and TiledSize32 equal Xenia at pitch 1000 (Xenia aligns the pitch up to 1024)");
    // Whole-region digests (oracle_tiled_offset.py): sum of offset * (1 + x + pitch * y) over the
    // first tile at pitch 32, and over rows 0-63 at pitch 1280 (two tile rows, 80 tiles).
    uint64_t tile = 0, rows = 0;
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            tile += uint64_t(TiledOffset(x, y, 32, 2)) * (1 + x + 32 * y);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 1280; x++)
            rows += uint64_t(TiledOffset(x, y, 1280, 2)) * (1 + x + 1280 * y);
    Expect(tile == 1430556672ull, "weighted digest of the first tile (pitch 32) equals Xenia's: 1 430 556 672");
    Expect(rows == 689766811893760ull, "weighted digest of rows 0-63 at pitch 1280 equals Xenia's: 689 766 811 893 760");
    // Xenia GetTiledAddressUpperBound2D(1280, 720, 1280, 2) (texture_util.cc:490-518): the origin of
    // the tile holding (1279, 719) is GetTiledOffset2D(1248, 704) = 919 * 512 * 8 = 3 764 224, plus
    // 0x400 << 2 = 4096: 3 768 320, which is the size Detile32 requires. The same at 32x32: 4096.
    Expect(TiledSize32(1280, 720) == 3768320u && TiledSize32(32, 32) == 4096u, "TiledSize32 equals Xenia's tiled address upper bound (1280x720, 32x32)");
    // Detile32 end to end: a 1280x720 tiled source whose word i holds i; the linear texel (x, y)
    // must be Xenia's offset / 4 (row-major output, texels copied whole).
    std::vector<uint8_t> tiled(TiledSize32(1280, 720));
    for (uint32_t i = 0; i < tiled.size() / 4; i++)
        std::memcpy(&tiled[size_t(i) * 4], &i, 4);
    std::vector<uint32_t> linear(size_t(1280) * 720, 0xFFFFFFFFu);
    bool placed = Detile32(tiled.data(), tiled.size(), linear.data(), 1280, 720, 1280);
    for (const TiledPoint& p : kXeniaAligned)
        if (p.pitch == 1280)
            placed = placed && linear[size_t(p.y) * 1280 + p.x] == p.offset / 4;
    Expect(placed, "Detile32 of 1280x720: the linear texel at each Xenia point holds that point's tiled word");
    // A source of exactly the padded size is accepted; one byte short is refused (CONTROL).
    Expect(!Detile32(tiled.data(), tiled.size() - 1, linear.data(), 1280, 720, 1280), "CONTROL: 1280x720 one byte short of 3 768 320 is refused");
}

void TestPng()
{
    // 1x1 black: signature 8 + IHDR 12 + 13 + IDAT 12 + (2 zlib + 5 block header + 4 raw + 4 adler = 15)
    // + IEND 12 = 72 bytes. Adler-32 of {0,0,0,0}: a = 1, b = 4 -> 0x00040001. IEND's CRC is
    // AE 42 60 82 (the CRC-32 of "IEND"). Offsets: IHDR 8-32, IDAT length 33-36, type 37-40, data 41-55
    // (zlib header 41-42, block header 43-47, raw 48-51, Adler-32 52-55), CRC 56-59; IEND 60-71.
    const std::vector<uint8_t> png = EncodePngRgb8(1, 1, {0, 0, 0});
    const bool sig = png.size() == 72 && png[0] == 0x89 && png[1] == 'P' && png[7] == '\n';
    const bool adler = png[52] == 0x00 && png[53] == 0x04 && png[54] == 0x00 && png[55] == 0x01;
    const bool iend = png[68] == 0xAE && png[69] == 0x42 && png[70] == 0x60 && png[71] == 0x82;
    Expect(sig && adler && iend, "PNG of one black pixel: 72 bytes, Adler-32 00040001, IEND CRC AE426082");
    // 2x2 RGB, samples 1..12: the complete file from ISO/IEC 15948, RFC 1950 and RFC 1951
    // (an independent transcription). Raw scanlines
    // 0 1 2 3 4 5 6 | 0 7 8 9 10 11 12 (14 bytes); Adler-32 a = 79, b = 399 -> 018F004F; one final
    // stored block 01 0E 00 F1 FF; CRC-32 (zlib.crc32) IHDR FDD49A73, IDAT 9E358949, IEND AE426082.
    const std::vector<uint8_t> expected2x2 = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A,
        0x73, 0x00, 0x00, 0x00, 0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x0E, 0x00, 0xF1, 0xFF,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x01, 0x8F,
        0x00, 0x4F, 0x9E, 0x35, 0x89, 0x49, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
        0x60, 0x82};
    const std::vector<uint8_t> small = EncodePngRgb8(2, 2, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    Expect(small == expected2x2, "PNG of 2x2 RGB (samples 1-12): the 82 bytes derived from the specifications");
    // Two stored blocks: 256x86 RGB -> raw 86 * (1 + 768) = 66134 = 65535 + 599 bytes. IDAT data is
    // 2 + (5 + 65535) + (5 + 599) + 4 = 66150 bytes (length field 00 01 02 66 at 33-36, data at
    // 41-66190); block 1 header at 43: 00 FF FF 00 00 (not final, LEN 65535, NLEN 0); block 2 header
    // at 65583: 01 57 02 A8 FD (final, LEN 599 = 0x257, NLEN 0xFDA8); the payloads are the raw
    // scanlines; Adler-32 78208681 at 66187-66190 (png_expected.py); IEND at 66195; 66207 bytes.
    std::vector<uint8_t> big(size_t(256) * 86 * 3);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = uint8_t(3 * i + 7);
    std::vector<uint8_t> raw;
    for (size_t y = 0; y < 86; y++)
        raw.push_back(0), raw.insert(raw.end(), big.begin() + y * 768, big.begin() + (y + 1) * 768);
    const std::vector<uint8_t> two = EncodePngRgb8(256, 86, big);
    const bool shape = two.size() == 66207 && std::vector<uint8_t>(two.begin() + 33, two.begin() + 37) == std::vector<uint8_t>{0x00, 0x01, 0x02, 0x66} &&
                       std::vector<uint8_t>(two.begin() + 43, two.begin() + 48) == std::vector<uint8_t>{0x00, 0xFF, 0xFF, 0x00, 0x00} &&
                       std::vector<uint8_t>(two.begin() + 65583, two.begin() + 65588) == std::vector<uint8_t>{0x01, 0x57, 0x02, 0xA8, 0xFD} &&
                       std::vector<uint8_t>(two.begin() + 66187, two.begin() + 66191) == std::vector<uint8_t>{0x78, 0x20, 0x86, 0x81} &&
                       std::equal(two.begin() + 66195, two.begin() + 66199, "\x00\x00\x00\x00") && two[66199] == 'I' && two[66202] == 'D';
    const bool payload = shape && std::equal(raw.begin(), raw.begin() + 65535, two.begin() + 48) && std::equal(raw.begin() + 65535, raw.end(), two.begin() + 65588);
    Expect(shape && payload, "PNG of 256x86: two stored blocks (65535 + 599) with the headers, payloads and Adler-32 of RFC 1951/1950");
    // Refused inputs produce no bytes (never a read past the end or a block-less zlib stream).
    Expect(EncodePngRgb8(2, 2, {1, 2, 3}).empty() && EncodePngRgb8(1, 1, {1, 2, 3, 4}).empty(), "CONTROL: an rgb buffer that is not width * height * 3 bytes is refused");
    Expect(EncodePngRgb8(0, 0, {}).empty() && EncodePngRgb8(0, 4, {}).empty(), "CONTROL: an empty image is refused");
    // Both files are left in the work directory (KEEP_WORK=1) for the wiring checks a reviewer
    // runs by hand: Python's zlib inflates the IDAT and sips reads the size (not oracles).
    for (const auto& [name, bytes] : {std::pair{"png_2x2.png", &small}, std::pair{"png_256x86.png", &two}})
        if (FILE* f = std::fopen(Path(name).c_str(), "wb"))
            std::fwrite(bytes->data(), 1, bytes->size(), f), std::fclose(f);
}

}  // namespace


// FM4_ASYNC_SUBMIT (async_submit.h): the worker submits in queue order; a wait or status query first awaits the worker; a failed
// real submit surfaces as that VkResult at the fence wait, so the guard strands the slot, and nothing is submitted after it.
struct Ordered final : SubmissionBackend<char>
{
    std::vector<uint32_t> order;
    int failAtSubmit = -1, submits = 0;  // the n-th real Submit (0-based) fails with -4 (VK_ERROR_DEVICE_LOST)
    std::string destroyed;
    int Submit(uint32_t s) override
    {
        const int n = submits++;
        if (n == failAtSubmit)
            return -4;
        order.push_back(s);
        return 0;
    }
    int WaitFence(uint32_t, uint64_t) override { return 0; }
    int FenceStatus(uint32_t) override { return 0; }
    void Destroy(char& r) override { destroyed += r; }
};
void TestAsyncSubmit()
{
    const std::string flag = Path("stop_absent_async");
    {
        Ordered real;
        gpu::SubmissionGate gate;
        AsyncSubmitBackend<char> async(real, 4, nullptr);
        GpuGuard<char, 4> g(async, gate, flag, k2s);
        for (uint32_t s = 0; s < 4; s++)
            Expect(g.Hold(s, char('a' + s)), "async: a resource held per slot");
        for (uint32_t s = 0; s < 4; s++)
            Expect(bool(g.Submit(s)), "async: the guard accepts the queued submit");
        for (uint32_t s = 0; s < 4; s++)
            Expect(bool(g.Retire(s)), "async: the retire awaits the worker, then the fence");
        Expect(real.order == std::vector<uint32_t>{0, 1, 2, 3}, "async: the real submits in queue order");
        Expect(real.destroyed == "abcd", "async: resources destroyed after confirmed completions, in order");
        Expect(async.Submits() == 4 && async.Failed() == 0, "async: four submits, no failure");
        async.Stop();
        Expect(async.FenceStatus(0) == 0, "async: a status query after the worker stopped reaches the real backend");
    }
    {
        Ordered real;
        real.failAtSubmit = 1;
        gpu::SubmissionGate gate;
        AsyncSubmitBackend<char> async(real, 4, nullptr);
        GpuGuard<char, 4> g(async, gate, flag, k2s);
        for (uint32_t s = 0; s < 3; s++)
            g.Hold(s, char('a' + s));
        Expect(bool(g.Submit(0)) && bool(g.Submit(1)) && bool(g.Submit(2)), "async: the guard cannot see the failure at submit time");
        Expect(bool(g.Retire(0)), "async: slot 0 retires");
        const Verdict r1 = g.Retire(1);
        Expect(!r1 && r1.stranded, "async: the failed real submit strands slot 1 at its retire");
        Expect(async.Failed() == -4, "async: the failure is remembered");
        async.Stop();
        Expect(real.order == std::vector<uint32_t>{0}, "async: nothing reached the queue after the failure (slot 2 refused)");
        Expect(async.FenceStatus(2) == -4, "async: slot 2 reports the failure, not VK_NOT_READY");
        Expect(!bool(g.Retire(2)), "async control: slot 2 is not confirmed (stranded or no longer awaited)");
    }
}
int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <work dir>\n", argv[0]);
        return 2;
    }
    work = argv[1];
    TestGpuStopRefusesBeforeAnyWork();
    TestGpuStopAbsentAndUncheckable();
    TestFlagPlantedMidRun();
    TestDestructionDefersUntilConfirmed();
    TestStranding();
    TestSubmitFailure();
    TestStopping();
    TestTimeoutFromEnvironment();
    TestWriteGpuStopAppends();
    TestLoopWork();
    TestKeepEvery();
    TestCaptureStore();
    TestFrontBufferDecode();
    TestDetileProperties();
    TestTiledOffsetAgainstXenia();
    TestPng();
    TestAsyncSubmit();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
