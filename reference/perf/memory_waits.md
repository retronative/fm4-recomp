# Memory WAIT_REG_MEM syncs and the race frame: findings

A performance investigation of FM4's command-processor waits in the race, and of the opt-in changes
tried against them. Every A/B below is one build, two bounded runs of the same scripted race scene,
both ending at the same swap with 0 validation messages, an idle machine before each run, medians
per frame unless stated. "Countdown" = race-time 0-10 s, matched by vblanks since race start;
"later race" windows are not matched (different draws a frame) and only indicate a direction.
Run-to-run spread with validation on is about ±15 ms in the countdown window, so a single pair
under that is not a verdict. All changes are opt-in environment flags unless stated.

## What the waits poll (VERIFIED: diagnostic log + the translation)

Every memory WAIT_REG_MEM that is not satisfied at once polls one of two words of one block
(`dev[11028]`, physical 08D4D000), diagnostic FM4_DIAG_MEMWAIT:

| word | logged (every 50th swap) | entry -> needed | outcome |
|---|---|---|---|
| 08D4D000 (poll 08D4D002) | 308 | 4 -> 0 | satisfied by the sync (~5-7 ms each); queue held only the INTERRUPT |
| 08D4D004 (poll 08D4D006) | 67 | 1 -> 0 | no flush (nothing pending); the CPU clears it ~5 ms later |

- **Emitter** `sub_822C22A8`, D3D's callback insert:
  - optional WAIT_UNTIL (register 0x5C8 = 0x20000, WAIT_3D_IDLECLEAN), emitted only when the flag
    bit 0 is set and `dev[11072] & 0x80`; that bit is cleared after;
  - type-0 writes of the callback function and data to scratch registers 0x57C/0x57D, written back
    to block+16/+20;
  - scratch register 0x578 = CPU mask (4), written back to block+0;
  - WAIT_REG_MEM waits on the block words, then INTERRUPT, then WAIT_REG_MEM for block+0 == 0.
- **Callers:** five call sites: two with flags 0 (made 6), one with flags 6, one with `r26 | 1`,
  one passing its flags through.
- **Handler** `sub_826E46D0` (registered through VdSetGraphicsInterruptCallback). For source 1 it:
  1. calls `block[16](block[20])`;
  2. under the lock at dev+11032, clears this CPU's bit in block+0.
- **So** the GPU waits for the CPU to acknowledge an interrupt. The sync ran only because
  FM4_DEFER_FENCES held the INTERRUPT until the open work retired. Neither word is a D3D fence (that
  is `*dev[11024]`).

## Change (opt-in FM4_IMMEDIATE_INTERRUPTS=1)

INTERRUPT is raised when reached, as Xenia does and as the ShiftGlue SDK's split recording does
(ShiftGlue SDK `src/graphics/command_processor.cpp:2496-2499`). There are two exceptions:
- a stream store is queued before it (fence order);
- a nonzero WAIT_UNTIL was written since the last INTERRUPT (D3D asked for idle).

A memory wait on a word nothing pending writes, with nothing queued, polls for 2 ms before it
syncs. Tests in `tests/runtime/test_runtime.cpp` cover the immediate case, the WAIT_UNTIL and
queued-store controls, and the grace then one sync.

## A/B (validation on, 480 s)

| median per frame | A menus | B menus | A game 0-10 s | B game 0-10 s | A game 10 s+ | B game 10 s+ |
|---|---|---|---|---|---|---|
| frame time | 17 ms | 17 | 241 | 232 | 198 | 198 |
| submit + wait | 10 | 10 | 122 | 121 | 107 | 106 |
| memory waits not at once | 2 | 2 | 4 | 4 | 4 | 4 |
| of them synced | 2 | 1 | 4 | 4 | 4 | 4 |
| memory-wait time | 10.9 ms | 10.9 | 29.5 | 29.2 | 39.1 | 39.5 |
| immediate interrupts | 0 | 0 | 0 | 0 | 0 | 0 |
| guest CPU | 25 ms | 25 | 450 | 426 | 382 | 380 |
| GPU busy | 7 | 7 | 79 | 76 | 74 | 75 |

- **Result: no effect.** OBSERVED: 0 immediate interrupts in B, so the change never engaged. The
  countdown difference (about 4 %) and the guest CPU difference are run-to-run variance.
- **Why** (OBSERVED): at 150 of 299 logged 08D4D002 waits the queue held only the INTERRUPT itself,
  so the idle rule deferred it: a nonzero WAIT_UNTIL was written between interrupts.

## Whose WAIT_UNTIL

Logged per INTERRUPT: the WAIT_UNTIL value and the packet headers since the previous one
(`CpState::lastHeaders`). The run's timing is INVALID (concurrent load on the machine); the packet
sequences are logical, so they stand.

