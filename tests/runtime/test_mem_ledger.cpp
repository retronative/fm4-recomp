// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// runtime/video/mem_ledger.h (M9, FM4_DIAG_MEMORY): the renderer's allocation ledger. Expected values are hand-computed sums.
#include "mem_ledger.h"
#include <cstdio>
#include <cstdlib>

using namespace runtime::video::mem;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

int main()
{
    const uint64_t MB = 1048576;
    Ledger l;
    Expect(l.Allocated() == 0 && l.Peak() == 0 && l.Count() == 0 && l.Largest(4).empty(), "empty: nothing allocated");
    // A 2560x1440 colour target (14 MB at 4 bytes a pixel, rounded here to 16), its depth (32), a 1x shadow (4), a texture (8), a ring
    // (256) and the guest's imported memory (512, shared).
    const uint64_t color = l.Add(Kind::EdramColor, 16 * MB, "key 808000");
    const uint64_t depth = l.Add(Kind::EdramDepth, 32 * MB, "key 6082D0");
    const uint64_t shadow = l.Add(Kind::ShadowColor, 4 * MB, "key 808000 shadow");
    l.Add(Kind::Texture, 8 * MB, "texture");
    l.Add(Kind::Ring, 256 * MB, "ring 0");
    l.Add(Kind::GuestShared, 512 * MB, "guest memory");
    Expect(color != 0 && depth != 0 && color != depth && l.Count() == 6, "ids: non-zero and distinct");
    Expect(l.Total(Kind::EdramColor) == 16 * MB && l.Total(Kind::EdramDepth) == 32 * MB && l.Total(Kind::Ring) == 256 * MB && l.Total(Kind::GuestShared) == 512 * MB,
           "totals by kind");
    // 16 + 32 + 4 + 8 + 256 = 316 MB; the shared guest memory is never added (828 would double-count it: CONTROL).
    Expect(l.Allocated() == 316 * MB && l.Allocated() != 828 * MB && l.Peak() == 316 * MB, "allocated: every kind but the shared guest memory");
    const auto top = l.Largest(3);
    Expect(top.size() == 3 && top[0].label == "guest memory" && top[1].label == "ring 0" && top[2].label == "key 6082D0", "largest: by bytes, descending");
    // The colour target is replaced by a smaller one (6 MB) and kept until shutdown: it moves to Retired, the total grows by 6.
    l.Move(color, Kind::Retired);
    const uint64_t smaller = l.Add(Kind::EdramColor, 6 * MB, "key 808000 (720 rows)");
    Expect(l.Total(Kind::EdramColor) == 6 * MB && l.Total(Kind::Retired) == 16 * MB && l.Allocated() == 322 * MB && l.Peak() == 322 * MB, "move: the bytes change kind, not the total");
    // Releasing the shadow and the moved target: 322 - 4 - 16 = 302; the peak stays 322.
    l.Release(shadow), l.Release(color);
    Expect(l.Allocated() == 302 * MB && l.Peak() == 322 * MB && l.Total(Kind::ShadowColor) == 0 && l.Total(Kind::Retired) == 0 && l.Count() == 5, "release: totals fall, the peak stays");
    l.Release(shadow), l.Release(0), l.Release(999), l.Move(999, Kind::Texture);
    Expect(l.Allocated() == 302 * MB && l.Count() == 5, "CONTROL: releasing twice, id 0 and an unknown id change nothing");
    const std::string report = l.Report("swap 600", 2);
    Expect(report.find("[mem] swap 600: 302.0 MB allocated by the renderer in 5 allocation(s) (peak 322.0 MB)") == 0 &&
               report.find("      6.0 MB  EDRAM colour targets (1)") != std::string::npos && report.find("512.0 MB  guest memory imported for the GPU (shared, not additional) (1)") != std::string::npos &&
               report.find("largest  1:    512.0 MB") != std::string::npos && report.find("largest  2:    256.0 MB  upload rings (staging/upload): ring 0") != std::string::npos &&
               report.find("largest  3") == std::string::npos && report.find("resolve shadows (colour)") == std::string::npos,
           "report: the total, each kind with bytes, the shared memory apart, the two largest");
    (void)smaller;
    std::printf("mem_ledger tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
