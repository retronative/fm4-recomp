// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the GPU texture load's CPU side (FM4_TEXTURE_GPU_UNTILE; runtime/video/texture_untile_gpu.h). No Vulkan header, no
// device. The shader's function body (FM4_UNTILE_GPU_BODY) is expanded here as C++ (namespace model) and run invocation by
// invocation over synthetic textures: every block size (1, 2, 4, 8, 16 bytes), every endian each size accepts, tiled and linear,
// odd sizes, mip chains and packed-mip origins, several ring placements and storage alignments. Expected values are independent of
// the new code: FillStaging (texture_untile.cpp, itself tested against Xenia), Xenia xenia-canary 67f16c4e315f
// src/xenia/gpu/texture_util.cc:443-458 GetTiledOffset2D transcribed below, and hand computation (worked in the comments). The
// emitted GLSL is written to the directory given as argv[1] for the runner's glslangValidator / spirv-val. "CONTROL" names a
// negative control.
//
//   tests/run_texture_untile_gpu_tests.sh
#include "texture_untile_gpu.h"
#include "texture_untile.h"
#include "texture_upload.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace runtime::video;

namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const std::string& name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name.c_str());
    failures += !ok;
    controls += name.rfind("CONTROL", 0) == 0;
}
}  // namespace

// The shader's body as C++: XeGuestDword reads the guest binding (dword `i` from its offset), flagging a read past its range.
namespace model {
using uint = uint32_t;
const uint8_t* binding = nullptr;
uint64_t bindingDwords = 0;
bool outside = false;
uint XeGuestDword(uint i)
{
    if (i >= bindingDwords)
        return outside = true, 0;
    uint v;
    std::memcpy(&v, binding + uint64_t(i) * 4, 4);
    return v;
}
FM4_UNTILE_GPU_BODY
}  // namespace model

namespace {

// texture_util.cc:443-458 GetTiledOffset2D (as tests/runtime/test_texture_vk.cpp transcribes it).
uint32_t XeniaTiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2)
{
    pitch = (pitch + 31) & ~31u;                                                    // :446 xe::align(pitch, 32)
    const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7);      // :448
    const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2;                    // :450
    const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);  // :452
    return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Guest memory with physical address p at byte p (as the imported buffer); textures start at kGuestStart.
constexpr uint32_t kGuestStart = 0x00100000u;
struct Guest
{
    std::vector<uint8_t> bytes;
};
const uint8_t* GuestOf(void* context, uint32_t physical, uint32_t size, std::string& error)
{
    const Guest& g = *static_cast<const Guest*>(context);
    if (physical < kGuestStart || uint64_t(physical) + size > g.bytes.size())
        return error = "outside the fake guest memory", nullptr;
    return g.bytes.data() + physical;
}

struct Synth
{
    TextureDescription d;
    LoadPlan plan;
    uint32_t end = 0;  // the first physical byte past the last level's storage
};
// A 2D texture of the given levels ({widthBlocks, heightBlocks, x, y} in blocks), laid out one level after the other from
// kGuestStart at 4 KB boundaries: tiled with the pitch the region needs aligned to 32 blocks and the Xenia extent
// (TiledAddressUpperBound2D); linear with rows aligned to 256 bytes. Staging offsets 16-byte aligned, as texture_cache.h's plan.
Synth Make(uint32_t log2, uint32_t endian, bool tiled, const std::vector<std::array<uint32_t, 4>>& levels)
{
    Synth s;
    TextureDescription& d = s.d;
    d.ok = true;
    d.fetch.tiled = tiled;
    d.fetch.endian = endian;
    d.log2BytesPerBlock = log2;
    d.dimension = 1, d.layers = 1, d.depth = 1;
    uint32_t cursor = kGuestStart;
    size_t staging = 0;
    for (const auto& [w, h, x, y] : levels)
    {
        MipLevel l;
        l.address = cursor;
        l.widthBlocks = w, l.heightBlocks = h, l.x = x, l.y = y, l.widthTexels = w, l.heightTexels = h;
        l.pitchBlocks = AlignUp(x + w, 32);
        l.rowPitchBytes = AlignUp((x + w) << log2, 256);
        l.extentBytes = tiled ? TiledAddressUpperBound2D(x + w, y + h, l.pitchBlocks, log2)
                              : size_t(l.rowPitchBytes) * y + (size_t(x) << log2) + LinearExtent(w, h, l.rowPitchBytes, log2);
        cursor += AlignUp(uint32_t(l.extentBytes), 4096);
        d.levels.push_back(l);
        staging = (staging + 15) & ~size_t(15);
        s.plan.levelOffsets.push_back(staging);
        s.plan.levelBytes.push_back((size_t(w) * h) << log2);
        staging += s.plan.levelBytes.back();
    }
    s.plan.stagingBytes = staging;
    s.end = cursor;
    d.levelCount = uint32_t(levels.size());
    d.width = d.levels[0].widthTexels, d.height = d.levels[0].heightTexels;
    d.base = {d.levels[0].address, uint32_t(d.levels[0].extentBytes)};
    if (levels.size() > 1)
        d.mips = {d.levels[1].address, cursor - d.levels[1].address};
    return s;
}

