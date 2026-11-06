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
#include "front_buffer.h"
#include "gpu_guard.h"
#include "png.h"
#include <cstdio>
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
}

}  // namespace

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
    TestCaptureStore();
    TestFrontBufferDecode();
    TestDetileProperties();
    TestPng();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