- **Logged:** 368 INTERRUPTs, all mask 4. The newest headers before the INTERRUPT are WAIT_REG_MEM
  ×2, then 00000578 (CPU mask), then 0001057C (callback function and data), then **000005C8**.
- **VERIFIED: FM4 itself asks for 3D idle.** In 256 of the 368 the WAIT_UNTIL 00020000 comes 5
  packets before the INTERRUPT. It is the callback emitter's own (`sub_822C22A8`, flag bit 0 with
  `dev[11072] & 0x80`). The sync at these callbacks is required, not redundant.
- **The rest** have stores queued before them (fence order), or no open work.
- **Other WAIT_UNTIL emitters** in the translation: 14 functions, including the fence insert
  `sub_8236F6F8`, under the same `dev[11072] & 0x80` guard.
- **Conclusion:** FM4_IMMEDIATE_INTERRUPTS has nothing to remove in the race; it stays opt-in. The
  remaining saving at these drains is to overlap (record the next work while the GPU drains), not
  to skip the drain. 08D4D006 (about 5 ms of command-processor stall per occurrence, no flush) is
  the CPU's acknowledgement latency.

## Vertex window reuse (opt-in FM4_VERTEX_REUSE=1 with FM4_WRITE_WATCH=1)

The largest flush cause left was the ring: it needed 197-499 MB of vertex windows a frame, against
128 MB, and every draw copied its whole window. The change: a window the open slot's ring already
holds is read from that copy, provided it has the same physical start and endian, at least the same
length, and the write watch's sequence shows no write since the copy (guest stores fault; resolves
and system-call writes mark). The vertex binding spans the whole ring. Ranges the watch cannot
protect (the E0000000 view) are copied as before. This is the watched-shared-memory technique of
Xenia, at slot scope.

- **Correctness (FM4_VERTEX_REUSE_VERIFY=1):** every reused window compared with guest memory: no
  mismatch stop through the race, 0 validation messages. Captures render normally (no new artefact;
  not byte-comparable across runs).

| | A | B | change |
|---|---|---|---|
| countdown frame time | 234 ms | 218 ms | −7 % |
| countdown flush waits | 122 | 110 | −10 % |
| ring flushes | 2 × 48.9 ms | 0 | gone |
| submissions | 20 | 17 | |
| GPU busy | 78 | 67 | |
| CPU (frame − waits) | 105 | 106 | = |
| later race frame | 201 | 185 | |
| vertices copied / reused | 197 MB copied | 74 MB copied, 120 MB reused | |

- **Reading:** the gain is the ring flushes and the GPU work behind them, not CPU time; the memcpy
  saving (about 120 MB a frame) is not visible in CPU time.

## Resolve-consumer waits by kind and resident mip levels (FM4_RESOLVE_RESIDENT_MIPS=1)

Tally per 300 race swaps (FM4_DIAG_CONSUMER=2):

| texture | waits |
|---|---|
| k_8_8_8_8 256x256 tiled | 1,425 (about 4.75 a frame) |
| k_8_8 (10) 512x512 | 600 |
| 512x512 f6, 64x32 f54, 32x32 f54, 160x96 f54, 32x32 f36, 512x512 f25 | 300 each (one a frame) |

- **Why (VERIFIED, the resident log):** the 256x256 at 1C2E3000 is a mip chain FM4 fills with one
  resolve a level: 256 at 1C2E3000, then 128, 64 and 32 at 1C323000, 1C333000 and 1C337000.
  Residency refused it as "more than one level".
- **Change:** each unpacked level is matched as a one-level texture at its own address, size and
  pitch (MipLevel); the words are copied into that level (dstSubresource mipLevel). Packed tail
  levels are never matched. Any overlapping resolve that is not adopted still drops the image's
  residency.
- **Tests:** `test_texture_vk` on FM4's fetch constant: by hand from Xenia's tiled layout the levels
  are at 1C323000, 1C333000 and 1C337000, and each of the four resolves goes to its level at (0, 0).
  Controls: the whole-texture match refuses, a wrong pitch, a packed level are refused, a
  single-level texture behaves as before.

| | A | B |
|---|---|---|
| countdown frame | 249 ms | 217 ms (−13 %) |
| countdown submissions | 17 | 13 |
| later race frame | 199 | 186 |
| 256x256 consumer waits (per 300 swaps) | 1,425 | 0 |

- **Caveat:** A was slower than an earlier run of the same configuration (218 ms); with ±15 ms
  spread, submissions −4 and the 256x256 waits gone are the attributable effect.

## Descriptor set cache (FM4_DESCRIPTOR_CACHE=1): no gain

Profile of the command-processor thread (validation on): vkUpdateDescriptorSets 9.8 %,
vkAllocateDescriptorSets 3.2 %, vkResetDescriptorPool 1.9 %, mostly inside the validation layer; the
validation layer's own frames about 21 % of the thread, the driver under Metal API validation about
9 %.