Guest RandomGuest(uint32_t end, uint32_t seed)
{
    Guest g{std::vector<uint8_t>(end + 4096)};
    uint32_t v = seed;
    for (uint8_t& b : g.bytes)
        v = v * 1664525u + 1013904223u, b = uint8_t(v >> 24);
    return g;
}

constexpr uint8_t kSentinel = 0xCD;

// What the GPU would leave in the ring: every planned level dispatched invocation by invocation (every invocation of every
// workgroup, the ones past `dwords` returning as the shader's main does), each write checked against its descriptor range, each
// read against the guest binding's. `swapOverride` (CONTROL only) replaces the planned swap mask.
struct RunResult
{
    bool planned = false, bound = false, readsInside = true, writesInside = true, spillZero = true;
    uint64_t writeEnd = 0;  // one past the last ring byte written
};
RunResult RunModel(const Synth& s, const Guest& g, uint64_t ringOffset, uint64_t storageAlign, std::vector<uint8_t>& ring, uint32_t swapOverride = ~0u)
{
    RunResult r;
    std::vector<GpuUntileLevel> levels;
    std::string why;
    r.planned = PlanGpuUntile(s.d, s.plan, 65535, levels, why);
    if (!r.planned)
        return r;
    r.bound = true;
    for (GpuUntileLevel& level : levels)
    {
        GpuUntileBinding b;
        if (!GpuUntileBind(level, ringOffset + level.levelOffset, g.bytes.size(), ring.size(), storageAlign, 1ull << 30, b))
            return r.bound = false, r;
        const GpuUntilePush& p = level.push;
        model::binding = g.bytes.data() + b.guestOffset;
        model::bindingDwords = b.guestRange / 4;
        model::outside = false;
        const uint32_t swap = swapOverride != ~0u ? swapOverride : p.swapMask;
        for (uint32_t w = 0; w < level.groups * kUntileGpuGroupSize; w++)
        {
            if (w >= p.dwords)
                continue;
            const uint32_t v = model::XeUntileOutputDword(w, p.levelBytes, p.srcByte, p.widthBlocks, p.x0, p.y0, p.pitch, p.log2Bytes, p.tiled, swap);
            for (uint32_t k = 0; k < 4; k++)
                if (w * 4 + k >= p.levelBytes && ((v >> (8 * k)) & 0xFF) != 0)
                    r.spillZero = false;
            const uint64_t at = b.ringOffset + 4 * (uint64_t(p.dstWord) + w);
            if (at + 4 > b.ringOffset + b.ringRange)
            {
                r.writesInside = false;
                continue;
            }
            std::memcpy(ring.data() + at, &v, 4);
            r.writeEnd = std::max(r.writeEnd, at + 4);
        }
        r.readsInside = r.readsInside && !model::outside;
    }
    return r;
}

