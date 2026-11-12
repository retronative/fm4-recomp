// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7, vertex reuse across submissions): T0 tests of the vertex cache's placement and lifetime policy
// (runtime/video/vertex_cache.h), CPU only: no Vulkan header, no device. Serials stand for submission parts (render_hooks.h
// currentPartSerial), Retired() for a confirmed retire. Every expected offset, chunk and validity is computed by hand in the
// comment next to it from the header's documented rules (L2-L6): bump allocation aligned up in the current chunk; else the
// recyclable chunk (last reader <= retired watermark) with the smallest last reader, lowest index on a tie. Keys are synthetic
// (no title address). "CONTROL" names a negative control: a wrong behaviour that must not happen.
//
//   tests/run_vertex_cache_tests.sh
#include "vertex_cache.h"
#include <cstdio>

using runtime::video::VertexCache;

namespace {
int failures = 0, controls = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
#define CONTROL(wrong, name)                                         \
    do                                                               \
    {                                                                \
        controls++;                                                  \
        Expect(!(wrong), "control: " name);                          \
    } while (0)

using P = VertexCache::Place;
constexpr uint64_t kA = 0x1000ull << 2 | 2, kB = 0x2000ull << 2 | 2, kC = 0x3000ull << 2 | 2, kD = 0x4000ull << 2 | 2;

void TestLayout()
{
    Expect(VertexCache(4096, 1024, 256).Valid() && VertexCache(4096, 1024, 256).Chunks() == 4 && VertexCache(4096, 1024, 256).Capacity() == 4096,
           "4096 bytes in 1024-byte chunks: 4 chunks");
    CONTROL(VertexCache(4096, 1000, 256).Valid(), "a chunk size that does not divide the capacity is refused");
    CONTROL(VertexCache(1024, 1024, 256).Valid(), "one chunk is refused (a draw could never be placed while it is read)");
    CONTROL(VertexCache(4096, 1024, 3).Valid(), "a non-power-of-two alignment is refused");
    VertexCache bad(4096, 1000, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    Expect(bad.Allocate(16, 1, at, chunk) == P::TooLarge, "an invalid cache places nothing");
}

void TestBumpAlignmentAndSize()
{
    VertexCache c(4096, 1024, 256);
    uint64_t at = ~0ull;
    uint32_t chunk = ~0u;
    // Nothing read yet: every chunk's last reader is 0 <= watermark 0; the lowest index is taken: chunk 0 at 0.
    Expect(c.Allocate(100, 1, at, chunk) == P::Ok && at == 0 && chunk == 0 && c.Recycles() == 0, "first block: chunk 0 at 0, no recycle (fresh chunk)");
    // Bump in chunk 0: 100 aligned up to 256; 256 + 300 = 556 <= 1024.
    Expect(c.Allocate(300, 1, at, chunk) == P::Ok && at == 256 && chunk == 0, "second block: 256 (100 aligned up to 256)");
    // 556 -> 768; 768 + 500 = 1268 > 1024: the next chunk with the smallest last reader <= 0 is chunk 1 (chunk 0's is 1): 1024.
    Expect(c.Allocate(500, 1, at, chunk) == P::Ok && at == 1024 && chunk == 1, "a block that does not fit the rest of chunk 0 opens chunk 1 at 1024");
    // Chunk 1 holds [1024, 1524); a whole chunk (1024) fits only a fresh one: chunk 2 at 2048.
    Expect(c.Allocate(1024, 1, at, chunk) == P::Ok && at == 2048 && chunk == 2, "a block of exactly one chunk takes chunk 2 whole");
    Expect(c.Allocate(1025, 1, at, chunk) == P::TooLarge, "a block larger than a chunk is refused");
    CONTROL(c.Allocate(0, 1, at, chunk) == P::Ok, "an empty block is never placed");
    Expect(c.LastRead(0) == 1 && c.LastRead(1) == 1 && c.LastRead(2) == 1 && c.LastRead(3) == 0, "each used chunk is marked read by part 1; chunk 3 untouched");
}

// A window copied in part 1 is still read from the same place in part 2 (a split: a new part, nothing retired), the case a
// ring lap broke under FM4_VERTEX_REUSE.
void TestHitAcrossSplitAndLap()
{
    VertexCache c(4096, 1024, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    Expect(c.Allocate(100, 1, at, chunk) == P::Ok && at == 0, "part 1 copies window A at 0");
    c.Remember(kA, chunk, at, 25, 7);
    const VertexCache::Entry* e = c.Find(kA);
    Expect(e != nullptr && e->offset == 0 && e->dwords == 25 && e->sequence == 7 && e->chunk == 0, "A is found with its offset, size and sequence");
    c.MarkRead(e->chunk, 2);  // part 2 (after a split) reads it
    Expect(c.LastRead(0) == 2, "the hit marks chunk 0 read by part 2");
    c.Retired(1);  // part 1 retires (a flush or a ring lap of the old path): chunk 0 is still read by part 2
    Expect(c.Find(kA) != nullptr && c.Find(kA)->offset == 0, "after part 1 retires, A is still valid at 0");
    c.Retired(2);
    Expect(c.Find(kA) != nullptr && c.Find(kA)->offset == 0, "a retire alone never drops an entry (only a recycle of its chunk does)");
}

// A guest write (the renderer's GuestRangeUnwritten fails) re-copies the window into fresh space; the old bytes are not reused
// while their chunk can still be read.
void TestCopyOnWrite()
{
    VertexCache c(4096, 1024, 256);
    uint64_t at = 0, first = 0;
    uint32_t chunk = 0;
    c.Allocate(100, 1, first, chunk);
    c.Remember(kA, chunk, first, 25, 7);
    // Part 1 still open; A was written: the renderer copies it again. Bump in chunk 0: 100 -> 256.
    Expect(c.Allocate(100, 1, at, chunk) == P::Ok && at == 256 && chunk == 0, "the re-copy of A goes to fresh space at 256");
    c.Remember(kA, chunk, at, 25, 9);
    Expect(c.Find(kA) != nullptr && c.Find(kA)->offset == 256 && c.Find(kA)->sequence == 9, "A now names the new copy and its sequence");
    // The next block bumps on (356 -> 512); the old copy's [0, 100) is not handed out again.
    Expect(c.Allocate(100, 1, at, chunk) == P::Ok && at == 512, "the next block is at 512");
    CONTROL(at == first, "a replaced copy's space is reused while its chunk may be read");
    c.Forget(kA);
    Expect(c.Find(kA) == nullptr, "Forget drops the entry (an unwatched window is never reused)");
}

// Every chunk read by a part that is not retired: nothing is recycled; the caller must flush (L6). After the retire the least
// recently read chunk is recycled and only its entries die.
void TestNoRecycleWhileInFlight()
{
    VertexCache c(4096, 1024, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    const uint64_t keys[4] = {kA, kB, kC, kD};
    for (uint32_t k = 0; k < 4; k++)
    {
        Expect(c.Allocate(1024, 1, at, chunk) == P::Ok && chunk == k && at == uint64_t(k) * 1024, "fill: a whole chunk each, in index order");
        c.Remember(keys[k], chunk, at, 256, 1);
    }
    // All four last read by part 1 > watermark 0.
    Expect(c.Allocate(16, 1, at, chunk) == P::NeedsRetire, "a full cache read by an unretired part answers NeedsRetire");
    CONTROL(c.Recycles() != 0 || c.Find(kA) == nullptr, "a chunk read by an unretired part is recycled");
    c.Retired(1);
    // All last read by 1 <= 1: the tie goes to the lowest index, chunk 0 at 0; its generation moves, A dies, B-D live.
    Expect(c.Allocate(16, 2, at, chunk) == P::Ok && chunk == 0 && at == 0 && c.Recycles() == 1, "after the retire chunk 0 is recycled at 0");
    Expect(c.Find(kA) == nullptr && c.Find(kB) != nullptr && c.Find(kC) != nullptr && c.Find(kD) != nullptr, "only the recycled chunk's entry is gone");
}

void TestLeastRecentlyRead()
{
    VertexCache c(4096, 1024, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    const uint64_t keys[4] = {kA, kB, kC, kD};
    for (uint32_t k = 0; k < 4; k++)  // part k + 1 fills chunk k
    {
        c.Allocate(1024, k + 1, at, chunk);
        c.Remember(keys[k], chunk, at, 256, 1);
    }
    c.MarkRead(c.Find(kA)->chunk, 5);  // part 5 reads A again: chunk 0's last reader is 5
    c.Retired(4);
    // Recyclable (last reader <= 4): chunk 1 (2), chunk 2 (3), chunk 3 (4); the smallest is chunk 1 at 1024.
    Expect(c.Allocate(512, 6, at, chunk) == P::Ok && chunk == 1 && at == 1024, "the least recently read recyclable chunk (1) is recycled");
    CONTROL(chunk == 0, "the oldest-filled chunk is recycled although part 5 read it after the others (FIFO, not LRU)");
    Expect(c.Find(kB) == nullptr && c.Find(kA) != nullptr && c.Find(kC) != nullptr && c.Find(kD) != nullptr, "B (chunk 1) dies; A, C, D live");
    // Chunk 1 is current: 512 more bump to 1536 without a recycle.
    Expect(c.Allocate(512, 6, at, chunk) == P::Ok && chunk == 1 && at == 1536 && c.Recycles() == 1, "the current chunk fills before another is taken");
    // Chunk 1 full; recyclable: chunk 2 (3), chunk 3 (4): chunk 2 at 2048, C dies.
    Expect(c.Allocate(16, 6, at, chunk) == P::Ok && chunk == 2 && at == 2048 && c.Find(kC) == nullptr && c.Find(kD) != nullptr, "then chunk 2 (C dies, D lives)");
}

// L4 with partial retires: the watermark is below the open part, so a chunk the open part reads is never recycled.
void TestOpenPartPins()
{
    VertexCache c(2048, 1024, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    c.Allocate(1024, 1, at, chunk);  // chunk 0
    c.Remember(kA, chunk, at, 256, 1);
    c.Allocate(1024, 1, at, chunk);  // chunk 1
    c.Retired(1);
    c.MarkRead(c.Find(kA)->chunk, 2);  // the open part 2 reads A
    // Recyclable: chunk 1 only (last reader 1 <= 1; chunk 0's is 2).
    Expect(c.Allocate(1024, 2, at, chunk) == P::Ok && chunk == 1 && at == 1024, "the chunk the open part does not read is recycled");
    Expect(c.Allocate(16, 2, at, chunk) == P::NeedsRetire && c.Find(kA) != nullptr, "then both chunks are read by part 2: NeedsRetire, A intact");
    c.Retired(1);  // a stale, lower watermark changes nothing
    Expect(c.RetiredSerial() == 1 && c.Allocate(16, 2, at, chunk) == P::NeedsRetire, "the watermark never moves back");
}

// L6: after the flush a draw is placed again from its first window. Its first window (a hit) is marked read by the new part
// before its second is allocated, so the second never recycles the first's chunk.
void TestRestartAfterFlush()
{
    VertexCache c(2048, 1024, 256);
    uint64_t at = 0;
    uint32_t chunk = 0;
    c.Allocate(1024, 1, at, chunk);  // chunk 0: window X (the draw's window 0)
    c.Remember(kA, chunk, at, 256, 1);
    c.Allocate(1024, 1, at, chunk);  // chunk 1: other data
    // The draw in part 1: window 0 hits X; window 1 (600 bytes) finds no recyclable chunk.
    if (c.Find(kA) != nullptr)
        c.MarkRead(c.Find(kA)->chunk, 1);
    Expect(c.Allocate(600, 1, at, chunk) == P::NeedsRetire, "window 1 needs a retire");
    VertexCache wrong = c;  // the same state, placed again without marking window 0 first
    c.Retired(1);           // the flush retires part 1; the draw is recorded in part 2
    wrong.Retired(1);
    Expect(c.Find(kA) != nullptr, "X is still cached after the retire");
    if (c.Find(kA) != nullptr)  // a failed check above must not crash the remaining checks (the mutation gate needs the summary)
        c.MarkRead(c.Find(kA)->chunk, 2);  // restart: window 0 hits X again and is marked read by part 2
    // Recyclable: chunk 1 only (last reader 1); window 1 goes to 1024; X stays at 0.
    Expect(c.Allocate(600, 2, at, chunk) == P::Ok && chunk == 1 && at == 1024 && c.Find(kA) != nullptr && c.Find(kA)->offset == 0,
           "after the restart window 1 takes chunk 1 and X stays valid in chunk 0");
    // Without the restart both chunks have last reader 1: the tie takes chunk 0 and drops X, which the draw is about to read.
    Expect(wrong.Allocate(600, 2, at, chunk) == P::Ok && chunk == 0, "(model check) without the restart chunk 0 would be taken");
    CONTROL(c.Find(kA) == nullptr, "a draw's own earlier window is recycled by its later window");
}

}  // namespace

int main()
{
    TestLayout();
    TestBumpAlignmentAndSize();
    TestHitAcrossSplitAndLap();
    TestCopyOnWrite();
    TestNoRecycleWhileInFlight();
    TestLeastRecentlyRead();
    TestOpenPartPins();
    TestRestartAfterFlush();
    std::printf("%d failure(s), %d negative control(s)\n", failures, controls);
    return failures == 0 ? 0 : 1;
}
