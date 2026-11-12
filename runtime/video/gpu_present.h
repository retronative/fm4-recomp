// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, opt-in FM4_GPU_PRESENT=1): the GPU present's
// CPU side, without a Vulkan header so tests/runtime/test_gpu_present.cpp builds without one. renderer.cpp records the work.
//
// Today's swap (renderer.cpp Present) detiles the front buffer from guest memory on the CPU (front_buffer.h Detile32), exchanges red
// and blue for a ZYXW swizzle, applies the display gamma ramp (kernel/gamma_ramp.h ApplyPwlToFrame), and uploads the pixels into the
// display image. FM4's front buffer is the output of converting resolves whose write-back, with FM4_RESOLVE_GPU_WORDS=1, IS the
// resolves' GPU words (edram_vk_plan.cpp ResolveWordsToGuest: word w of rectangle pixel (x, y) stored as GpuSwap32(w, destEndian) at
// the tiled offset of its texel). So when those resolves are the last writers of the front buffer's bytes, the detiled texel (x, y) is
// GpuSwap32(words[y * width + x], destEndian): row-major, no detile. The GPU present copies those words at each resolve (the EDRAM
// unit's resident image, EdramHooks::lastResolve) into a buffer of its own (whole, or one band of rows per resolve) and at the swap
// runs one compute pass, words -> display words; the display image then takes them exactly as it takes the CPU's staging today.
//
// After ReCarbon (GPL-3.0-or-later, read-only) runtime/gpu/command_processor.cpp:503-517 and runtime/gpu/renderer.cpp:9390-9474
// (PresentFrontBuffer: a compute pass from the GPU's copy of the front buffer into a front image, submitted without waiting, the
// presenter's blit later in queue order; the CPU path only for frame dumps), and SpeedBreaker 312ce69 (GPL-3.0-or-later, read-only)
// runtime/gpu/command_processor.cpp:523-535, runtime/gpu/renderer.cpp:11029-11143 (the same), runtime/video/presenter.cpp:1850-1875
// (the display gamma as a table built on the CPU and read by the GPU pass). FM4 Recomp changes: the source is the front-buffer
// resolves' words, not shared guest memory (FM4's write-back is CPU-side: guest memory has the bytes only after a retire); the output
// is the packed display word (integer table lookups, no float), copied into the existing display image, so the presenter, the
// hand-over and the captures are unchanged; validity is tracked per front buffer (FrontWordsTracker) and anything else falls back
// to the CPU path, named.
#pragma once
#include "front_buffer.h"
#include "resolve_resident.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

// The shared body: valid GLSL 450 and valid C++ once `uint` is uint32_t and XePresentLut(uint i) (BuildPwlLut's tables, flattened
// component-major: index 1024 * c + v) is declared before it. Expanded as code by the test's model; stringified into the shader.
//  XePresentSwap: edram.h GpuSwap32 (Xenia xenos.h:1070-1086 GpuSwap): 1 8in16, 2 8in32 (8in16 then 16in32), 3 16in32.
//  XePresentWord: the word the CPU path leaves in the display image for this resolve word: the guest word (the swap), front_buffer.h
//    SwapRedBlue1010102 for a ZYXW / ZYX1 swizzle, then gamma_ramp.cpp ApplyPwlToFrame's expression (alpha 3, the three lookups).
#define FM4_GPU_PRESENT_BODY                                                                                                         \
    uint XePresentSwap(uint v, uint endian)                                                                                          \
    {                                                                                                                                \
        uint e = endian & 3u;                                                                                                        \
        if (e == 1u || e == 2u)                                                                                                      \
            v = ((v << 8u) & 0xFF00FF00u) | ((v >> 8u) & 0x00FF00FFu);                                                               \
        if (e == 2u || e == 3u)                                                                                                      \
            v = (v >> 16u) | (v << 16u);                                                                                             \
        return v;                                                                                                                    \
    }                                                                                                                                \
    uint XePresentWord(uint word, uint endian, uint swapRedBlue)                                                                     \
    {                                                                                                                                \
        uint p = XePresentSwap(word, endian);                                                                                        \
        if (swapRedBlue != 0u)                                                                                                       \
            p = (p & 0xC00FFC00u) | ((p & 0x3FFu) << 20u) | ((p >> 20u) & 0x3FFu);                                                   \
        return (3u << 30u) | XePresentLut(p & 0x3FFu) | XePresentLut(1024u + ((p >> 10u) & 0x3FFu)) |                               \
               XePresentLut(2048u + ((p >> 20u) & 0x3FFu));                                                                          \
    }

constexpr uint32_t kGpuPresentGroupSize = 128;  // local_size_x: Vulkan's guaranteed maxComputeWorkGroupInvocations is 128
constexpr uint32_t kGpuPresentLutWords = 3 * 1024;

// The compute shader (GLSL 450): binding 0 the resolves' words, 1 the gamma tables, 2 the display words; push constants GpuPresentPush.
std::string GpuPresentGlsl();

struct GpuPresentPush  // std430-compatible: the shader's XePresentPush in this order
{
    uint32_t count = 0;        // width * height
    uint32_t endian = 0;       // the resolves' RB_COPY_DEST_INFO endian
    uint32_t swapRedBlue = 0;  // front_buffer.h FrontBuffer::swapRedBlue
};
static_assert(sizeof(GpuPresentPush) == 3 * 4);