// The staging FillStaging writes (the expected bytes), at `ringOffset` of a sentinel-filled ring.
std::vector<uint8_t> Expected(const Synth& s, const Guest& g, uint64_t ringOffset, size_t ringBytes, bool& filled)
{
    std::vector<uint8_t> ring(ringBytes, kSentinel);
    std::string error;
    filled = FillStaging(s.d, s.plan, GuestOf, const_cast<Guest*>(&g), ring.data() + ringOffset, error);
    return ring;
}

// Every level's bytes equal; nothing written before the staging or past its dword-rounded end.
bool SameStaging(const Synth& s, const std::vector<uint8_t>& expected, const std::vector<uint8_t>& ring, uint64_t ringOffset)
{
    for (size_t i = 0; i < s.plan.levelOffsets.size(); i++)
        if (std::memcmp(expected.data() + ringOffset + s.plan.levelOffsets[i], ring.data() + ringOffset + s.plan.levelOffsets[i], s.plan.levelBytes[i]) != 0)
            return false;
    for (uint64_t at = 0; at < ringOffset; at++)
        if (ring[at] != kSentinel)
            return false;
    for (uint64_t at = ringOffset + GpuUntileStagingBytes(s.plan.stagingBytes); at < ring.size(); at++)
        if (ring[at] != kSentinel)
            return false;
    return true;
}

void TestTiledOffset()
{
    bool xenia = true, cpu = true;
    for (uint32_t log2 = 0; log2 <= 4; log2++)
        for (uint32_t pitch : {32u, 64u, 96u, 128u, 160u, 40u})
            for (uint32_t y = 0; y < 128; y++)
                for (uint32_t x = 0; x < 160; x++)
                {
                    const uint32_t v = model::XeUntileTiledOffset2D(x, y, pitch, log2);
                    xenia = xenia && v == XeniaTiledOffset2D(x, y, pitch, log2);
                    cpu = cpu && v == TiledOffset2D(x, y, pitch, log2);
                }
    Expect(xenia, "the shared tiled offset equals Xenia's GetTiledOffset2D (x < 160, y < 128, six pitches, 1-16 bytes a block)");
    Expect(cpu, "the shared tiled offset equals texture_untile.h TiledOffset2D");
    // Hand (texture_util.cc:443-458, log2 2, pitch 32): block (1, 0): micro 4, offset 4 -> 4; (8, 0): micro 0, bank term ((0 + 1) & 3)
    // << 6 = 64; (0, 1): micro 0, offset (1 & 1) << 4 = 16 -> 16; (16, 0): ((0 + 2) & 3) << 6 = 128.
    Expect(model::XeUntileTiledOffset2D(1, 0, 32, 2) == 4 && model::XeUntileTiledOffset2D(8, 0, 32, 2) == 64 && model::XeUntileTiledOffset2D(0, 1, 32, 2) == 16 &&
               model::XeUntileTiledOffset2D(16, 0, 32, 2) == 128,
           "hand: tiled blocks (1,0) (8,0) (0,1) (16,0) of 4 bytes at 4, 64, 16, 128");
}