The change: dynamic buffer descriptors, and a set reused per (layout, textures, index range) within
a slot. The first run compiled about 2,100 Metal pipelines cold (MoltenVK's MSL changes with
dynamic offsets: 14-25 s frames at race start), INVALID. Warm: countdown 265 vs 248 ms, later race
201 vs 197 ms; sets reused about 865 a frame, written about 456. **No gain**: the dynamic-offset
binds cost about what the writes did. Opt-in, not in the stack.

## Index reuse (FM4_INDEX_REUSE=1)

BuildHostIndices was about 5 % of the command thread. The change keeps converted host indices on
the CPU, keyed by every input of the conversion, while the write watch's sequence for the index
range is unchanged (protect, then the sequence, then the read). It works across slots and frames;
the cache is cleared above 128 MB.

| | A | B |
|---|---|---|
| countdown frame | 298 ms | 282 ms (−5 %) |
| countdown CPU (frame − waits) | 144 | 134 |
| later race frame | 203 | 187 (−8 %) |
| later race CPU | 100 | 86 |
| indexed draws a frame | | 1,157 reused, 2 converted |

- **Caveat:** both arms ran on a busier machine (baseline 298 ms against 217-249 before): compare
  only within the pair.
- **Open:** a stream store (MEM_WRITE) applied straight to guest memory does not mark the watch. An
  index buffer written that way would be missed; none has been observed. A verify mode is needed
  before this is proposed as a default.

## Resolve write-back from the GPU words (FM4_RESOLVE_GPU_WORDS=1): INCONCLUSIVE

The CPU conversion of resolve read-backs (ResolveConvertToGuest, about 7 % of the command thread) is
replaced, for resolves the GPU already converts (FM4_RESOLVE_RESIDENT), by tiling and
endian-swapping those words (ResolveWordsToGuest). Test: `test_edram_vk` TestResolveWordsToGuest
(hand-computed tiled offsets at pitch 1280, equality with ResolveConvertToGuest for
ResolveConvertPixel's words, refusals that write nothing). Countdown 265 vs 268 ms (no change);
later race 202 vs 183 ms, CPU 101 vs 91. Kept in the stack (no regression), no gain claimed.

## Split submission (FM4_SUBMIT_SPLIT=<draws>)

Every flush was submit + wait, so the GPU's work never overlapped the CPU's recording: about 95 ms
of waits a race frame for about 60 ms of GPU work.
- **The change:** once the recording slot 0's command buffer holds <draws> draws, it is ended and
  submitted without waiting, and recording continues in an idle part slot (command buffer and fence
  only). Slot 0's ring and descriptor pool are shared and reset only after every part retires, in
  order. Reference: the ShiftGlue SDK's `vulkan/command_processor.cpp:6047-6054` (split every N
  draws without waiting), adapted to FM4's about 200 draws between flushes.
- **Safety:** GpuGuard tracks each part as its own submission (gate Enter/Leave, bounded retire);
  `test_gpu_guard` covers two slots in flight independently.

| | A | B (128) |
|---|---|---|
| countdown frame | 248 ms | **226 ms (−9 %)** |
| countdown flush waits | 121 | 82 (−32 %) |
| countdown CPU (frame − waits) | 132 | 147 (more submissions) |
| splits a frame | | 5.3 |

- The timestamp "GPU busy" now spans the idle gaps between parts (106 vs 72 ms), so it no longer
  measures execution.
- **Split size** (8 slots): 128 → frame 211 ms, 4.7 splits, waits 75, CPU 138; 48 → 227 ms, 17.4
  splits, waits 80, CPU 145. The extra submissions cost CPU and did not shorten the waits: 128
  stays. With asynchronous submission (below) 64 and 32 were again no gain.

## Hot-range skip (FM4_HOT_SKIP=1): no gain

Countdown 234 vs 237 ms. Faults stayed at about 1,050 a frame although about 1,400 ranges a frame
were not watched, so the faults come from the texture watch. Opt-in, not in the stack.

## Where the GPU time goes (FM4_DIAG_PASS_TIMES, validation on)

Medians per race frame: frame 186 ms; GPU busy span 93 ms (includes idle gaps with split
submission); draw render passes 51 ms of GPU in about 92 passes.

| target (host size, samples, colour VkFormat, depth VkFormat) | GPU ms per 300 race swaps | passes |
|---|---|---|
| 1280x1024 x2, R16G16B16A16_SFLOAT (97), D32_SFLOAT_S8_UINT (130): the main scene | 13,077 | 6,696 |
| 400x3280 x2, 37, 130 | 927 | 1,499 |
| 1280x2048 x1, 97 | 324 | 4,232 |
| 1280x2048 x1, 64 | 314 | 300 |
| 280x2344 x4, 97, 130 | 234 | 330 |
| all others | < 100 each | |

