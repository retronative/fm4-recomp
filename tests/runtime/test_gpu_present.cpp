// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the GPU present's CPU side (FM4_GPU_PRESENT; runtime/video/gpu_present.h). No Vulkan device. The shader's function body
// (FM4_GPU_PRESENT_BODY) is expanded here as C++ (namespace model) and compared with today's CPU path, which it replaces:
//   - the layout: the front-buffer resolve's words written back by ResolveWordsToGuest (edram_vk_plan.cpp, FM4_RESOLVE_GPU_WORDS) from
//     FM4's traced front-buffer resolve (the local capture trace12 (not distributed), as tests/runtime/test_edram_vk.cpp builds it), then Detile32
//     (front_buffer.h, reviewed against Xenia's GetTiledOffset2D), give texel i == GpuSwap32(words[i], endian) for every texel of
//     1280x720 at pitch 1280 and 1312 and every endian;
//   - the word: XePresentWord == SwapRedBlue1010102 (when asked) + ApplyPwlToFrame (gamma_ramp.cpp) of that texel, for every 10-bit
//     value of each channel and 2^20 pseudo-random words, every endian, both swizzles, three ramps (Xenia's default, Direct3D 9's
//     linear ramp, a title-like one);
//   - the tables: BuildPwlLut == ApplyPwl10 per entry, plus hand-computed PWL values (Xenia apply_gamma_pwl.xesli:12-25: the
//     nearest integer to (8 * base + (v & 7) * delta) / 512, a tie rounding up, clamped to 1023);
//   - the swap: XePresentSwap == edram.h GpuSwap32 and hand vectors (Xenia xenos.h:1070-1086);
//   - FrontResolveBand's accepted bands (whole, FM4's three bands) and each refusal, FrontWordsTracker's states (hand-built sequences).
// The emitted GLSL is written to the directory given as argv[1] for the runner's glslangValidator / spirv-val. "CONTROL" names a
// negative control.
//
//   tests/run_gpu_present_tests.sh
#include "gpu_present.h"
#include "edram.h"
#include "edram_vk_plan.h"
#include "front_buffer.h"
#include "kernel/gamma_ramp.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace runtime::video;
namespace gamma = runtime::gpu::gamma;

namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const std::string& name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name.c_str());
    failures += !ok;
    controls += name.rfind("CONTROL", 0) == 0;
}
}  // namespace

// The shader's body as C++: XePresentLut reads the flattened tables (binding 1).
namespace model {
using uint = uint32_t;
const uint32_t* lut = nullptr;
uint XePresentLut(uint i) { return lut[i]; }
FM4_GPU_PRESENT_BODY
}  // namespace model