void TestSourceBytes()
{
    using model::XeUntileSourceByte;
    // Linear 4-byte 8in32 (mask 3), 4 blocks a row: byte 0 of block 0 reads byte 3; byte 5 (block 1, byte 1) reads 4 + (1 ^ 3) = 6.
    Expect(XeUntileSourceByte(0, 4, 0, 0, 16, 2, 0, 3) == 3 && XeUntileSourceByte(5, 4, 0, 0, 16, 2, 0, 3) == 6, "hand: linear 8in32 bytes 0 and 5 read 3 and 6");
    // Linear 2-byte 8in16 (mask 1): bytes 0, 1, 3 read 1, 0, 2.
    Expect(XeUntileSourceByte(0, 4, 0, 0, 8, 1, 0, 1) == 1 && XeUntileSourceByte(1, 4, 0, 0, 8, 1, 0, 1) == 0 && XeUntileSourceByte(3, 4, 0, 0, 8, 1, 0, 1) == 2,
           "hand: linear 8in16 2-byte bytes 0, 1, 3 read 1, 0, 2");
    // Linear 1-byte, 3 blocks a row, rows 256 bytes apart: byte 4 is block (1, 1) at 256 + 1.
    Expect(XeUntileSourceByte(4, 3, 0, 0, 256, 0, 0, 0) == 257, "hand: linear 1-byte, 3-wide: byte 4 is block (1,1) at 257");
    // Tiled 4-byte, 16 blocks a row, pitch 32: byte 32 is block (8, 0) at 64; byte 64 is block (0, 1) at 16; with the origin (16, 0)
    // (a packed tail), byte 0 is block (16, 0) at 128.
    Expect(XeUntileSourceByte(32, 16, 0, 0, 32, 2, 1, 0) == 64 && XeUntileSourceByte(64, 16, 0, 0, 32, 2, 1, 0) == 16 &&
               XeUntileSourceByte(0, 16, 16, 0, 32, 2, 1, 0) == 128,
           "hand: tiled 4-byte bytes 32, 64 read 64, 16; at origin (16,0) byte 0 reads 128");
    Expect(UntileSwapMask(2, kEndianNone) == 0 && UntileSwapMask(2, kEndian8in16) == 1 && UntileSwapMask(3, kEndian8in32) == 3 &&
               UntileSwapMask(4, kEndian16in32) == 2 && UntileSwapMask(1, kEndian8in16) == 1 && UntileSwapMask(0, kEndianNone) == 0,
           "hand: swap masks (SwapBlockEndian's dword permutations as byte xors): 0, 1, 3, 2; 2-byte 8in16 1; 1-byte 0");
}

// The exhaustive comparison: every block size and endian it accepts, tiled and linear, four level sets, three placements.
void TestAgainstFillStaging()
{
    const std::vector<std::vector<std::array<uint32_t, 4>>> sets = {
        {{3, 5, 0, 0}},                               // odd size, one level
        {{37, 19, 0, 0}, {19, 10, 0, 0}, {10, 5, 0, 0}},  // a mip chain, odd sizes
        {{4, 4, 16, 0}, {2, 2, 0, 16}, {1, 1, 4, 4}},     // packed-mip origins inside a 32-block storage
        {{70, 40, 0, 0}},                             // past one 32x32 macro tile both ways
    };
    const char* sizes[5] = {"1-byte", "2-byte", "4-byte", "8-byte", "16-byte"};
    for (uint32_t log2 = 0; log2 <= 4; log2++)
        for (uint32_t endian = 0; endian <= 3; endian++)
        {
            if (!EndianSupported(log2, endian))
                continue;
            for (bool tiled : {true, false})
            {
                bool same = true, planned = true, inside = true, spill = true;
                // Ring placements (16-byte aligned, as StagingAlignment) and minStorageBufferOffsetAlignment values, one with a residual.
                const std::pair<uint64_t, uint64_t> placements[3] = {{256, 64}, {4096 + 48, 256}, {1024 + 16, 16}};
                for (size_t set = 0; set < sets.size(); set++)
                    for (const auto& [ringOffset, align] : placements)
                    {
                        const Synth s = Make(log2, endian, tiled, sets[set]);
                        const Guest g = RandomGuest(s.end, 1 + log2 * 31 + endian * 7 + uint32_t(set) * 3 + (tiled ? 1 : 0));
                        const size_t ringBytes = ringOffset + s.plan.stagingBytes + 512;
                        bool filled = false;
                        const std::vector<uint8_t> expected = Expected(s, g, ringOffset, ringBytes, filled);
                        std::vector<uint8_t> ring(ringBytes, kSentinel);
                        const RunResult r = RunModel(s, g, ringOffset, align, ring);
                        planned = planned && filled && r.planned && r.bound;
                        inside = inside && r.readsInside && r.writesInside;
                        spill = spill && r.spillZero;
                        same = same && SameStaging(s, expected, ring, ringOffset);
                    }
                const std::string what = std::string(sizes[log2]) + " blocks, endian " + std::to_string(endian) + (tiled ? ", tiled" : ", linear");
                Expect(planned, "planned and bound: " + what);
                Expect(same, "model == FillStaging: " + what);
                Expect(inside, "reads stay inside the guest binding, writes inside the ring binding: " + what);
                Expect(spill, "bytes past a level in its last dword are 0: " + what);
            }
        }
    // CONTROL: the comparison sees a wrong endian (4-byte 8in32 planned, run with 8in16's mask).
    const Synth s = Make(2, kEndian8in32, true, {{37, 19, 0, 0}});
    const Guest g = RandomGuest(s.end, 99);
    bool filled = false;
    const std::vector<uint8_t> expected = Expected(s, g, 256, 256 + s.plan.stagingBytes + 512, filled);
    std::vector<uint8_t> ring(expected.size(), kSentinel);
    RunModel(s, g, 256, 64, ring, 1);
    Expect(filled && !SameStaging(s, expected, ring, 256), "CONTROL: a wrong swap mask differs from FillStaging");
}