// A front buffer as the swap sees it (front_buffer.h DecodeFrontBuffer; pitch in texels, a multiple of 32).
struct FrontShape
{
    uint32_t base = 0, width = 0, height = 0, pitch = 0;
    bool operator==(const FrontShape&) const = default;
};
inline FrontShape FrontShapeOf(const FrontBuffer& f) { return {f.physical, f.width, f.height, f.pitch}; }
// The bytes Detile32 may read (TiledSize32): the range whose last writers decide the presented pixels.
inline uint32_t FrontBytes(const FrontShape& s) { return uint32_t(TiledSize32(s.pitch, s.height)); }

// Where resolve `r`'s GPU words land in front buffer `f`: a band of whole rows. Its words are row-major r.width x r.height (the
// EDRAM unit's resident image, the rectangle at 0,0); the write-back stores word (x, y) as GpuSwap32(word, r.destEndian) at texel
// (cornerX + r.x0 + x, cornerY + r.y0 + y) of the surface at f.base, where (cornerX, cornerY) is the 32x32 tile r.textureBase starts
// (resolve_resident.h ResidentTextureMatches: a base delta that is a multiple of 4 KB names a tile; FM4 resolves its front buffer
// whole, or in three bands at +0, +0x140000 and +0x280000: 1B441000 / 1B581000 / 1B6C1000). Accepted:
// words exist; a 32 bpp destination in a format whose write-back is those words (6, 7, 50, 54: the converting resolves edram_vk.cpp
// writes back from its resident words with FM4_RESOLVE_GPU_WORDS; not 22/23 depth or 36, whose write-back is another path); the
// same pitch in tiles (TilesPerRow: ResolveWordsToGuest and Detile32 both align it up to 32); a 4 KB multiple at or past f.base; the
// band at column 0, exactly f.width wide, inside f.height. Then texel (x, row + y) = GpuSwap32(words[y * f.width + x], endian).
struct FrontBand
{
    bool ok = false;
    uint32_t row = 0, rows = 0;
};
FrontBand FrontResolveBand(const ResidentResolve& r, const FrontShape& f, std::string* why);

// Which of the last front buffers have GPU words that equal what the CPU path would detile now. Command-processor thread only.
//   Note       at every presented swap: the entry of its shape (a new shape replaces the least recently noted entry, its words
//              dropped). The first swap of a front buffer is always presented on the CPU (no words yet).
//   OnResolve  after every recorded resolve that writes guest memory: an entry whose range it overlaps takes its band when
//              FrontResolveBand accepts it with the set's endian (Recorded; the caller copies the words into that entry's buffer at
//              the band's row; an Empty or Landed entry starts a new set of bands) and drops its words otherwise (another write).
//   OnRetired  after the command processor confirmed every part up to `through` retired (their write-backs landed): a Recorded
//              entry whose bands all retired and cover every row is now guest memory's bytes: `watch(base, bytes, sequence)`
//              (guest_writes.h WatchGuestRange) takes the write sequence from which a later write shows (false: dropped). Bands
//              that retire in different retires, or that do not cover every row, drop the words (after its band landed, a row's
//              guest bytes may be written again with nothing watching it).
//   Current    at a swap: Recorded, every row covered, and its newest band the newest pending write over the range
//              (`pendingSerial`, GuestWritesPendingSerial: the write-backs are still to land and will leave exactly these words,
//              FM4_RESOLVE_DEFER's rule), or Landed and nothing wrote the range since (`unwritten(base, bytes, sequence)`,
//              GuestRangeUnwritten). `why` names a refusal.
class FrontWordsTracker
{
public:
    static constexpr size_t kEntries = 3;
    enum class State : uint8_t { Empty, Recorded, Landed };
    struct Entry
    {
        FrontShape shape;
        bool used = false;
        State state = State::Empty;
        uint64_t firstSerial = 0, lastSerial = 0, sequence = 0, noted = 0;
        uint32_t endian = 0, covered = 0;  // the endian of every band of the set; the rows covered (each counted once)
        std::vector<bool> rows;            // shape.height entries: covered by a band of the set
    };
    struct Take
    {
        int entry = -1;  // -1: nothing to copy
        uint32_t row = 0;
    };

    size_t Note(const FrontShape& s, uint64_t swap);
    Take OnResolve(const ResidentResolve& r, uint64_t serial);
    template <typename Watch>
    void OnRetired(uint64_t through, Watch watch)
    {
        for (Entry& e : entries_)
            if (e.used && e.state == State::Recorded && e.firstSerial <= through)
            {
                uint64_t sequence = 0;
                if (e.lastSerial <= through && e.covered == e.shape.height && watch(e.shape.base, FrontBytes(e.shape), sequence))
                    e.state = State::Landed, e.sequence = sequence;
                else
                    Drop(e);
            }
    }
    template <typename Unwritten>
    bool Current(size_t i, uint64_t pendingSerial, Unwritten unwritten, const char*& why) const
    {
        const Entry& e = entries_[i];
        why = !e.used || e.state == State::Empty ? "no resolve bands over the whole front buffer since its last other write"
              : e.state == State::Recorded && e.covered != e.shape.height ? "its resolve bands do not cover every row"
              : e.state == State::Recorded && pendingSerial != e.lastSerial ? "its newest band is not the newest pending write over the range"
              : e.state == State::Landed && !unwritten(e.shape.base, FrontBytes(e.shape), e.sequence) ? "written since its resolves landed"
                                                                                                         : nullptr;
        return why == nullptr;
    }
    const Entry& At(size_t i) const { return entries_[i]; }

private:
    static void Drop(Entry& e);
    Entry entries_[kEntries];
};

}  // namespace runtime::video
