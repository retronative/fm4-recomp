// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/video/zpd_report.h (occlusion query reports counted on the GPU; adapted from ReCarbon): the report kinds, the weight of a
// host sample, the count's saturation, the open brackets and the finished segments' ring. Expected values are hand-computed from the
// rules (a block is 64 bytes: END at +0x00, BEGIN at +0x20; guest samples = host samples x guest MSAA / host MSAA / scale^2,
// rounded to nearest, at least 1 when anything passed), not taken from the code's output.
#include "zpd_report.h"
#include <cstdio>
#include <memory>

using namespace runtime::video::zpd;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

int main()
{
    // FM4's observed addresses (g2_v_720_look_zpd): 147C8020 BEGIN, 147C8000 END, 147C8060 BEGIN, 147C8040 END.
    Expect(Classify(0x147C8020) == Kind::Begin && Classify(0x147C8000) == Kind::End && Classify(0x147C8060) == Kind::Begin &&
               Classify(0x147C8040) == Kind::End && Classify(0x147C8010) == Kind::Unknown && Classify(0x147C8004) == Kind::Unknown,
           "classify: +0x20 of a 64-byte block is BEGIN, +0x00 END, anything else unknown (CONTROL)");

    // Weights: 720p (2 halves), a 2x guest surface on a 1-sample host image: each host sample is 2 guest samples (8/4).
    const Weight hd = HostSampleWeight(2, 1, 2), fhd = HostSampleWeight(2, 1, 3), qhd = HostSampleWeight(2, 1, 4), uhd = HostSampleWeight(2, 1, 6);
    const Weight same = HostSampleWeight(4, 4, 2), one = HostSampleWeight(1, 1, 2);
    Expect(hd.num == 8 && hd.den == 4 && fhd.num == 8 && fhd.den == 9 && qhd.num == 8 && qhd.den == 16 && uhd.num == 8 && uhd.den == 36 &&
               same.num == 4 && same.den == 4 && one.num == 4 && one.den == 4,
           "weight: guest MSAA / host MSAA x 4 over halves squared");
    // A 16x16 guest-pixel quad on a 2x surface is 512 guest samples at every resolution: 256 host pixels at 720p, 576 at 1.5x
    // (24x24), 1024 at 2x (32x32), 2304 at 3x (48x48).
    Expect(Normalize(256, hd.num, hd.den) == 512 && Normalize(576, fhd.num, fhd.den) == 512 && Normalize(1024, qhd.num, qhd.den) == 512 &&
               Normalize(2304, uhd.num, uhd.den) == 512 && Normalize(256, same.num, same.den) == 256,
           "normalize: a 16x16 quad counts 512 guest samples at 1x, 1.5x, 2x and 3x; host MSAA equal to the guest's counts as is");
    // Rounding to nearest, never 0 for a nonzero count, 0 stays 0; a zero denominator counts as 1.
    Expect(Normalize(0, 8, 36) == 0 && Normalize(1, 8, 36) == 1 && Normalize(2, 8, 36) == 1 && Normalize(7, 8, 36) == 2 && Normalize(5, 8, 9) == 4 &&
               Normalize(3, 2, 0) == 6,
           "normalize: 1 of 36ths x 8 -> 1 (not 0); 7 x 8 / 36 = 1.56 -> 2; 5 x 8 / 9 = 4.44 -> 4; 0 -> 0");
    Expect(EndCount(0) == 0 && EndCount(1000) == 1000 && EndCount((1u << 24) - 1) == (1u << 24) - 1 && EndCount(1u << 24) == 1u << 24 &&
               EndCount(0x1'0000'0005ull) == 1u << 24,
           "end count: below 2^24 as is; at or above it 2^24 (never wrapped to a small 32-bit value: CONTROL 2^32 + 5)");

    // Brackets: the END at a block's +0x00 closes the BEGIN at its +0x20; another block's END does not.
    BracketBook book;
    Bracket b;
    const bool opened = book.Open(0x147C8020, 5, 100) == BracketBook::Opened::New && book.Open(0x147C8060, 9, 100) == BracketBook::Opened::New &&
                        book.Size() == 2;
    const bool wrong = !book.Close(0x147C8080, &b) && book.Size() == 2;
    const bool closed = book.Close(0x147C8000, &b) && b.s0 == 5 && b.beginAddress == 0x147C8020 && book.Size() == 1 && book.Close(0x147C8040, &b) &&
                        b.s0 == 9 && book.Empty() && !book.Close(0x147C8040, &b);
    Expect(opened && wrong && closed, "brackets: an END closes its own block's BEGIN and gives its first segment; an END without one is refused");
    Bracket old;
    const bool replaced = book.Open(0x147C8020, 1, 7) == BracketBook::Opened::New && book.Open(0x147C8020, 4, 8, &old) == BracketBook::Opened::Replaced &&
                          old.s0 == 1 && old.frame == 7 && book.Size() == 1 && book[0].s0 == 4;
    bool overflow = true;
    for (uint32_t i = 1; i < BracketBook::kMax; i++)
        overflow = overflow && book.Open(0x1000 + 0x40 * i + 0x20, 10 + i, 8) == BracketBook::Opened::New;
    overflow = overflow && book.Size() == BracketBook::kMax && book.Open(0x9020, 99, 9, &old) == BracketBook::Opened::DroppedOldest &&
               old.beginAddress == 0x147C8020 && book.Size() == BracketBook::kMax;
    Bracket first;
    const bool dropped = book.DropBefore(9, &first) == BracketBook::kMax - 1 && first.frame == 8 && book.Size() == 1 && book[0].beginAddress == 0x9020;
    Expect(replaced && overflow && dropped, "brackets: a BEGIN issued again replaces its bracket; a ninth drops the oldest; a swap drops the earlier frames'");

    // The ring: a bracket's count is the sum of its segments [s0, s1); a segment never stored, or overwritten, is lost, not guessed.
    auto ring = std::make_unique<ResultRing>();
    ring->Put(1, 10, 5), ring->Put(2, 20, 10), ring->Put(3, 30, 15);
    uint32_t lost = 0;
    uint64_t raw = 0;
    const bool sums = ring->Sum(1, 4, lost, &raw) == 60 && raw == 30 && lost == 0 && ring->Sum(2, 3, lost) == 20 && ring->Sum(3, 3, lost) == 0 && lost == 0;
    const bool missing = ring->Sum(3, 6, lost) == 30 && lost == 2;
    ring->Put(1 + ResultRing::kSize, 7, 7);  // overwrites segment 1's entry
    lost = 0;
    const bool overwritten = ring->Sum(1, 3, lost) == 20 && lost == 1 && ring->Sum(0, 1, lost) == 0 && lost == 2;
    Expect(sums && missing && overwritten, "ring: sums of segments; an empty bracket is 0; unknown, overwritten and id-0 segments are counted lost (CONTROL)");
    std::printf("zpd report tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