void TestReservation()
{
    Expect(GpuUntileStagingBytes(0) == 0 && GpuUntileStagingBytes(5) == 8 && GpuUntileStagingBytes(8) == 8 && GpuUntileStagingBytes(4097) == 4100,
           "hand: the reservation rounds the staging up to a dword (0, 5 -> 8, 8, 4097 -> 4100)");
    // A k_8 chain ending in a 3x1 level: 16 + 3 = 19 staging bytes after the 4x4 level; the last dword reaches byte 20.
    const Synth s = Make(0, kEndianNone, true, {{4, 4, 0, 0}, {3, 1, 0, 0}});
    const Guest g = RandomGuest(s.end, 5);
    std::vector<uint8_t> ring(256 + 64, kSentinel);
    const RunResult r = RunModel(s, g, 256, 64, ring);
    Expect(s.plan.stagingBytes == 19 && r.writeEnd == 256 + 20 && r.writeEnd <= 256 + GpuUntileStagingBytes(s.plan.stagingBytes),
           "the ring reservation covers the last level's last dword (19 staging bytes, written to 20)");
}

void TestBind()
{
    GpuUntileLevel l;
    l.address = 0x00101040u, l.extentBytes = 0x100, l.push.dwords = 64;
    GpuUntileBinding b;
    // Hand: align 256: the guest binding at 101000 with 0x40 before the level, 0x140 bytes; the ring level at 0x1230 -> binding at 0x1200,
    // dword 0x30 / 4 = 12, range 0x30 + 64 x 4 = 0x130.
    const bool ok = GpuUntileBind(l, 0x1230, 0x200000, 0x10000, 256, 1u << 27, b);
    Expect(ok && b.guestOffset == 0x101000 && l.push.srcByte == 0x40 && b.guestRange == 0x140 && b.ringOffset == 0x1200 && l.push.dstWord == 12 &&
               b.ringRange == 0x130,
           "hand: bindings at 101000 + 0x140 (src byte 0x40) and ring 0x1200 + 0x130 (dst dword 12)");
    GpuUntileLevel m = l;
    Expect(!GpuUntileBind(m, 0x1230, 0x200000, 0x10000, 48, 1u << 27, b), "CONTROL: a storage alignment that is not a power of two is refused");
    Expect(!GpuUntileBind(m, 0x1232, 0x200000, 0x10000, 256, 1u << 27, b), "CONTROL: a staging residual that is not a dword multiple is refused");
    Expect(!GpuUntileBind(m, 0x1230, 0x200000, 0x1300, 256, 1u << 27, b), "CONTROL: a ring range past the ring is refused");
    Expect(!GpuUntileBind(m, 0x1230, 0x101100, 0x10000, 256, 1u << 27, b), "CONTROL: a guest range past the guest buffer is refused");
    Expect(!GpuUntileBind(m, 0x1230, 0x200000, 0x10000, 256, 0x100, b), "CONTROL: a range above maxStorageBufferRange is refused");
}

