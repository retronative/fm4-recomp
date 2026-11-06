# Texture cache, CPU side

Design notes for the texture cache's CPU side. The unit was first checked by one CPU-only test binary
(ASan/UBSan): 141 pass / 0 fail, 46 of them negative controls. Labels: VERIFIED / OBSERVED /
HYPOTHESIS / FALSIFIED / UNRESOLVED. Sources: **X** = xenia-canary `src/xenia/gpu/` (cited by
file:line); **MVK** = MoltenVK `v1.4.2` source; "an earlier FM4 port" = an earlier FM4 port attempt
whose observations and reviewed code are cited where used.

## 1. Files

| File | What |
|---|---|
| `runtime/video/texture_info.{h,cpp}` | fetch-constant decode, format table (64), host mapping (5 formats); `DescribeTexture` = the **storage** (level range and layout, watch ranges; a function of the `TextureKey` fields only, cached per key); `DescribeBinding` = the **per-draw** part (component mapping, gamma, exp/LOD bias, sampler; a function of the swizzle, signs, clamps, filters and the shader's fetch instruction; bit arithmetic at every draw, no level loop); refusals split accordingly |
| `runtime/video/texture_untile.{h,cpp}` | `TiledOffset2D` (X transcription, pitch aligned), upper bound, pitches/extents, endian rules, tiled and linear copies with an origin block |
| `runtime/video/texture_cache.h` | `WriteWatch` (per-page sequences, epoch, fault hook, `MarkWritten`, `Rearm`), `TextureKey`, `TextureCache` (find / plan / loaded), the contract in its header |
| `tests/runtime/test_texture_info.cpp`, `tests/run_texture_tests.sh` | 141 checks, 46 of them negative controls; the script follows `tests/run_renderer_guard_tests.sh` |

An independent transcription of X's layout functions (a small script, not distributed) cross-checks
the hand values.

Attribution: `DecodeTextureFetch`, the mip-level layout rules and `HostSwizzle`'s folding, and the
pitch/extent/copy helpers, are adapted from an earlier FM4 port attempt (GPL-3.0-or-later, same
copyright holder), re-derived against X and marked in each header. The write watch is written new
after a reference Xbox 360 port's design (read, not copied; none of its code or addresses).

## 2. Trace-derived facts

A CP trace to the intro (27,725 lines, 21,645 draws; not distributed) has per-draw state registers.
Its shader hashes match the shader census names (the hash recipe of `cp_trace.cpp` is the census's;
older traces used an earlier, uncommitted recipe). The textured draws of the first frames, OBSERVED:

| PS (words) | Census `texture_slots` | Draw state |
|---|---|---|
| `8432bb77068e9a2d` (21) with VS `2ea19ab24eb902b8` (42) | **0** | `op 22` (DRAW_INDX), `surface 14000500`, `color 000A0000` (RB_COLOR_INFO format 10), `scissor_br 02D00500` (1280x720), `initiator 0020008D`, `blend0 07060706`; 395 loads, 357 draws of this pair |
| `7e80c717a36c3d9a` (123) with VS `7f2c7cb69e591601` (24) | **0, 1, 2** | `op 22`, `color 00010000` (format 1, `k_8_8_8_8_GAMMA`), `initiator 00040086`, `blend0 00010001`; 38 loads = the intro video's frames (the YUV->RGB shader with three `k_8` planes into a GAMMA target) |

No other textured census shader (`351bebda`, `666b5a4f`, `93879bb4`, `c7be8798`, `cabc1bc4`,
`db89be56`, `e9cd98c0`, `03726933`, `221907d2`) loads in that trace. So the cache must serve exactly
**slot 0** (the 357 UI draws) and **slots 0-2** (the video) to reach the intro.

**What the trace logs** (`cp_trace.cpp` `TraceDraw`): for each draw and texture slot s, the six
registers **`0x4800 + 6 * s` .. `0x4800 + 6 * s + 5`** (`kShaderConstantFetch00` in
`kernel/graphics.h`; X `xenos.h:1134`: XE_GPU_REG_SHADER_CONSTANT_FETCH_*, 32 texture fetch constants
of 6 dwords = `0x4800..0x48BF`; `GetSamplerParameters` reads `FETCH_00_0 + fetch_constant * 6`,
`vulkan_texture_cache.cc:614-615`), as `tex <s> <d0> .. <d5>`. `DescribeTexture` + `DescribeBinding`
on the logged dwords give the exact format, size, tiling, endian, level count, swizzle and sampler,
with no GPU run.

- The earliest traces (draw state only, no fetch constants) show the first 48 draws with VS
  `8bb7e189d92e3def` (24 words) and PS `438c2af84c78a133` (9 words), then VS `539ea9e08aa83f0c`
  (27 words) with the same PS, surface `0A020280` / colour `00020000` / depth `000102D0`. The 9-word
  PS is, by size, `ps_277a2f4a3e7ec198` of the runtime census (`texture_slots -`); every 24- and
  27-word VS in that census has `texture_slots -` too. So **those draws sample no texture**
  (OBSERVED by size); this agrees with the earlier port's observation that the legal screen is
  written "without GPU draws".
- **The earlier port's observations** (OBSERVED there, later confirmed on our disc, see the addendum):
  - intro video: a triangle strip sampling three **linear `k_8`** planes, Y 1280x720, U and V
    640x360, through a YUV->RGB pixel shader with bool-constant conditionals, into a
    `k_8_8_8_8_GAMMA` target, then a resolve; the census row `ps 7e80c717a36c3d9a 123 dwords,
    texture_slots 0,1,2, bool_constants 128` is that shader;
  - the composite after it: **`k_8_8_8_8` textures, formats 6 and 50, with a swizzle**;
  - the legal-screen pass: **tiled DXT1, endian 8in16, single level**;
  - menus: non-power-of-two sizes with packed mips (48x32, 12x12) - supported here for that reason.
- X `texture_cache.h:152-153`: "a 8192x sky texture is used in 4D530910" (FM4): 2D widths up to
  8192 are in the key's 13 bits.
- `vs 48b7493f34f9ef46` and `vs b3ae58af9297ee1d` in the census read texture slot 17: a **vertex**
  texture fetch exists in the first frames' shader set; the cache serves VS slots too (the binding
  contract in `shader_glsl.h` is PS and VS).