The main scene's passes cost about 43.6 ms a frame (85 % of draw-pass GPU time), in about 22 passes
a frame, about 2 ms each. Each pass covers the whole 1280x1024 host target (render area = image),
loading and storing both attachments, although the screen is 720 rows.

## Pass render area (FM4_PASS_AREA=1): no gain

Render area = the rows the draws touch. Countdown frame 234.3 vs 233.9 ms; draw-pass GPU 64.2 ms in
91 passes vs 68.4 ms in 92. FALSIFIED for the countdown: the narrower render area did not reduce
the draw passes' GPU time (either MoltenVK's Metal load and store still cover the attachment, or
those passes' draws reach the full rows). Opt-in, not in the stack. The later native breakdown
confirms it: the fixed load/store cost of a pass is at most ~0.12 ms; the cost is shading.

## Where 30 FPS has to come from (validation on)

Countdown frame about 230 ms. The command-processor thread is the bottleneck: never idle, about
174 ms of CPU a frame.

| bucket | about | lever |
|---|---|---|
| debug layers on the command thread (Khronos validation 29.6 %, Metal API validation 12.5 %) | 42 % of the thread, about 70 ms | measure native frame time without validation |
| our own work on the command thread (draw decode, texture binds and untiling, resolve write-back tiling, present and capture, submits) | 42 %, about 70 ms | per-item 2-5 % each; structurally a second thread (PM4 parsing and draw preparation on one, Vulkan recording and submission on another, as the ShiftGlue SDK's async submission worker) |
| waits (GPU tail, 3 required callback drains, about 6 resolve-consumer flushes) | about 85 ms | split submission overlaps part; the remaining consumer kinds need GPU paths (k_8_8 16 bpp, k_16_16, k_32_FLOAT, depth-as-colour, pitch remaps) or GPU-side shared memory |
| GPU draw passes | about 64 ms (main 1280x1024 2x HDR target about 44 ms in about 22 passes) | shader cost, not load/store area |

None of these alone reaches 33 ms.

## Fast texture untiling (FM4_FAST_DETILE=1)

DetileBlocksAt was about 5 % of the command thread, mostly memmove calls for 1-16 byte blocks.
DetileBlocksAtFast uses a fixed-size copy and an integer swap per block size, with rows on the
worker pool (`parallel_rows.h`). Test: `test_texture_info` checks the same bytes as DetileBlocksAt
for every block size and supported endian, a packed origin and a 256x256 region, and the same
refusals. Countdown frame 231 → 220 ms (−5 %), CPU 151 → 136, waits 85 = 85: one pair near the
noise band, consistent with the profile share. Added to the stack.

## Deferred event writes (FM4_DEFER_EVENTS=1): neutral

EVENT_WRITE_EXT (extents) and EVENT_WRITE_ZPD (sample counts) store through the deferred queue as
big-endian dwords (the same bytes; tests in `test_runtime`), with no sync.

| countdown | A | B |
|---|---|---|
| CPU-sync flushes | 5 (47.0 ms) | 3 (39.0 ms) |
| consumer flushes | 7 (38.8 ms) | 7 (44.1 ms) |
| submissions | 13 | 11 |
| frame | 219 ms | 219 ms |

The GPU work moved to the next flush; frame time unchanged. Kept (fewer syncs, no regression), no
gain claimed. About 95 ms of GPU work against about 135 ms of CPU, yet a 219 ms frame: the overlap
is poor.

## k_8_8 on the GPU (FM4_RESOLVE_RESIDENT_K88=1)

Resolve shapes without GPU words (FM4_DIAG_CONSUMER):
- storage 0 to k_8_8 at 1x: FM4's chain 512x512 (and 512x256) down to 8x8, each at its own 4 KB-aligned
  base, texel (0, 0);
- storage 14 to k_32_FLOAT (36) at 1x, 32x32, a fast copy;
- storage 4 to k_16_16 (25) at 4x, 512x512, exp bias 10;
- depth to k_24_8 (22) and k_24_8_FLOAT (23).

**The change:** storage 0 to k_8_8 is converted on the GPU (kResolveResident: r | g << 8, the CPU
rule) into an R16_UINT resident image and copied into k_8_8 textures at their base.
FM4_RESOLVE_RESIDENT_CHECK compares the 16-bit words too. MoltenVK 1.4.2 reports a blend false
positive for R16_UINT (blendEnable is false); it is allow-listed exactly as R32_UINT and R16G16_UINT
are.

- **Check run:** every k_8_8 shape 512x512 ... 8x8 had **0 differing pixels** against the CPU
  conversion, 0 validation messages; k_8_8 512x512 consumer waits fell from 825 to 224 per 300
  swaps (the rest presumably resolves into its mip range that are not adopted).
- **A/B:** countdown frame 218 vs 215 ms; consumer flushes 7 (42.7 ms) vs 7 (36.7 ms); later race
  168 vs 156. A small gain; the flush count stays at 7 because the other consumer kinds still
  flush at those points. Kept (byte-exact).
- **Remaining consumer kinds, one wait a frame each:** k_32_FLOAT 32x32 (a 1x fast copy: a direct
  image copy would do), k_16_16 512x512 (4x, bias 10: uint-view source variant), three
  k_2_10_10_10 at another pitch (a remap), and k_8_8_8_8 512x512 read from a depth resolve.

## Native measurement, validation off

Same binary and stack, no layer, no MTL_DEBUG_LAYER.

| window | frame | waits | CP CPU | GPU busy | subs | guest CPU |
|---|---|---|---|---|---|---|
| validation on, game 0-10 s | 215 | 72 | 174 | 101 | 11 | 417 |
| validation off, game 0-10 s | 114 | 60 | 63 | 54 | 10 | 175 |
| validation off, game 10-22 s | 100 | 46 | 58 | 53 | 9 | 214 |
| menus, on / off | 66 / 17 | | | | | |

Validation cost ~47 % of the race frame, CPU and GPU (Metal API validation also doubled GPU busy).
A late-race native swap: 82 ms = CP CPU ~40 + submit-and-wait ~53 (CPU sync 3x = ~36 ms: the game's
3D-idle drains, which wait for the GPU's remaining work; resolve consumers 5x = ~16.5 ms); GPU busy
~48 ms. Main 1280x1024 x2 HDR passes ~20 ms a swap (about 2x lower than with validation), shadow
400x3280 ~5.9 ms. **Conclusion:** CPU recording and GPU execution are serial (drains 3x a frame);
the frame ≈ CP CPU + GPU remaining. Both must fall below ~33 ms combined for 30 FPS.

## GPU cost breakdown, native

Target: GPU under 14 ms a frame. Race 0-10 s: frame 132, GPU busy 59; later: frame 104, GPU busy 55.
Per race frame:
- main 1280x1024 x2 HDR draws ~30 ms: passes of 32-127 draws ~2.5 ms each (~6/frame), 128+ draws
  ~2.5 ms (~5.6/frame); 2-7 draws 0.12 ms, 8-31 draws 0.34 ms. So the fixed load/store cost of a
  pass is at most ~0.12 ms: the cost is shading, not pass boundaries.
- shadow 400x3280 x2 ~8.7 ms (geometry-heavy: points at vertex-shader cost too).
- 1280x2048 x1 colour 97, 1-draw passes ~3 ms (16.5/frame, 0.18 ms each); other kinds ~3 ms.
- between passes ~12 ms: EDRAM transfers ~4.3 ms (17/frame, 0.25 ms each), resolve-related ~7.5 ms.
- passes ended by (per 300 swaps): resolve resident 6746, another target 6724, EDRAM transfer 4224,
  split submit 3413, resolve 929, dump 735, texture 609, submit 529.

Largest lever: shader arithmetic. `precise` becomes `[[clang::optnone]]` spvFAdd/spvFMul/spvFSub
calls in SPIRV-Cross's MSL (hot PS: 49 calls); all 666 dumped shaders compile without it, with 0
helper calls.

## FM4_SHADER_NO_PRECISE A/B (native)

Measured GPU = draw passes + between passes (FM4_DIAG_PASS_TIMES; "GPU busy" is the submission's
wall span incl. CPU gaps between split parts, not GPU work). The off arm's warm-up pair was
invalidated by a concurrent video call and repeated.

| race | draw passes | between | measured GPU | frame |
|---|---|---|---|---|
| off, swaps 0-80 | 47.0 | 7.4 | 54.4 | 150 |
| on, swaps 0-80 | 14.6 | 16.2 | 30.8 | 132 |
| off, swaps 80-226 | 38.8 | 9.0 | 47.8 | 109 |
| on, swaps 80-143 | 12.2 | 19.8 | 32.0 | 107 |

Per pass (race start): main 32-127 draws 2.49 → 0.52 ms; 128+ 2.52 → 0.62; shadow 128+ 3.29 → 0.92;
full-screen 1-draw 0.184 → 0.044. The pass/between split is fuzzy on Metal (timestamps at encoder
boundaries); the sum is reliable. The frame barely moves: the CP is CPU-bound (~70 ms).
Correctness: a static menu screen identical but for the animated spinner box (x1180-1208,
y655-683, 270 px); race frames visually correct, no NaN/black artefacts. The 2/255 tolerance on 3D
frames is NOT measured (no same-frame oracle across runs).

## k_16_16 resolve number format + untiled-tiling data

- FM4_DIAG_BINS (native): predicated draws are never skipped (bin mask stays FFFFFFFF; 0 skipped in
  60 swaps); per race frame about 975 draws under select 0x80000003 (tile 0), 284 under 0xC
  (tile 1), 960 under FFFFFFFF (untiled passes), 207 unpredicated. So FM4 bins per object itself:
  untiled rendering (as the ShiftGlue SDK does for FM4) would save only tile 1's ~12 % of draws plus
  its resolves/transfers. Deferred.
- The k_16_16 fix (number format 3 → signed int16 pack; 16_16 unpack clamp −32) and
  FM4_SHADER_NO_PRECISE both pass a validated run (layers on, 0 messages, no lock-up). A dumped
  512x512 load at 1C1E3000 was the texture's first load, before a resolve wrote it: all 0x7FFF7FFF
  (a cleared target passed through). Cockpit captures: no visible change.

## FM4_TRANSFER_STENCIL_EXPORT (native); unit-pass timing

The device offers both features (sampleRateShading, VK_EXT_shader_stencil_export): one draw per
transfer. A first pair measured "transfer to depth 640x1024 x4" 0.89 → 0.19 ms each and "transfer to
depth 1280x1024 x2" 698 → 163 ms per 300 swaps, but every draw-pass kind got 30-50 % slower per pass
in the on arm (including 1280x2048 1-draw passes WITHOUT a depth attachment, 0.064 → 0.088 ms); the
on arm ran right after a long heavy CPU load, so it was repeated as cool back-to-back runs.

## Stencil export, cool back-to-back runs

Per race swap (draw passes / unit passes / between = GPU; µs per draw pass):
- off: 13.3 / 18.2 / 5.5 = 37; 139 µs a pass; frame 118 (first 80) / 95.
- mode 1 (one per-sample-shaded draw): 14.6 / 7.2 / 3.7 = 26.4; 173 µs a pass (+25-35 %, as in the
  first pair).
- mode 2 (one export draw per sample, no sample shading): 13.7 / 8.9 / 3.3 = **25.9**; 143 µs a pass
  (normal); frame 74.7 average, 68 in the race proper.

**VERIFIED:** the per-sample-shaded pipelines slow every later draw pass by ~30 % (whole-GPU,
including passes without depth); plain stencil export does not. Mode 2 is the candidate default:
depth transfers 640x1024 x4 0.89 → 0.22 ms each. Remaining GPU 25.9: draw passes 13.7 (main 2x HDR
~0.7 ms x ~10 passes; 1-draw 1280x2048 post passes ~13 x 0.08), transfers ~5.5 (640x1024 x4 depth 1.9
+ colour 1.1, 1280x2048 colour 0.75, 1280x1024 x2 colour 0.75, 520x1264 x4 depth 0.5, 1280x2048 depth
0.5), clears ~0.6, resident ~0.4, between 3.3. Pass enders per 300 swaps: resolve resident 6085,
another target 4584, EDRAM transfer 3222, **split submit 2475** (8 a swap, our own; each a reload +
store of the 2x HDR attachments), submit 579, texture 356.

## FM4_SPLIT_AT_PASS_END

Split only at a pass boundary. Off: frame 74.7, waits 34.5; draw passes 13.7 ms in 95.5 passes; GPU
25.9. On: frame 79.6, waits 39.2; draw passes 12.0 ms in 85.0 passes; GPU 24.0; "split submit" pass
ends 8 → 2 a swap. GPU −1.9 ms but frame +4.9 and waits +4.7: later splits start the GPU later. The
frame must improve, so it is left off; to revisit when the waits are smaller.

Transfer pairs (FM4_DIAG_TRANSFERS, 60 race swaps): depth@720 1x ↔ 4x (2.4 a swap each way, 105k +
79k tiles); depth@1024 4x → 2x (1 a swap, the whole 2x HDR depth, 61k tiles); colour@0 4x f3 ↔ 2x f3
(1 a swap each way); colour@0 1x f2 → 4x f0; colour@832 4x f4 → depth@1024 4x; depth@0 4x f0 →
colour@0 4x f3 (2 a swap) ... Same-region MSAA ping-pong is useful only if the destination reads the
content before overwriting it.

## FM4_DIAG_OVERWRITE: the draws that follow ownership transfers

FM4's clear is a rectangle-list draw (prim 8, 3 vertices, one VS, PS a constant colour) with
pre-transformed vertices (PA_CL_VTE xy/w 1, scale 1, offset 0; stride 7 floats): v0(−0.5,−0.5)
v1(W−0.5,−0.5) v2(W−0.5,H−0.5), i.e. exactly [0,W)x[0,H) with the half pixel; depth z1 w1 func 7
(ALWAYS), stencil 1 func 7 zpass 2 (REPLACE) wmask FF; colour mask F or 0. It follows the depth
ping-pong transfers: 1440 tiles (the 4x 640x360 target, colour+depth), 416 tiles (the 2x 1280x256
band, depth only), 96 tiles (an 80x48 4x region); a 1664-tile transfer precedes a partial clear (a
32x512 strip: that one is needed). Full-screen post quads (prim 8 count 24, the NDC grid, or a
prim-6 strip (0,0)-(1280,720)) with depth off, colour mask F, no blend overwrite their colour target
too (1024- and 720-tile transfers wasted), but their coverage needs the VS's transform.

Plan (FM4_OVERWRITE_CLAIM): an overwrite claim (an idea from the ShiftGlue SDK, implemented
independently) for the clear class: VS position passthrough proven by the translator, the three
vertices read through the guest view, rect ∩ scissor as the Ownership::Change cutout when
depth+stencil (or all colour channels, no blend/alpha test/kill) are replaced; depth-only clear
when stencil is REPLACE FF too. Expected ~3 ms a frame of the ~5.5 ms of transfers. Measured: 12
claims a swap, frame 76.5 / 72.6 vs 73.3 / 68.8 (no gain: CPU-bound); it reduces executed GPU work
(transfers) and is a candidate again when the GPU becomes the limit.

## Metal System Trace (20 s of the race, xctrace attached)

Per second of race (about 13 swaps/s): Fragment, Vertex and Compute channel time, with 2349 render
encoders/s; Compute was dominated by "Record GPU Counter Sample" (4736 blit encoders/s: MoltenVK's
vkCmdWriteTimestamp, i.e. FM4_DIAG_PASS_TIMES's two samples per pass), i.e. the pass diagnostic adds
~380 encoders a frame of its own. The shader-profiler tables are empty in this template (no
per-shader sampling). **Consequence:** "GPU time" for the 14 ms target must be executed GPU work,
measured with the pass diagnostics OFF (FM4_GPU_TIMESTAMPS' two samples a submission only) or from a
trace. The absolute figures of this first aggregation are withdrawn (see the correction below).
Pass diagnostics off: frame 73.3 / 68.8 median, waits 33.0, CP CPU 46.6: the diagnostic costs ~1 ms.

## CPU phase 1: captures off and the resolve write-back unwatch

- PNG captures off by default: frame 73.3 → 67.9 mean, 68.8 → 66.5 median; CP CPU 46.6 → 42.4.
  Kept (default).
- FM4_RESOLVE_HOST_WRITE=1 (first 450 race swaps matched): watch faults 1140 → 13 a swap, fault hook
  21.8 → 0 ms, other threads' CPU 53.2 → 27.6 ms a swap, waits 31.8 → 26.1, CP CPU 43.9 → 43.8,
  frame 69.2 → 66.9 mean / 66.9 → 66.4 median. Kept (opt-in in the stack). Frame ~67 ms (15 fps).

## CPU phase 2: asynchronous submission

CP CPU 43.8 → 36.9 ms (MoltenVK encodes at vkQueueSubmit; that is now off the CP thread, as the
profile predicted: it was 19.5 % of it); frame 66.9 → 65.7 mean, 66.4 → 65.0 median; waits 26.1 →
27.7; GPU busy span 35.5 → 29.8 (the GPU gets its parts sooner). Kept (opt-in in the stack). The
frame now follows the GPU tail at each drain (CP 37 + waits 28).

## CPU phase 3: poll by yield and per-part retire

- FM4_WAIT_POLL_US=2000: frame 65.7 → 61.4 mean / 65.0 → 64.2 median; waits 27.7 → 24.2; memory
  waits 19.7 → 17.5; CP 36.9 → 39.8 (the yield spin is CP time). Waits = fences 16.4 + write-back
  6.3 ms a swap. Later repeats: mean −4.3 beyond the ±1.5 noise, median inside it: unproven, kept
  (no GPU effect).
- FM4_PARTIAL_RETIRE=1: 6 partial retires a swap, 5.9 of them with a forced split of the open part
  (the resolve and its consumer are close), parts 14 → 20 a swap. A first pair showed 58.8 / 55.3
  ms, but repeats of the same configuration gave 65.1 / 65.3, 61.7 / 64.2 and 63.1 / 65.2: that
  pair was an outlier. Neutral within noise and +2 ms of executed GPU (the forced splits end render
  passes, each a reload + store of the attachments): removed from the stack (code opt-in).
- Split after every resolve: 37.5 parts a swap, frame 66.2: rejected.
- Tiled-run resolve store (FM4_TILED_RUNS=1): byte-exact (TestTiledRuns), write-back 5.8 vs 5.7 ms,
  worker CPU unchanged: neutral, opt-in.

**Rule from an incident with a split-after-resolve change:** a ForceSplit right after the resolve
let the resident-resolve adoption that follows record into the command buffer the split had already
ended and submitted (stop 13, "retire: slot 1 completion unknown", GPU_STOP written in-process; no
GPU restart or hang in the system log; validation was off, so no layer caught it). Fix: the split is
a flag consumed at the next draw's entry (before anything of that draw is recorded), and
RequireRecording stops the run (fail closed) whenever a unit would record into a handle that is not
the slot being recorded (texture bind, target acquire, resolve adoption).

**State of the stack:** ~61-65 ms mean / 64-65 median = CP 37-42 + waits 24-25 in series; the waits
have not moved since asynchronous submission, whatever the CP gained.

## Memory (Game Memory template, 60 s from race start)

Metal current allocated size: time-weighted median 1150 MiB, peak 1162 MiB, min 1117 MiB. Resource
events in the window (all in MoltenVK's IOGPUMetalHeap): buffers 175 allocations / 595 MiB and 167
deallocations / 587 MiB (the per-submission transient uploads: ~3.5 MiB each, net +8 MiB), textures
54 allocations / 2.6 MiB. Process footprint (footprint(1) sampled beside the game, no profiler):
race median 1733 MiB, peak 1804 MiB (RSS 747 / 773 MiB); whole run median 1020 MiB; before the race
median 1019, peak 1184. Metal's 1150 MiB sits inside the footprint.

## CORRECTION: executed GPU work per frame

A first aggregation of the Metal System Trace summed only Instruments `metal-nesting-level` 1 rows
and gave 8.0 / 9.1 ms a frame: WITHDRAWN. The level is the overlap lane within a GPU channel, not a
command-buffer/encoder hierarchy; level 1 held a small subset of the encoders. Re-aggregated with
one method over every fm4recomp encoder row (first `duration` column = Duration, the second is
CPU-to-GPU latency):

| stack | fps (race) | Fragment | Vertex | Compute | channel sum | GPU busy (union over channels) | in-run gpu_busy_span (median) |
|---|---|---|---|---|---|---|---|
| GPU-phase stack, DIAG_PASS_TIMES on | 11.76 | 14.2 | 10.3 | 5.8 (4.8 our timestamp samples) | 31.4 | **22.6** | 33.7 |
| kept stack incl. PARTIAL_RETIRE | 13.47 | 17.7 | 13.4 | 1.9 | 33.2 | **28.2** | 46.7 |
| final stack (PARTIAL_RETIRE off, TILED_RUNS off) | 13.45 | 16.5 | 12.7 | 2.0 | 31.7 | **26.7** | 34.2 |

(ms a frame; the fps denominator is the race average of the run; the trace window's own rate is
~14.7/s, which would scale the figures by 0.91.) The independent cross-check is the in-run
FM4_GPU_TIMESTAMPS span (two samples a submission): it agrees in trend and over-counts only the idle
inside submissions. **The ≤ 14 ms target is NOT met**: 26.7 ms a frame GPU busy on the final stack;
the fragment channel alone is 16.5 ms.

Encoder histogram (final stack, per second of trace): 3100 fragment + 3100 vertex render encoders
(~230 a frame). Fragment: the <100 µs encoders (2477/s) cost 65 ms/s (4.8 ms a frame), the
100-500 µs ones (579/s) 122 ms/s, >500 µs (43/s) 36 ms/s. Vertex: <100 µs (2852/s) 37 ms/s,
200-1000 µs (171/s) 86 ms/s, >1 ms (30/s) 45 ms/s. So the small passes' fixed overhead is ~7.5 ms a
frame of channel time and the heavy passes (shading; ~8 vertex passes a frame over 0.5 ms each) the
rest. UNRESOLVED: GPU busy rose 22.6 → 26.7 ms (+18 %) at constant per-encoder cost with
encoders/frame 200 → 230: scene variance between trace windows, or a kept change that alters the
pass structure (TRANSFER_STENCIL_EXPORT=2, PASS_MERGE). Caveats: Apple GPUs scale clocks with load
(36 % busy here), so per-encoder costs may change at 30 fps (HYPOTHESIS); WindowServer takes ~1.8 ms
a frame of GPU in present mode, outside the union.

## Summary

- FM4's memory waits are D3D callback acknowledgements; most are preceded by FM4's own WAIT_UNTIL
  3D-idle request, so the drain is required. Fewer sync points only move the wait to the next one;
  only less GPU work a frame, or overlapping CPU recording with the drain, shortens it.
- What moved the frame: vertex window reuse, resident mip levels, split submission (128 draws),
  fast untiling, index reuse, FM4_SHADER_NO_PRECISE (GPU), stencil export mode 2 (GPU), captures
  off, resolve host write, asynchronous submission.
- What did not: immediate interrupts (never engaged), descriptor cache, hot-range skip, pass render
  area, deferred events (neutral), smaller splits, split at pass end, partial retire, split after
  every resolve, overwrite claim (GPU-only gain while CPU-bound).