void TestPlan()
{
    std::vector<GpuUntileLevel> levels;
    std::string why;
    // Hand: 40x24 blocks of 4 bytes = 3840 bytes, 960 dwords, 960 / 128 = 7.5 -> 8 groups; pitch align(40, 32) = 64; 8in32 mask 3.
    const Synth s = Make(2, kEndian8in32, true, {{40, 24, 0, 0}});
    Expect(PlanGpuUntile(s.d, s.plan, 8, levels, why) && levels.size() == 1 && levels[0].push.levelBytes == 3840 && levels[0].push.dwords == 960 &&
               levels[0].groups == 8 && levels[0].push.pitch == 64 && levels[0].push.swapMask == 3 && levels[0].push.tiled == 1,
           "hand: 40x24 4-byte 8in32 tiled: 3840 bytes, 960 dwords, 8 groups, pitch 64, mask 3");
    Expect(!PlanGpuUntile(s.d, s.plan, 7, levels, why), "CONTROL: more workgroups than maxComputeWorkGroupCount[0] stay on the CPU");
    const auto refused = [&](Synth t, const std::string& name, bool fillRefuses) {
        std::vector<GpuUntileLevel> l;
        std::string w;
        bool filled = false;
        if (fillRefuses)  // the same refusal on the CPU path (only where FillStaging handles the layout)
        {
            const Guest g = RandomGuest(t.end, 3);
            std::vector<uint8_t> out(t.plan.stagingBytes + 64);
            std::string error;
            filled = FillStaging(t.d, t.plan, GuestOf, const_cast<Guest*>(&g), out.data(), error);
        }
        Expect(!PlanGpuUntile(t.d, t.plan, 65535, l, w) && l.empty() && !filled, name);
    };
    Synth t = s;
    t.d.dimension = 2;
    refused(t, "CONTROL: a 3D texture stays on the CPU", false);
    t = s, t.d.layers = 6;
    refused(t, "CONTROL: a cube stays on the CPU", false);
    t = s, t.d.host.depth20e4 = true;
    refused(t, "CONTROL: a converted format (20e4 depth) stays on the CPU", false);
    t = s, t.plan.levelBytes.push_back(4);
    refused(t, "CONTROL: a plan that does not match the levels stays on the CPU", true);
    t = s, t.d.levels[0].extentBytes -= 1;
    refused(t, "CONTROL: a level one byte short of its tiled extent is refused (as FillStaging)", true);
    t = s, t.d.levels[0].pitchBlocks = 48;
    refused(t, "CONTROL: a tiled pitch that is not a multiple of 32 is refused (as FillStaging)", true);
    Synth lin = Make(2, kEndianNone, false, {{40, 24, 0, 0}});
    lin.d.levels[0].rowPitchBytes = 156;
    refused(lin, "CONTROL: a linear row pitch narrower than the level is refused (as FillStaging)", true);
    t = s, t.plan.levelOffsets[0] = 2;
    refused(t, "CONTROL: a staging offset that is not a dword multiple stays on the CPU", false);
    t = s, t.d.base.size = 16;
    refused(t, "CONTROL: a level outside the texture's guarded ranges stays on the CPU", false);
    t = Make(0, kEndian8in16, true, {{8, 8, 0, 0}});
    refused(t, "CONTROL: an endian 1-byte blocks have no rule for is refused (as FillStaging)", true);
}

// A guard that records its calls; `base` / `mips` decide each range.
struct GuardLog
{
    int calls = 0;
    uint64_t serial = 0;
    uint32_t basePhysical = 0;
    bool base = true, mips = true;
};
bool LoggedGuard(void* context, uint32_t physical, uint32_t, uint64_t serial)
{
    GuardLog& g = *static_cast<GuardLog*>(context);
    g.calls++, g.serial = serial;
    return physical == g.basePhysical ? g.base : g.mips;
}