namespace {

uint32_t Rand(uint64_t& s)  // splitmix64, the low 32 bits
{
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return uint32_t(z ^ (z >> 31));
}

// The three ramps: Xenia's default (gamma_ramp.h DefaultRamp), Direct3D 9's linear PWL (X register_table.inc:341-371: entry i base
// i * 0x200, delta 0x200; entry 7F base FFC0, delta 0), and a title-like ramp (pseudo-random, bits 0-5 zero as the hardware's).
std::vector<std::pair<std::string, gamma::Ramp>> Ramps()
{
    std::vector<std::pair<std::string, gamma::Ramp>> r;
    r.push_back({"default", gamma::DefaultRamp()});
    gamma::Ramp linear;
    for (uint32_t i = 0; i < 128; i++)
        for (uint32_t c = 0; c < 3; c++)
            linear.pwl[i][c] = {uint16_t(i < 0x7F ? i * 0x200 : 0xFFC0), uint16_t(i < 0x7F ? 0x200 : 0)};
    r.push_back({"D3D linear", linear});
    gamma::Ramp title;
    uint64_t s = 42;
    for (uint32_t i = 0; i < 128; i++)
        for (uint32_t c = 0; c < 3; c++)
            title.pwl[i][c] = {uint16_t(Rand(s) & 0xFFC0u), uint16_t(Rand(s) & 0xFFC0u)};
    r.push_back({"title-like", title});
    return r;
}

// The flattened tables the shader reads (binding 1: component-major, 1024 words each).
std::vector<uint32_t> Flat(const gamma::Ramp& ramp)
{
    uint32_t lut[3][1024];
    gamma::BuildPwlLut(ramp.pwl, lut);
    return std::vector<uint32_t>(&lut[0][0], &lut[0][0] + kGpuPresentLutWords);
}

// Today's CPU path over detiled texels (renderer.cpp Present): the red/blue exchange, then ApplyPwlToFrame over the frame.
std::vector<uint32_t> CpuWords(std::vector<uint32_t> texels, bool swapRedBlue, const gamma::Ramp& ramp)
{
    if (swapRedBlue)
        for (uint32_t& p : texels)
            p = SwapRedBlue1010102(p);
    gamma::ApplyPwlToFrame(texels.data(), texels.size(), ramp.pwl);
    return texels;
}

void TestSwap()
{
    const uint32_t v = 0x11223344u;  // hand: 8in16 swaps the bytes of each halfword, 16in32 the halfwords, 8in32 both
    Expect(model::XePresentSwap(v, 0) == 0x11223344u && model::XePresentSwap(v, 1) == 0x22114433u && model::XePresentSwap(v, 2) == 0x44332211u &&
               model::XePresentSwap(v, 3) == 0x33441122u,
           "hand: swap of 11223344 for endians 0-3");
    uint64_t s = 7;
    for (uint32_t e = 0; e < 4; e++)
    {
        bool same = true;
        for (int k = 0; k < 100000; k++)
        {
            const uint32_t w = Rand(s);
            same = same && model::XePresentSwap(w, e) == edram::GpuSwap32(w, e);
        }
        Expect(same, "model swap == GpuSwap32: endian " + std::to_string(e));
    }
}

void TestTables()
{
    // Hand (gamma_ramp.h ApplyPwl10's rule): base 0x20, delta 0, v & 7 = 0: (256 + 256) / 512 = 1 (the tie 0.5 rounds up);
    // base 0x40, delta 0x40, v & 7 = 3: 8 * 64 + 3 * 64 = 704, (704 + 256) / 512 = 1 (1.375 rounds down);
    // base FFC0, delta FFC0, v & 7 = 7: 8 * 65472 + 7 * 65472 = 982080, (982080 + 256) / 512 = 1918, clamped to 1023;
    // base FFC0, delta 0: 523776 / 512 = 1023.0 exactly, 1023.
    gamma::Ramp hand;
    hand.pwl[0][0] = {0x20, 0};
    hand.pwl[1][1] = {0x40, 0x40};
    hand.pwl[2][2] = {0xFFC0, 0xFFC0};
    hand.pwl[127][0] = {0xFFC0, 0};
    uint32_t lut[3][1024];
    gamma::BuildPwlLut(hand.pwl, lut);
    Expect(lut[0][0] == 1, "hand: the tie rounds up (base 20, v 0 -> 1)");
    Expect(lut[1][8 + 3] == 1u << 10, "hand: 1.375 rounds down (green, base 40, delta 40, v 11 -> 1)");
    Expect(lut[2][16 + 7] == 1023u << 20, "hand: the clamp (blue, base FFC0, delta FFC0, v 23 -> 1023)");
    Expect(lut[0][1016] == 1023, "hand: base FFC0 at entry 7F (red, v 1016 -> 1023)");
    for (const auto& [name, ramp] : Ramps())
    {
        gamma::BuildPwlLut(ramp.pwl, lut);
        bool same = true;
        for (uint32_t c = 0; c < 3; c++)
            for (uint32_t v = 0; v < 1024; v++)
                same = same && lut[c][v] == gamma::ApplyPwl10(v, ramp.pwl[v >> 3][c]) << (10 * c);
        Expect(same, "BuildPwlLut == ApplyPwl10 for every channel value: " + name + " ramp");
    }
}

void TestWord()
{
    for (const auto& [name, ramp] : Ramps())
    {
        const std::vector<uint32_t> flat = Flat(ramp);
        model::lut = flat.data();
        for (uint32_t e = 0; e < 4; e++)
            for (uint32_t swap = 0; swap < 2; swap++)
            {
                uint64_t s = 1000 + e * 2 + swap;
                // Every 10-bit value of each channel (the others pseudo-random), then 2^20 pseudo-random texels. Each texel's resolve word
                // is GpuSwap32(texel, e): its guest bytes, read little-endian by Detile32, are the texel (GpuSwap32 is an involution).
                std::vector<uint32_t> texels;
                for (uint32_t c = 0; c < 4; c++)
                    for (uint32_t v = 0; v < 1024; v++)
                    {
                        const uint32_t shift = 10 * c, mask = c < 3 ? 0x3FFu : 3u;
                        texels.push_back((Rand(s) & ~(mask << shift)) | ((v & mask) << shift));
                    }
                for (int k = 0; k < (1 << 20); k++)
                    texels.push_back(Rand(s));
                const std::vector<uint32_t> want = CpuWords(texels, swap != 0, ramp);
                bool same = true;
                for (size_t k = 0; k < texels.size() && same; k++)
                    same = model::XePresentWord(edram::GpuSwap32(texels[k], e), e, swap) == want[k];
                Expect(same, "full word == the CPU path: " + name + " ramp, endian " + std::to_string(e) + (swap ? ", swapped" : ", straight"));
            }
    }
    // Hand: Xenia's default ramp is the identity on 10 bits apart from the lowest entries' truncation; 0x3FF in every channel stays
    // 0x3FF (entry 7F base FFC0: 1023), alpha becomes 3: C0000000 | 3FF << 20 | 3FF << 10 | 3FF = FFFFFFFF.
    const std::vector<uint32_t> flat = Flat(gamma::DefaultRamp());
    model::lut = flat.data();
    Expect(model::XePresentWord(0x3FFFFFFFu, 0, 0) == 0xFFFFFFFFu, "hand: white with alpha 0 -> FFFFFFFF (default ramp)");
    // CONTROL: forgetting the red/blue exchange changes a word whose red and blue differ.
    Expect(model::XePresentWord(0x000003FFu, 0, 1) != model::XePresentWord(0x000003FFu, 0, 0), "CONTROL: the swizzle exchanges red and blue");
}

edram::ResolveRegisters FrontBufferResolve()  // FM4's traced front-buffer resolve (tests/runtime/test_edram_vk.cpp Trace12Resolve)
{
    const auto guestFloat = [](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return __builtin_bswap32(bits);
    };
    edram::ResolveRegisters r;
    r.surfaceInfo = 0x14000500, r.colorInfo[0] = 0x00020000, r.depthInfo = 0x000102D0, r.modeControl = 6;
    r.copyControl = 0x00100300, r.copyDestBase = 0x1B441000, r.copyDestPitch = 0x02D00500, r.copyDestInfo = 0x01000380;
    r.colorClear = r.colorClearLo = 0;
    r.windowScissorBr = 0x02D00500, r.screenScissorBr = 0x20002000;
    r.fetch0 = 0x1160BDF3, r.fetch1 = 0x1000001A;
    const float v[6] = {0.0f, 0.0f, 1280.0f, 0.0f, 0.0f, 720.0f};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = guestFloat(v[i]);
    return r;
}

void TestLayout()
{
    const edram::ResolveInfo traced = edram::DecodeResolve(FrontBufferResolve());
    Expect(traced.ok && traced.x0 == 0 && traced.y0 == 0 && traced.x1 == 1280 && traced.y1 == 720 && traced.destBytesLog2 == 2 && traced.destFormat == 7 &&
               traced.destEndian == 0 && traced.destPitch == 1280,
           "the traced front-buffer resolve: 1280x720 at 0,0, format 7, endian 0, pitch 1280");
    const uint32_t w = 1280, h = 720;
    std::vector<uint32_t> words(size_t(w) * h), texels(size_t(w) * h);
    uint64_t s = 99;
    for (uint32_t& x : words)
        x = Rand(s);
    for (const uint32_t pitch : {1280u, 1312u})
        for (uint32_t e = 0; e < 4; e++)
        {
            edram::ResolveInfo info = traced;
            info.destPitch = pitch, info.destEndian = e;
            std::vector<uint8_t> guest(TiledSize32(pitch, h), 0xEE);
            std::string why;
            const bool stored = ResolveWordsToGuest(info, 2, words.data(), guest.data(), uint32_t(guest.size()), why);
            const bool detiled = Detile32(guest.data(), guest.size(), texels.data(), w, h, pitch);
            bool same = stored && detiled;
            for (size_t i = 0; i < texels.size() && same; i++)
                same = texels[i] == edram::GpuSwap32(words[i], e);
            Expect(same, "detiled texel i == GpuSwap32(word i) after the write-back: pitch " + std::to_string(pitch) + ", endian " + std::to_string(e) +
                             (stored ? "" : " (" + why + ")"));
        }
    // FM4's banded front buffer (resolves at 1B441000, 1B581000, 1B6C1000, 1280x256 / 1280x256 / 1280x208): each
    // band's words written back at its own base (+0x140000 = 8 rows of 40 tiles of 4 KB, +0x280000 = 16), each band at 0,0 of its
    // rectangle; FrontResolveBand places them at rows 0, 256, 512, and the composed words are the detiled texels row-major.
    {
        std::vector<uint8_t> guest(TiledSize32(1280, h), 0xEE);
        const uint32_t bandRows[3] = {256, 256, 208}, deltas[3] = {0, 0x140000, 0x280000};
        const FrontShape front{0x1B441000, w, h, 1280};
        bool stored = true, placed = true;
        uint32_t row = 0;
        for (uint32_t b = 0; b < 3; b++)
        {
            edram::ResolveInfo info = traced;
            info.y1 = bandRows[b];
            std::string why;
            stored = stored && ResolveWordsToGuest(info, 2, words.data() + size_t(row) * w, guest.data() + deltas[b], uint32_t(guest.size() - deltas[b]), why);
            ResidentResolve r;
            r.converted = true, r.textureBase = front.base + deltas[b], r.destFormat = 7, r.destPitch = 1280, r.width = w, r.height = bandRows[b];
            const FrontBand band = FrontResolveBand(r, front, &why);
            placed = placed && band.ok && band.row == row && band.rows == bandRows[b];
            row += bandRows[b];
        }
        bool same = stored && placed && Detile32(guest.data(), guest.size(), texels.data(), w, h, 1280);
        for (size_t i = 0; i < texels.size() && same; i++)
            same = texels[i] == words[i];
        Expect(same, "three bands: FrontResolveBand's rows 0, 256, 512 and the detiled texels == the composed words");
    }
    // CONTROL: the words written back at pitch 1312 and detiled at 1280 do not line up (the tracker refuses another pitch).
    edram::ResolveInfo info = traced;
    info.destPitch = 1312;
    std::vector<uint8_t> guest(TiledSize32(1312, h), 0xEE);
    std::string why;
    ResolveWordsToGuest(info, 2, words.data(), guest.data(), uint32_t(guest.size()), why);
    Detile32(guest.data(), guest.size(), texels.data(), w, h, 1280);
    bool same = true;
    for (size_t i = 0; i < texels.size() && same; i++)
        same = texels[i] == words[i];
    Expect(!same, "CONTROL: a pitch of 1312 detiled at 1280 differs");
}

ResidentResolve FrontResolve()  // the traced resolve as the EDRAM unit reports it (edram_vk.cpp Resolve, unit->last)
{
    ResidentResolve r;
    r.converted = true, r.extentStart = 0x1B441000, r.extentLength = uint32_t(TiledSize32(1280, 720)), r.textureBase = 0x1B441000;
    r.destFormat = 7, r.destPitch = 1280, r.destEndian = 0, r.x0 = 0, r.y0 = 0, r.width = 1280, r.height = 720, r.bytesLog2 = 2;
    return r;
}

// Band b (0-2) of FM4's banded front buffer: 1280x256 at +0, 1280x256 at +0x140000, 1280x208 at +0x280000.
ResidentResolve Band(uint32_t b)
{
    ResidentResolve r = FrontResolve();
    const uint32_t deltas[3] = {0, 0x140000, 0x280000};
    r.textureBase = r.extentStart = 0x1B441000 + deltas[b];
    r.extentLength = b < 2 ? 0x140000 : uint32_t(TiledSize32(1280, 720)) - 0x280000;
    r.height = b < 2 ? 256 : 208;
    return r;
}

void TestBand()
{
    const FrontShape f{0x1B441000, 1280, 720, 1280};
    std::string why;
    FrontBand band = FrontResolveBand(FrontResolve(), f, &why);
    Expect(band.ok && band.row == 0 && band.rows == 720, "band: the traced front-buffer resolve is rows 0-719");
    band = FrontResolveBand(Band(2), f, &why);  // hand: delta 0x280000 = tile 640 = row 16 of 40 tiles -> texel row 512
    Expect(band.ok && band.row == 512 && band.rows == 208, "band: +0x280000 is rows 512-719");
    ResidentResolve r = FrontResolve();
    r.destPitch = 1270;  // TilesPerRow(1270) == 40 == TilesPerRow(1280): the same layout
    Expect(FrontResolveBand(r, f, &why).ok, "band: pitch 1270 in the same 40 tiles");
    r = FrontResolve(), r.destFormat = 54;
    Expect(FrontResolveBand(r, f, &why).ok, "band: format 54");
    r = FrontResolve(), r.y0 = 32, r.height = 64;  // the rectangle's own y0 adds to its base's corner (resolve_resident.h)
    band = FrontResolveBand(r, f, &why);
    Expect(band.ok && band.row == 32 && band.rows == 64, "band: the rectangle's own y0 adds to the corner");
    const auto refused = [&](const char* name, auto change) {
        ResidentResolve x = FrontResolve();
        change(x);
        Expect(!FrontResolveBand(x, f, &why).ok, std::string("CONTROL: band: ") + name + " is refused");
    };
    refused("a resolve without GPU words", [](ResidentResolve& x) { x.converted = false; });
    refused("a 16 bpp destination", [](ResidentResolve& x) { x.bytesLog2 = 1; });
    refused("a depth destination (22)", [](ResidentResolve& x) { x.destFormat = 22; });
    refused("a k_32_FLOAT destination (36)", [](ResidentResolve& x) { x.destFormat = 36; });
    refused("a base inside a tile", [](ResidentResolve& x) { x.textureBase += 0x800; });
    refused("a base before the front buffer", [](ResidentResolve& x) { x.textureBase -= 0x1000; });
    refused("a base one tile right (column 32)", [](ResidentResolve& x) { x.textureBase += 0x1000, x.height = 32; });
    refused("a rectangle at x 32", [](ResidentResolve& x) { x.x0 = 32, x.width = 1248; });
    refused("a 1279-wide rectangle", [](ResidentResolve& x) { x.width = 1279; });
    refused("rows past the bottom", [](ResidentResolve& x) { x.y0 = 32; });
    refused("pitch 1312", [](ResidentResolve& x) { x.destPitch = 1312; });
}

void TestTracker()
{
    const FrontShape a{0x1B441000, 1280, 720, 1280}, b{0x1B800000, 1280, 720, 1280};
    const uint32_t bytes = FrontBytes(a);
    Expect(bytes == 40u * 32 * 736 * 4, "hand: the front buffer's range is 40 x 23 tiles of 4 KB (3,768,320 bytes)");
    const auto watchOk = [](uint32_t, uint32_t, uint64_t& q) { return q = 77, true; };
    const auto watchNo = [](uint32_t, uint32_t, uint64_t&) { return false; };
    const auto clean = [](uint32_t, uint32_t, uint64_t q) { return q == 77; };
    const auto dirty = [](uint32_t, uint32_t, uint64_t) { return false; };
    using State = FrontWordsTracker::State;
    const char* why = nullptr;
    FrontWordsTracker t;
    const size_t i = t.Note(a, 1);
    Expect(!t.Current(i, 0, clean, why), "CONTROL: a new front buffer has no words");
    FrontWordsTracker::Take take = t.OnResolve(FrontResolve(), 5);
    Expect(take.entry == int(i) && take.row == 0 && t.At(i).state == State::Recorded && t.At(i).lastSerial == 5 && t.At(i).covered == 720,
           "the whole-buffer resolve is taken (Recorded, its part's serial, every row)");
    Expect(t.Current(i, 5, dirty, why), "Recorded and the newest pending write: current");
    Expect(!t.Current(i, 6, dirty, why), "CONTROL: Recorded, a later pending write is not current");
    Expect(!t.Current(i, 0, dirty, why), "CONTROL: Recorded, nothing pending (retired without its watch) is not current");
    t.OnRetired(4, watchOk);
    Expect(t.At(i).state == State::Recorded, "retired before its serial: still Recorded");
    t.OnRetired(5, watchOk);
    Expect(t.At(i).state == State::Landed && t.At(i).sequence == 77, "retired through its serial: Landed, the watch's sequence");
    Expect(t.Current(i, 0, clean, why), "Landed and unwritten since: current");
    Expect(!t.Current(i, 0, dirty, why), "CONTROL: Landed and written since is not current");
    // A resolve just past the range keeps the words; one into its last 4 bytes drops them.
    ResidentResolve past = FrontResolve();
    past.extentStart = a.base + bytes, past.textureBase = past.extentStart;
    Expect(t.OnResolve(past, 9).entry == -1 && t.At(i).state == State::Landed, "a resolve just past the range keeps the words");
    ResidentResolve tail = past;
    tail.extentStart = a.base + bytes - 4, tail.extentLength = 4;
    Expect(t.OnResolve(tail, 9).entry == -1 && t.At(i).state == State::Empty, "CONTROL: a resolve into the range's last 4 bytes drops the words");
    Expect(t.OnResolve(FrontResolve(), 10).entry == int(i), "a new whole-buffer resolve is taken again");
    ResidentResolve partial = FrontResolve();
    partial.width = 64, partial.height = 64, partial.extentLength = 0x4000;
    Expect(t.OnResolve(partial, 10).entry == -1 && t.At(i).state == State::Empty, "CONTROL: a partial resolve over it drops the words");
    t.OnResolve(FrontResolve(), 11);
    t.OnRetired(11, watchNo);
    Expect(t.At(i).state == State::Empty, "CONTROL: an unwatchable range drops the words");
    // Bands: three in one part cover every row; two of three are not current; bands across two retires, or of two endians, drop.
    take = t.OnResolve(Band(0), 20);
    Expect(take.entry == int(i) && take.row == 0, "band 0 is taken at row 0");
    take = t.OnResolve(Band(1), 20);
    Expect(take.entry == int(i) && take.row == 256 && t.At(i).covered == 512, "band 1 is taken at row 256");
    Expect(!t.Current(i, 20, dirty, why), "CONTROL: two of three bands are not current");
    take = t.OnResolve(Band(2), 21);
    Expect(take.entry == int(i) && take.row == 512 && t.At(i).covered == 720 && t.Current(i, 21, dirty, why), "three bands over two parts cover every row: current");
    t.OnRetired(21, watchOk);
    Expect(t.At(i).state == State::Landed, "three bands retired together: Landed");
    t.OnResolve(Band(0), 30), t.OnResolve(Band(1), 30);
    Expect(t.At(i).state == State::Recorded && t.At(i).covered == 512, "a Landed set restarts with the next band");
    t.OnResolve(Band(2), 31);
    t.OnRetired(30, watchOk);  // the first two bands' part retires; the third's is still in flight
    t.OnRetired(31, watchOk);
    Expect(t.At(i).state != State::Landed, "CONTROL: bands retired in two retires drop the words");
    t.OnResolve(Band(0), 40);
    ResidentResolve swapped = Band(1);
    swapped.destEndian = 2;
    t.OnResolve(swapped, 40);
    t.OnResolve(Band(2), 40);
    Expect(!t.Current(i, 40, dirty, why), "CONTROL: a band of another endian drops the set");
    // Entries: a second base gets its own; the same base with another pitch is reset; a fourth replaces the least recently noted.
    const size_t j = t.Note(b, 2);
    Expect(j != i && t.Note(a, 3) == i, "two front buffers, two entries");
    t.OnResolve(FrontResolve(), 50);
    FrontShape wide = a;
    wide.pitch = 1312;
    Expect(t.Note(wide, 4) == i && t.At(i).state == State::Empty && t.At(i).shape.pitch == 1312, "the same base with another pitch is reset");
    const size_t k = t.Note({0x1C000000, 1280, 720, 1280}, 5);
    t.Note(wide, 6), t.Note({0x1C000000, 1280, 720, 1280}, 7);  // b (noted at 2) is now the least recently noted
    const size_t m = t.Note({0x1D000000, 1280, 720, 1280}, 8);
    Expect(k != i && k != j && m == j, "the least recently noted entry is replaced");
    Expect(t.At(j).shape.base == 0x1D000000u && t.At(j).state == State::Empty, "a replaced entry has no words");
}

void EmitGlsl(const char* dir)
{
    const std::string glsl = GpuPresentGlsl();
    Expect(glsl.find("XePresentWord(uint word, uint endian, uint swapRedBlue)") != std::string::npos, "the shader holds the shared body");
    if (dir == nullptr)
        return;
    std::ofstream f(std::string(dir) + "/gpu_present.comp");
    f << glsl;
    Expect(bool(f), "the emitted shader is written for glslangValidator");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSwap();
    TestTables();
    TestWord();
    TestLayout();
    TestBand();
    TestTracker();
    EmitGlsl(argc > 1 ? argv[1] : nullptr);
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures ? 1 : 0;
}