## 3. Formats and MoltenVK (claims, labelled)

| Guest format | Host image | Claim |
|---|---|---|
| 2 `k_8` | `R8_UNORM`, host swizzle RRRR | X `vulkan_texture_cache.cc:100-104`; VERIFIED against X's table |
| 6 `k_8_8_8_8`, 50 `_AS_16_16_16_16` | `R8G8B8A8_UNORM`, RGBA | X `:121-125`, `:335-339`; VERIFIED against X |
| 18 `k_DXT1`, 51 `_AS_16_16_16_16` | `BC1_RGBA_UNORM_BLOCK` (RGBA, not RGB: 1-bit punch-through) | X `:186-190`, `:340-344`; **device-dependent**, below |
| 54 `k_2_10_10_10_AS_16_16_16_16` | `A2B10G10R10_UNORM_PACK32` | X `:355-360`; VERIFIED against X (the front buffer's format, `front_buffer.h`) |
| everything else (DXT3/5, DXN, CTX1, 16-bit, float, 3D/cube/stacked, signed, YCbCr) | refused by name | by design; each a negative control in the test |

- **BC1 on Apple silicon is a runtime property, not a static fact.** MVK `MVKDevice.mm`
  `initFeatures()`: `_features.textureCompressionBC = _gpuCapabilities.supportsBCTextureCompression;`
  which is `mtlDev.supportsBCTextureCompression` behind a selector check (VERIFIED by reading the
  v1.4.2 source). Whether the test Mac (Apple M5) reports true is UNRESOLVED until a run logs
  `VkPhysicalDeviceFeatures.textureCompressionBC` and `vkGetPhysicalDeviceFormatProperties(BC1)`'s
  `SAMPLED_IMAGE | SAMPLED_IMAGE_FILTER_LINEAR`. (Apple7+ Macs are believed to support BC -
  HYPOTHESIS, not cited.) **Rule:** the renderer checks both at `Initialize`, logs them, and refuses
  DXT textures when absent (fail closed); X's fallback is a CPU/compute DXT1->RGBA8 decode
  (`vulkan_texture_cache.cc:1805-1850`, `kLoadShaderIndexDXT1ToRGBA8`), which the earlier port also
  had; that is a follow-up unit only if the check fails.
- **Image-view swizzles.** The earlier port applied texture component mappings on the CPU, reporting
  that "MoltenVK's portability subset forbids image-view swizzles; found by the validation layer".
  MVK `MVKDevice.mm` hard-codes `portabilityFeatures->imageViewFormatSwizzle = true` and sets
  `_metalFeatures.nativeTextureSwizzle = true` for `Apple1`+ and `Mac2` GPUs (VERIFIED, v1.4.2
  source). The validation complaint is therefore most likely that the feature was never *enabled*
  in `VkDeviceCreateInfo` (`VkPhysicalDevicePortabilitySubsetFeaturesKHR::imageViewFormatSwizzle`)
  - HYPOTHESIS. `TextureDescription::components` is a `VkComponentMapping` either way; the
  recommendation is to enable the feature at device creation and use view swizzles, with CPU
  permutation as the fallback. `vulkan_device.cpp` enabled the portability extension but no feature
  struct at the time (OBSERVED).
- `VK_KHR_sampler_mirror_clamp_to_edge`: MVK "Requires a Mac GPU or Apple family 7 GPU"
  (MoltenVK_Runtime_UserGuide.md); not needed: the modes that would use it (3, 5, 7) are refused.
- 16-bit formats (`VK_EXT_4444_formats` needs "16-bit formats and either native texture swizzling
  or manual swizzling", user guide) are refused here; `k_5_6_5` etc. would need Xenia's
  red/blue-swapping load shaders (`:112-118`). UNRESOLVED whether FM4's first screens need them.

## 4. Decisions

1. **Minimal subset, fail closed:** 2D (dimension 1, not stacked), the five formats, endian 0-3 for
   blocks of 4+ bytes, 0-1 for 2-byte blocks, 0 for 1-byte blocks (X's load shaders:
   `texture_load_8bpb.xesli:40-50` copies with no swap; `16bpb.xesli:41-43` `XeEndianSwap16`
   (`endian.xesli:25-31`: only 8in16 acts); `32bpb`/`64bpb.xesli:41-43` `XeEndianSwap32`). Signs
   unsigned or gamma only (gamma = all non-constant x, y, z sign 3, per `shader_glsl.h`);
   swizzle 6/7 refused (X maps them, `texture_cache.cc GuestToHostSwizzle`); clamp modes 3, 4, 5, 7
   refused where X approximates (`vulkan_texture_cache.cc:2648-2665`); YCbCr border colours refused.
2. **Level range as X (`texture_util.cc:60-92`) but refusing where X adjusts:** base page 0 (X skips
   the base), first level above 0 (X ignores the base), addresses at or past 512 MB (X keeps 17 page
   bits). A mip address of 0 means one level even with `mip_max_level` set (`:74-76`).
3. **Storage as X `GetGuestTextureLayout` for one 2D slice:** base pitch from the fetch constant, not
   re-aligned (`:306-308`, `:321-327`); mips from the next power of two, 32-block aligned, linear
   rows 256-aligned, strides 4 KB-aligned (`:310-344`); packed tail (`texture_util.h:36-39`,
   `texture_util.cc:144-181`). The watch ranges are the **data extents**, not the 4 KB strides
   (X's own 1280x720 linear `k_8_8_8_8` note, `texture_util.h:107-122`: aligning to 32 rows touches
   an unallocated page). Difference from X, documented: for a texture whose base is itself a packed
   tail (16 or fewer texels on a side) X's mips-tail extent also counts sublevel 0 (`:366-368`);
   here the range is the union of the stored levels 1.., which is what is read. HYPOTHESIS that
   this never matters (the tail is one tile either way); a 2x2 packed test can settle it.
4. **Sampler as X's `GetSamplerParameters` (`:611-690`) and `VkSamplerCreateInfo` (`:755-820`)** with
   one deliberate difference: X's Vulkan backend uses `mag_linear` for `minFilter` and `mipmapMode`
   too (`:766-772`); here min and mip are their own fields (Direct3D semantics; OBSERVED discrepancy
   in X). Base-map mip = `maxLod = minLod + 0.25` (`:804-807`); aniso `1 << (n - 1)` (`:797-802`);
   LOD bias and exp adjust go to the shader defines (`shader_glsl.h`), not the sampler ("LOD biasing
   is performed in shaders", `:796`).
5. **Untile on the CPU for M4**, X's formula transcribed with the pitch alignment (`:446`) that
   `front_buffer.h` lacked (FALSIFIED in [the front-buffer review](front_buffer.md); the regression
   rows (0, 32, 1000) -> 131072 and (999, 32, 1000) -> 258092 are in the test, and a mutant that
   removes the alignment fails exactly them). A compute untile (as in Xenia and a reference Xbox 360
   port) is a measured change later ("measure before optimizing").
6. **Write watch, not hashing:** per 4 KB guest page sequences, a global epoch, the entry's
   `loadedSequence` (taken **before** the untile reads guest memory) and `checkedEpoch`; per draw
   per slot: a hash lookup and an epoch compare; a page walk only when the epoch moved since the
   entry's last check. A CPU fault marks all four guest pages of its 16 KB host page (as in the
   reference port's write watch); a GPU write marks exactly its pages (`MarkWritten`).
7. **Reload = a new host image generation:** the old image is handed to the recording slot's
   lifetime (`GpuGuard::Hold`, destroyed after a confirmed completion); no image is written while a
   submission may read it. Staging is per slot, planned by `TextureCache::Plan` (levels tightly
   packed, 16-byte aligned offsets).
8. **Per-frame CPU-written textures (the video planes):** no special path. Bound per frame =
   ceil(bytes written / 16 KB) faults + unprotects (Y 57, U 15, V 15 = 87), three stale entries,
   1,382,400 bytes untiled and copied, 87 protects. `WriteWatch::Stats` counts it for the `[gpu]`
   line. If a later measurement shows the fault cost matters, sub-page watching (as a reference port
   offers) or a "known rewritten every frame" bypass are the options - not for M4.
9. **Storage and binding are separate:** `TextureCache::Find` keys on the storage fields only and
   calls `DescribeTexture` once per key; the swizzle/sign/clamp/filter refusals and the sampler are
   `DescribeBinding`, per draw, against the entry's host format. A fetch constant with the same
   storage and another swizzle shares the host image (tested), and a per-draw refusal (e.g. a signed
   component the shader asks for) stops the draw without poisoning the cache entry.

## 5. Write watch and `runtime/guest_memory.cpp` (coexistence)

Before integration: `OnFault` checks the address is inside the guest range, asks `DescribeAddress`,
and stops with `GuestFault`. `Heap::SetHostAccess` (`virtual_memory.cpp`) recomputes each host page's
access from the guest page protections and applies it to the A/C mirrors.
`ProtectGuestMemoryReadOnly` makes the stop-5 guard pages read-only.

Integration plan:

1. `guest_memory.h`: `using WriteWatchHook = bool (*)(uint32_t guest);` and
   `void InstallWriteWatchHook(WriteWatchHook)`; `OnFault`, after the range check and **before**
   `describeAddress`, calls the hook with the guest address when the fault is a write (macOS gives
   no read/write bit in `siginfo`; the hook decides by its own table: a watched page is read-only,
   so any fault on it is a write); `true` returns from the handler and the store re-runs. Every
   other fault takes the existing path unchanged: an unwatched page (a real guest fault, a
   data-import guard, a stop-5 guard page) still stops with its name. The hook is
   `WriteWatch::OnWriteFault` with the physical address (`runtime::PhysicalAddress`, a table
   lookup, signal-safe), on the faulting guest thread: atomics and `mprotect` only (`mprotect` is
   not in POSIX's async-signal-safe list; the reference port does the same in its handler -
   OBSERVED).
2. `WatchBackend` in the renderer: `Protect(physical, size, readOnly)` = `mprotect` of the host pages
   in **every view that maps the frame's memory**: the view holding the frame (`PhysicalToGuest`),
   and both A0000000/C0000000 when it is the shared memory; E0000000 has its own memory, so a
   texture reached through it is watched only there. `readOnly == false` restores the heap's
   computed access (`Heap::SetHostAccess` on the range), never a blind read-write.
3. `Heap::SetHostAccess` masks out `PROT_WRITE` for host pages `WriteWatch::IsWatchedHostPage` reports
   (rule 4 of `texture_cache.h`), so a neighbouring commit/protect/decommit cannot un-watch a
   texture silently. The alternative, `WriteWatch::Rearm` after every heap access change, is in the
   model too (the mask is cheaper: no extra `mprotect`). The test's negative control shows the miss:
   the fake heap drops the protection, `IsWatchedHostPage` still says watched, `Rearm` restores it.
4. Pages the stop-5 guards protect (`ProtectGuestMemoryReadOnly`) are never passed to `Watch`
   (they hold no texture); a texture that overlapped one would be refused by its address range
   (UNRESOLVED whether any guard range can overlap physical memory at all).
5. Only the guest's *writable* pages are watched: `Watch` runs after a load, which read the pages,
   and a title write to a guest-read-only page is a real guest fault, which the restored heap
   access keeps as one.
6. A fault inside `OnWriteFault` while the CP thread is in `Watch` for the same page: the CP thread
   sets `watched_` after the `mprotect`, the fault clears it after the bump; the sequence bump is a
   `fetch_add`, so a load that took `Current()` before the bump sees a higher sequence and reloads.
   The remaining race (the CP thread protects a page the title is writing between two faults) is
   the reference port's accepted behaviour: the title's write faults again and the texture reloads
   once more.

## 6. Refusals (each a negative control in the test)

fetch type not 2; shader dimension not 2D; texture dimension 1D/3D/cube; stacked; format without
a host mapping (DXT5, 5_6_5, ... all but 2, 6, 50, 18, 51, 54); integer number format; base address 0
or past 512 MB; endian without a load rule for the block size; signed or biased component; mixed
gamma in x, y, z; gamma on w; swizzle 6/7; clamp modes 3, 4, 5, 7; YCbCr border colour with a
border clamp; mag/min filter 2 or 3, mip filter 3, aniso 6 or 7 after the instruction override; mip
min level above 0; mip address past 512 MB; a level wider than its pitch; a level past the physical
memory; a packed offset inside a block (reachable only with 2x1 blocks, not for the five formats);
untile: unsupported endian, pitch not a multiple of 32 blocks, region past the pitch, short source;
cache: a refused fetch constant makes no entry, a watch the backend cannot take leaves the entry
invalid.

## 7. Mutants

Planted in a scratch copy, each caught:

| Mutant | Caught by |
|---|---|
| `TiledOffset2D` without the pitch alignment | the (0, 32, 1000) and (999, 32, 1000) rows (126976 / 253996 vs 131072 / 258092) |
| 16in32 permutation = 8in32 | "a 4-byte block under 16in32 swaps the halves" |
| mips range without the packed tail | "watch ranges: base 32,768, mips 32,768" and the 48x32 case |
| a fault bumps one guest page, not its host page's four | "all four guest pages of the host page carry sequence 1" |

Candidates for `tests/mutants.py` (critical logic: address arithmetic, bounds, the watch): these
four plus "extent uses the 4 KB stride instead of the data extent", "`EndianSupported` accepts
8in16 on 1-byte blocks", "`Loaded` records `Current()` after `Watch`", "`Valid` skips the walk
when the epoch moved".

## 8. Open questions

1. `textureCompressionBC` and BC1 format features on the test Mac (§3): one log line at
   `Initialize`.
2. Image-view swizzle under the portability subset (§3): enable the feature at `vkCreateDevice`
   and let validation confirm, or permute on the CPU.
3. Packed-base textures (<= 16 texels on a side) with mips: the union-vs-X extent difference (§4.3).
4. Vertex texture fetches (slot 17 in two census vertex shaders): same cache, VS descriptor
   bindings; nothing here is PS-specific, but the renderer's binding code must serve both.
5. Threading of `OnWriteFault` against `TextureCache::Find` on the CP thread: the model's atomics
   are per field; the renderer must not hold the cache's map across a fault (it never does: the
   fault handler touches only `WriteWatch`).

## Addendum: texture fetch constants OBSERVED on our disc

`TraceDraw` logs slots 0-2 (registers 4800 + 6*slot) for DRAW_INDX draws. Distinct values in a CP
trace of the first 400 swaps (not distributed):
- 357 UI draws (PS 8432bb77068e9a2d): slot 0 = `8A0048FE 11A8F073 0059E4FF 01280D10 00000003 00000A00`: format 0x33 = 51
  (k_DXT1_AS_16_16_16_16), 1280x720 (dword 2: width-1 4FF, height-1 2CF), base 11A8F000, tiled; slots 1-2 unset (dword 3 = 01000000).
- 38 video draws (PS 7e80c717a36c3d9a, 8_8_8_8_GAMMA target): slot 0 = `8A0248FE 13F2B0B2 0059E4FF 01000C14 00000000 00000200`:
  format 0x32 = 50 (k_8_8_8_8_AS_16_16_16_16), 1280x720; slots 1 and 2 = `06024802 0C3CA002 / 0C261002 002CE27F ...` and
  `06024802 0C40E002 / 0C2A5002 002CE27F ...`: format 2 (k_8), 640x360, two alternating bases each (double-buffered planes).
- 19+19 draws: slot 0 = `0A024802 0C2E9002 / 0C180002 0059E4FF 01281400 00000003 00000200`: k_8, 1280x720, two bases (the Y plane).

These match the formats this unit maps (k_8, 50, 51) and the earlier port's observations; nothing
else is sampled in the first 400 swaps.