void TestChoose()
{
    const Synth s = Make(2, kEndianNone, true, {{32, 32, 0, 0}, {16, 16, 0, 0}});
    GuardLog g;
    g.basePhysical = s.d.base.physical;
    GpuLoadChoice c = ChooseTextureLoad(false, false, true, s.d, 7, LoggedGuard, &g);
    Expect(!c.gpu && g.calls == 0, "CONTROL: off: the CPU path, no guard taken");
    c = ChooseTextureLoad(true, true, true, s.d, 7, LoggedGuard, &g);
    Expect(!c.gpu && g.calls == 0, "CONTROL: a pending resolve write-back stays on the CPU (no guard taken)");
    c = ChooseTextureLoad(true, false, false, s.d, 7, LoggedGuard, &g);
    Expect(!c.gpu && g.calls == 0, "CONTROL: an unplanned layout stays on the CPU (no guard taken)");
    g.base = false;
    c = ChooseTextureLoad(true, false, true, s.d, 7, LoggedGuard, &g);
    Expect(!c.gpu, "CONTROL: an unguarded level 0 stays on the CPU");
    g.base = true, g.mips = false, g.calls = 0;
    c = ChooseTextureLoad(true, false, true, s.d, 7, LoggedGuard, &g);
    Expect(!c.gpu && g.calls == 2, "CONTROL: an unguarded mip range stays on the CPU");
    g.mips = true, g.calls = 0;
    c = ChooseTextureLoad(true, false, true, s.d, 9, LoggedGuard, &g);
    Expect(c.gpu && g.calls == 2 && g.serial == 9, "guarded level 0 and mips with the reading part's serial: the GPU path");
    const Synth one = Make(2, kEndianNone, true, {{32, 32, 0, 0}});
    g.calls = 0, g.basePhysical = one.d.base.physical;
    c = ChooseTextureLoad(true, false, true, one.d, 9, LoggedGuard, &g);
    Expect(c.gpu && g.calls == 1, "one level: only level 0 is guarded");
    Expect(!ChooseTextureLoad(true, false, true, one.d, 9, nullptr, nullptr).gpu, "CONTROL: no guard function stays on the CPU");
}

// The texture side of an overlapping read and write, on a CPU model of the guard: a GPU-untiled load is
// recorded for part 7, a guest store hits its range before the part runs; the guard holds it, the load (executed at completion)
// reads the old bytes, the store lands after, the write watch sees it, and the reuse hash then reloads.
struct HeldGuard
{
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    uint64_t serial = 0;
    bool complete = false, holds = true;
    struct Held
    {
        uint32_t physical;
        uint8_t value;
    };
    std::vector<Held> held;
};
bool HeldGuardFn(void* context, uint32_t physical, uint32_t bytes, uint64_t serial)
{
    HeldGuard& g = *static_cast<HeldGuard*>(context);
    g.ranges.push_back({physical, bytes}), g.serial = serial;
    return true;
}
void Store(HeldGuard& guard, Guest& guest, WriteWatch& watch, uint32_t physical, uint8_t value)
{
    bool guarded = false;
    for (const auto& [p, n] : guard.ranges)
        guarded = guarded || (physical >= p && physical < p + n);
    if (guarded && guard.holds && !guard.complete)
        return guard.held.push_back({physical, value});
    guest.bytes[physical] = value;
    watch.MarkWritten(physical, 1);  // the write watch's record of the store (its fault path; MarkWritten in the model)
}
uint64_t LevelHash(const Synth& s, const Guest& g)
{
    return ContentHash(g.bytes.data() + s.d.base.physical, s.d.base.size, s.d.base.physical);
}

void TestOverlap()
{
    const Synth s = Make(2, kEndian8in32, true, {{40, 24, 0, 0}});
    const uint32_t hit = s.d.base.physical + 100;
    for (bool holds : {true, false})
    {
        UnprotectedWatchBackend backend;
        WriteWatch watch(backend);
        Guest guest = RandomGuest(s.end, 11);
        const uint64_t ringOffset = 256;
        bool filled = false;
        const std::vector<uint8_t> old = Expected(s, guest, ringOffset, ringOffset + s.plan.stagingBytes + 64, filled);
        const uint64_t hashAtBind = LevelHash(s, guest), sequenceAtBind = watch.Current();
        const uint8_t before = guest.bytes[hit];
        HeldGuard guard;
        guard.holds = holds;
        const GpuLoadChoice c = ChooseTextureLoad(true, false, true, s.d, 7, HeldGuardFn, &guard);
        Store(guard, guest, watch, hit, uint8_t(before ^ 0xFF));  // before the part is submitted
        // The part runs: the recorded load reads guest memory now; then the held store lands.
        std::vector<uint8_t> ring(old.size(), kSentinel);
        RunModel(s, guest, ringOffset, 64, ring);
        guard.complete = true;
        for (const HeldGuard::Held& h : guard.held)
            guest.bytes[h.physical] = h.value, watch.MarkWritten(h.physical, 1);
        if (holds)
        {
            Expect(c.gpu && guard.serial == 7 && guard.held.size() == 1, "overlap: the load is guarded for part 7 and the store is held");
            Expect(filled && SameStaging(s, old, ring, ringOffset), "overlap: the GPU load reads the bytes of the bind, not the held store");
            Expect(guest.bytes[hit] == uint8_t(before ^ 0xFF), "overlap: the held store lands after the part completes");
            Expect(watch.WriteSequence(s.d.base.physical, s.d.base.size) > sequenceAtBind, "overlap: the write watch records the landed store");
            const uint64_t full = s.plan.stagingBytes + 16, after = LevelHash(s, guest);
            Expect(after != hashAtBind && ReservedStagingBytes(true, true, true, after, hashAtBind, full) == full,
                   "overlap: the next bind's hash differs, so the texture is reloaded");
            bool refilled = false;
            const std::vector<uint8_t> fresh = Expected(s, guest, ringOffset, old.size(), refilled);
            std::vector<uint8_t> again(old.size(), kSentinel);
            RunModel(s, guest, ringOffset, 64, again);
            Expect(refilled && SameStaging(s, fresh, again, ringOffset) && !SameStaging(s, old, again, ringOffset), "overlap: the reload carries the store");
        }
        else
            Expect(!SameStaging(s, old, ring, ringOffset), "CONTROL: without the hold the GPU load reads the later store (the race the guard closes)");
    }
    // The fallback when the guard refuses: FillStaging's snapshot at the bind keeps the old bytes; the store lands at once and the
    // watch and the hash see it.
    UnprotectedWatchBackend backend;
    WriteWatch watch(backend);
    Guest guest = RandomGuest(s.end, 12);
    GuardLog refuse;
    refuse.base = refuse.mips = false, refuse.basePhysical = s.d.base.physical;
    const GpuLoadChoice c = ChooseTextureLoad(true, false, true, s.d, 7, LoggedGuard, &refuse);
    bool filled = false;
    const std::vector<uint8_t> snapshot = Expected(s, guest, 256, 256 + s.plan.stagingBytes + 64, filled);
    const uint64_t hashAtBind = LevelHash(s, guest), sequenceAtBind = watch.Current();
    HeldGuard none;  // nothing guarded
    Store(none, guest, watch, hit, uint8_t(guest.bytes[hit] ^ 0xFF));
    bool refilled = false;
    const std::vector<uint8_t> now = Expected(s, guest, 256, snapshot.size(), refilled);
    Expect(!c.gpu && filled && refilled && !SameStaging(s, now, snapshot, 256), "fallback: a refused guard stages on the CPU, a snapshot of the bind");
    Expect(watch.WriteSequence(s.d.base.physical, s.d.base.size) > sequenceAtBind && LevelHash(s, guest) != hashAtBind,
           "fallback: the store is seen by the watch and the hash (the next bind reloads)");
}

void EmitGlsl(const char* dir)
{
    const std::string glsl = UntileGpuGlsl();
    Expect(glsl.find("XeUntileOutputDword(uint w") != std::string::npos && glsl.find("layout(local_size_x = 128)") != std::string::npos &&
               glsl.find("push_constant") != std::string::npos,
           "the emitted shader holds the shared body, 128 invocations a group, the push constants");
    if (dir == nullptr)
        return;
    std::ofstream f(std::string(dir) + "/texture_untile.comp");
    f << glsl;
    Expect(bool(f), "the emitted shader is written for glslangValidator");
}

}  // namespace

int main(int argc, char** argv)
{
    TestTiledOffset();
    TestSourceBytes();
    TestAgainstFillStaging();
    TestReservation();
    TestBind();
    TestPlan();
    TestChoose();
    TestOverlap();
    EmitGlsl(argc > 1 ? argv[1] : nullptr);
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures ? 1 : 0;
}
