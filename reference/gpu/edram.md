# The CPU-side EDRAM render-target model

Files:

| File | Role |
|---|---|
| `runtime/video/edram.h`, `edram.cpp` | the CPU model: keys, tile math, ownership + transfers, Bind, resolve decode + plan, M4 policy |
| `runtime/video/render_targets_vk.h` | the Vulkan-side plan (header level; nothing runs on the GPU) |
| `tests/runtime/test_edram.cpp`, `tests/run_edram_tests.sh` | every expected value hand-computed from Xenia's formulas (cited per line); negative controls for every refusal |

Build wiring: `runtime/CMakeLists.txt` globs `video/*.cpp` into `fm4recomp`, so `edram.cpp` needs no
CMake edit (`edram.cpp` includes only `front_buffer.h` and the standard library). The test binary also takes a trace
path (`build/tests/edram/test_edram <cp.txt>`) to replay a CP trace through the model (§1b).

Verification (CPU only): `edram.cpp` + `test_edram.cpp` compiled with Homebrew
clang++ `-std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined`: **238 pass, 0 fail, no sanitizer
report** (204 before `TestTrace10`; one `TestTrace10` expectation was first written with z_enable and z_write_enable
swapped and corrected from X `registers.h:744-752`, not from the code). Two expectations were wrong on the first run
and corrected from Xenia's text, not from the code: the window scissor also takes the window offset
(`draw_util.cc:596-606`), and a two-target binding claims only 720 tiles, so a clear at row 2040 needs a colour-only
binding (`render_target_cache.cc:819-827`).

Citations: X = xenia-canary `src/xenia/gpu` (`render_target_cache.{h,cc}`, `draw_util.{h,cc}`, `vulkan/*`,
`shaders/edram.xesli`, `xenos.cc`, `ui/graphics_util.cc`). MVK = MoltenVK v1.4.2 sources and docs.

## 1. Trace-derived facts (early CP traces, pre-swap)

Eight early CP traces (not distributed) are identical in their render-target content (137-138 draws each; every
run stopped before the first swap, at an unimplemented import). **OBSERVED, partial frame, pre-swap**: these are
the first ~137 draws of the first frame, not a whole frame.

| Draws | RB_SURFACE_INFO | RB_COLOR_INFO | RB_DEPTH_INFO | RB_COLOR1_INFO | RB_MODECONTROL | PA_SC_WINDOW_SCISSOR_BR |
|---|---|---|---|---|---|---|
| 0-47 | `00000000` | `00000000` | `00000000` | `00000000` | 4 | `00100010` (16x16) |
| 48, 49, 98, 99 | `0A020280` | `00020000` | `000102D0` | `00000000` | 4 | `20002000` (8192x8192) |
| 50-97, 100-136 | `0A020280` | `00020000` | `000102D0` | `00000000` | 4 | `00100010` (16x16) |

Decoded (X `registers.h:618-628, 684-700, 779-789`): `0A020280` = pitch **640**, `msaa_samples` **2 (4x)**,
hiz pitch 640; `00020000` = colour base **0**, format **2 (k_2_10_10_10)**, exp bias 0; `000102D0` = depth base
**720**, format **1 (kD24FS8, 20e4)**. Pitch tiles at 32 bpp = (640 x 2 + 79) / 80 = **16**; a 360-row draw
covers 45 tile rows = **720 tiles** per aspect, so the depth base 720 is exactly the end of the colour surface
(the layout an earlier FM4 port attempt observed; the 1280x720 1x pair on the same tiles is **not in these
traces**, which end before any UI pass).

- Draws 0-47 have **pitch 0** (RB_SURFACE_INFO never written yet): X treats that as "nothing bound"
  (`render_target_cache.cc:594-598`; `draw_util.cc:35-47` IsRasterizationPotentiallyDone false), **not** a
  refusal. `edram::Bind` returns `rasterizes = false` and claims nothing (tested). The first GPU draw run must
  skip rasterization for them (with memexport still possible later: X `command_processor` note), not stop.
- Draws 48/49 and 98/99 use the 27-word vertex shader `539ea9e08aa83f0c` with an 8192x8192 scissor and the
  **same** surface/colour/depth words as the scene draws: D3D's fast-clear quad (a known Xbox 360 pattern, "EDRAM
  MSAA clears vs 1x targets", seen in a reference Xbox 360 port) binds the same Keys here, so it is an **ordinary
  draw** for the model; that port's hazard (a half-pitch 4x clear over a 1x scene) does **not** occur in these
  traces. Whether the 1x UI pass later clears through a 4x quad is UNRESOLVED.
- The other scene draws (VS `8bb7e189d92e3def` 24 words, PS `438c2af84c78a133` 9 words) have a 16x16 window
  scissor: a 16x16 region of the 640x360 4x target (UNRESOLVED what it renders; the model's `heightUsed` for
  them is 16 rows → 2 tile rows = 32 tiles claimed per aspect, lengths bounded by the distance rule).
- **No draw in copy mode (RB_MODECONTROL 6) in any trace**: no resolve, no resolve clear, no RB_COPY_* values
  observed. The resolve path is modelled from X only (HYPOTHESIS for FM4's exact registers; the accepted set is
  the FM4 set an earlier FM4 port attempt observed: colour sources 8_8_8_8 and 2_10_10_10(_AS_), destinations
  k_8_8_8_8(_AS_16_16_16_16) and k_2_10_10_10(_AS_16_16_16_16), convert, 32-bit endians, 2D).
- These early traces logged 0x2000-0x2003, 0x2208 and 0x2082; the extended trace line of §1b adds the initiator,
  RB_COLOR_MASK, RB_DEPTHCONTROL and more. **Still missing** for the resolve decode: RB_COPY_CONTROL/DEST_BASE/
  DEST_PITCH/DEST_INFO and RB_DEPTH/COLOR_CLEAR(_LO) (0x2318-0x231B, 0x231D-0x231F) at mode-6 draws,
  PA_SC_WINDOW_SCISSOR_TL and PA_SC_WINDOW_OFFSET (0x2081, 0x2080), and the resolve's vertex fetch constant 0
  with its six words.

(Superseded for the "first GPU draw run" list by §1b: that trace reaches swap 400.)

## 1b. A headless CP trace to swap 400 (27,724 lines, 21,645 draw lines; the trace extended with the draw initiator, RB_COLOR_MASK, RB_DEPTHCONTROL, blend, colour control, clip, SU mode, VTE, program)

**Replay of the whole trace through the model (OBSERVED, `test_edram <cp.txt>` replay mode):** 21,644 draws
parsed (1 truncated line skipped), 2,023 bound (the 4x clears, the UI draws), 19,139 rasterizing without a target
(the colour-mask-0 points), 48 not rasterizing (pitch 0), 434 resolves (each with the synthesized format-54
1280x720 destination), **0 refusals**; 866 colour transfers of 720 tiles each at binds (623,520 tiles) and 1 at a
resolve (draw 19504, the §2 case), **0 depth transfers**. That is the GPU side's per-frame work: about two
720-tile colour transfers a frame (4x → 1x for the UI, 1x → 4x at the next clear), plus a 1x storage-2 → storage-0
reinterpretation in the GAMMA frames.

Every draw is DRAW_INDX_2 (op 36) or DRAW_INDX (op 22) with auto indexing. Distinct configurations (count of
lines; `awk` over fields surface, color, depth, color1, mode, scissor_br, initiator, colormask, depthctl):

| Lines | Surface | Colour | Depth | Mode | Scissor | Initiator | Mask | Depthctl | What |
|---|---|---|---|---|---|---|---|---|---|
| 48 | `00000000` | 0 | 0 | 4 | 16x16 | `00010081` points | 0 | 0 | pitch 0: nothing rasterizes |
| 1,590 | `0A020280` 640 4x | `00020000` fmt 2 | `000102D0` 720 D24FS8 | 4 | 8192² | `00030088` rect list | `FFFF` | `8777` | D3D fast clear: colour 0 + depth bound; colour 1-3 (`RB_COLOR1-3_INFO` 0 = base 0) dropped as duplicates of base 0 (X `.cc:688-719`) |
| 9,648 | `0A020280` | `00020000` | `000102D0` | 4 | 16x16 | `00010081` points | 0 | 0 | NoWrite points: rasterize, bind nothing |
| 9,491 | `14000500` 1280 1x | `00020000` | `000102D0` | 4 | 16x16 | `00010081` points | 0 | 0 | the same on the 1x surface |
| 357 | `14000500` | `000A0000` fmt 10 | `000102D0` | 4 | 1280x720 | `0020008D` quad list | `F` | `00700730` | UI: colour only (z_enable, stencil clear); Key `Color(0,16,1x,storage 2)` |
| 38 | `14000500` | `000A0000` | `000102D0` | 4 | 1280x720 | `00180088` rect list | `F` | `00700730` | UI: colour only |
| 38 | `14000500` | `00010000` fmt 1 GAMMA | `000102D0` | 4 | 1280x720 | `00040086` tri strip | `F` | `00700730` | colour only; Key `Color(0,16,1x,storage 0)`: a same-aspect **format reinterpretation** of the 720 tiles (the first non-immediate shaders, `pointer` loads, just before) |
| 396 | `14000500` | `00020000` | `000102D0` | **6** | 1280x720 | `00030088` | `F` | 0 | resolve from the storage-2 1x key |
| 38 | `14000500` | `00000000` fmt 0 | `000102D0` | **6** | 1280x720 | `00030088` | `F` | 0 | resolve from the storage-0 1x key, right after each GAMMA draw |

Decoded: `14000500` = pitch 0x500 = **1280**, `msaa_samples` 0 (**1x**), hiz pitch 1280 → **16 tiles a row, the
same as the 4x surface**: the aliasing is **OBSERVED on our disc** (colour tiles 0-719 shared; the depth tiles
720-1439 are only ever bound by the 4x clears and point draws, never by the 1x UI draws, whose `RB_DEPTHCONTROL
00700730` has z_enable and stencil_enable clear). `RB_DEPTHCONTROL 8777`: stencil + z + z write, zfunc 7 (always),
stencilfunc 7, stencil zpass REPLACE (X `registers.h:744-760`): the known fast-clear state. The resolves'
`RB_COLOR_INFO` is format **2** where the draws used **10** (and 0 where they used 1): X's storage-format keying is
what makes the resolve find the tiles — the model keys both pairs to one Key each (tested, `TestTrace10`).

The resolve destination registers (RB_COPY_*) are **not in this trace**; the swap stops name the front buffer
("swap 400: front buffer 1B441000, 1280x720"; 1B441000 / 1B7D9000 alternating, fetch format 54, tiled). So the
destination is HYPOTHESIS: 1280x720, format 54 (k_2_10_10_10_AS_16_16_16_16), exp bias 0, convert — all within the
accepted set; its endian is UNRESOLVED but most likely **0**: `front_buffer.h` presents only a fetch constant with
endian 0 (OBSERVED on the legal screen), and the resolve's write must round-trip through that fetch (the model
accepts endians 0-3 either way). **Follow-up:** log 0x2318-0x231B and 0x231D-0x231F at mode-6 draws (one
`TraceDraw` line) so the resolve decode is OBSERVED before the GPU draw run; the test's resolve uses a plain
address and the same shape.

**The first GPU draw run needs exactly** (all accepted by the model, everything else refused — `TestTrace10`):
Keys `Color(0,16,4x,2)` + `Depth(720,16,4x,D24FS8)` (640x1024 host images), `Color(0,16,1x,2)` and
`Color(0,16,1x,0)` (1280x2048); render passes: 4x colour+depth (clears), 4x none-bound (points: X skips
binding; the pipeline writes nothing), 1x colour-only; transfers per frame: 4x storage-2 → 1x storage-2 (MSAA
change, 720 tiles), occasionally 1x storage-2 → 1x storage-0 (format change) and back to the 4x key; never a
depth transfer; resolves: ~1.1 per swap from the 1x colour key to the front buffer. Sequence tested in
`TestTrace10` with the ownership after each step.

## 2. Design decisions

- **Keys as Xenia's** (`render_target_cache.h:269-339`): base, pitch tiles at 32 bpp, MSAA, aspect, storage
  format; height is not in the key (an earlier FM4 port attempt keyed by draw height and failed).
  Same bit layout, so `packed` can be logged and compared with X traces.
- **Ownership as a 2048-entry array of Keys** (the representation of an earlier FM4 port attempt) with X's
  semantics (`ChangeOwnership`, `.cc:1539-1699`); transfers coalesce per adjacent source exactly as X's
  `end_tiles == it->first` rule. Each FM4 alias switch is one transfer per aspect of 720 tiles (tested).
- **Reduction: no host-depth owners** (X `.h:629-700` tracks, per tile, the last unorm24 and float24 host
  depth targets so that depth → colour → depth round trips keep float32 precision). Instead `Change` **refuses**
  a colour ↔ depth transfer and a D24S8 ↔ D24FS8 transfer. FM4's known alias is depth → depth of the same
  format on tiles 720-1439 and colour → colour on 0-719, so X's `host_depth_source == source` collapses there
  (`.cc:1609-1614`). The full logic is to be restored if a trace shows cross-aspect aliasing (depth → colour
  since added: [edram_xaspect.md](edram_xaspect.md)).
- **Reduction: no draw-extent estimator** (X `draw_extent_estimator_`, `.cc:762-767`, runs the vertex shader
  on the CPU to bound the rows touched). `Bind(…, heightUsed)` takes the rows from the caller: the window
  scissor's bottom (after the window offset) is a valid upper bound; `kHeightUnknown` is conservative. The
  consequence is tested: a colour-only draw with unknown height claims the whole EDRAM (2048 tiles) and, on a
  map where depth tiles exist, is refused (cross aspect). **The CP must pass the scissor height** (open question
  1; the estimator since added: [edram_extent.md](edram_extent.md)).
- **Resolve reads one owner after claiming the span** (`PrepareResolve`): the source Key takes the copied
  span's tiles (`Ownership::Change` row by row → `ResolvePlan.copyTransfers`), then the copy reads that one host
  target. X dumps every owner's rectangles into its EDRAM buffer and resolves from it (`.cc:1074-1159`); claiming
  the span for the source is the host-target form of the same data movement, and leaves the tiles where the next
  1x draw wants them. This replaced a first refusal ("resolve across owners") after the §1b replay found it fires
  on our disc: draws 19502-19504 are two 4x fast clears followed directly by a 1x resolve with no 1x draw between,
  so the 1x source key must take the colour tiles from the 4x key (a 4x → 1x transfer) before the copy. Unowned
  span tiles are still refused (nothing drew them; X would read its EDRAM buffer's stale words).
- **Resolve clears without the cutout** (X passes the clear rectangle as a cutout so that tiles about to be
  cleared are not transferred first, `.cc:1347-1359`, `:338-420`): the model transfers then clears — correct,
  slightly more work; the cutout is an optimisation for later.
- **Depth control normalized as X does** (`draw_util.cc:60-79`): z_enable with no z write and zfunc always
  binds no depth target; stencil as is. The trace's `8777` keeps the depth target (write bit set), `00700730`
  binds none (z and stencil clear).
- **Mode kDepth (5) binds no colour target** (X `draw_util.cc` GetNormalizedColorMask returns 0 unless
  kColorDepth), a colour target is bound for any write bit of its RB_COLOR_MASK nibble (X also drops targets
  the pixel shader never writes: the model does not know the shader, so it binds more, never fewer).
- **Bind is atomic**: the ownership map is snapshotted and restored on any refusal (X has no refusals there).
- Guest addresses are plain numbers below 0x80000000 in tests (physical resolve destinations); no title
  address anywhere.

## 3. Refusals (fail closed; each has a negative control in `test_edram.cpp`)

Bind: `msaa_samples` 3; pitch > 8192 (X `.cc:597-603`); an unknown colour format (8, 9, 11, 13); outside the M4
whitelist: 2x MSAA, 7e3 float (3/12), 16_16 / 16_16_FLOAT / 32_FLOAT (4, 6, 14), 64 bpp (5, 7, 15); a transfer
across aspects or between depth formats.

Resolve decode: RB_MODECONTROL not kCopy; copy command 2/3 (X `:773-780`) and **raw (0)** (never observed;
convert only); vertex fetch constant 0 not a 6-word vertex fetch (X `:786-791`); `msaa_samples` 3; the
rectangle beyond the surface pitch (X clamps and logs, `:874-883`: refused here); taller than 16376 (X clamps,
`:886-892`); an empty rectangle (X returns false, `:895-901`); a depth source (`copy_src_select` 4); a 3D
(array) destination; destination endian 4/5 (64/128-bit swaps); destination formats other than 6, 7, 50, 54;
number format ≠ unsigned repeating fraction; exponent bias ≠ 0; destination pitch 0; a source format outside the
whitelist or unknown.

Bind applies the whitelist after X's duplicate-base elimination (a colour target 1-3 at target 0's base is
dropped before its format is judged, as X creates only the surviving targets, `.cc:839-845`).

Resolve plan: an undecoded resolve; a span tile that no target holds; a span tile whose owner is of the other
aspect (the copy's ownership change refuses as Bind does); a kD24FS8 clear whose 20e4 value is above 1.0 (depth
bits > 0xF00000 — see open question 3: X's halved float32 representation makes this unnecessary if the viewport
math adopts it); a clear Key outside the whitelist; a cross-aspect transfer in the clear's ownership change.

## 4. MoltenVK 1.4.2 and Xenia Vulkan facts for the plan (`render_targets_vk.h`)

| Claim | Status | Evidence |
|---|---|---|
| `VK_EXT_external_memory_host` is supported | VERIFIED (docs) | MVK `Docs/MoltenVK_Runtime_UserGuide.md:232, 348` ("also supports the following Vulkan extensions") |
| Import alignment = host page size (16 KB on Apple silicon); imported memory uses the host-visible memory types | VERIFIED (source) | MVK `MVKDevice.mm:1279-1280` (`minImportedHostPointerAlignment = hostMemoryPageSize`), `:4051-4066` (`getMemoryHostPointerProperties` → `getHostVisibleMemoryTypes`) |
| The import works on the test Mac's (Apple M5) guest block | UNRESOLVED | needs a device run (log `vkGetMemoryHostPointerPropertiesEXT` and the import result); fallback in the plan §6 |
| `D24_UNORM_S8_UINT` on Apple GPUs | UNRESOLVED (expected unsupported) | MVK `MVKDevice.mm:128` gates `Depth24Unorm_Stencil8` on `isDepth24Stencil8PixelFormatSupported`; `MVKPixelFormats.mm:912` substitutes `Depth32Float_Stencil8`, `:1488` removes the caps; X queries `vkGetPhysicalDeviceFormatProperties` (`vulkan_render_target_cache.cc:266-274`) and falls back to `D32_SFLOAT_S8_UINT` (`:1581-1588`) — plan the same; log the properties in the next self-check |
| kD24FS8 (20e4) as `D32_SFLOAT_S8_UINT`, guest [0, 2) stored as [0, 1) | VERIFIED (X source) | `vulkan_render_target_cache.cc:1587`, `:5357-5359` (`Float20e4To32(v) * 0.5f` for clears); `:1935-1945` the host encoding differs for kD24FS8 always, for kD24S8 when D24 is unsupported |
| `VK_EXT_shader_stencil_export` for depth transfers | VERIFIED (docs) | MVK UserGuide `:395` (Mac GPU family 2) |
| X uses draws, not compute, for ownership transfers; integer views keep bit patterns | VERIFIED (X source) | `vulkan_render_target_cache.cc:594-700` (transfer vertex shader, pipelines), `:1628-1650` |
| X canary's host format for k_2_10_10_10 | DISCREPANCY | `:1598-1601` returns `VK_FORMAT_A8B8G8R8_UNORM_PACK32` (an 8:8:8:8 format) for 2_10_10_10 — a canary quirk; the plan uses `A2B10G10R10_UNORM_PACK32` (R in bits 0-9, the Xenos word as `front_buffer.h` decodes the 2_10_10_10 front buffer, OBSERVED on the presented legal screen) |
| Metal compute cannot write multisampled textures | UNRESOLVED here (well known; not re-verified in MVK source) | drives plan §4: transfers into 4x targets are draws |
| MoltenVK `maxFramebufferWidth/Height` ≥ 8192 (so `kMaxSurfacePitch` = 8192 is the only cap) | UNRESOLVED | query at device creation; `RenderTargetHeight` assumes a host cap ≥ 8192 as X's `GetMaxRenderTargetHeight` |

## 5. Open questions

1. **Draw height from the CP.** `Bind` needs the rows a draw can touch; the window scissor bottom (plus the
   window offset) is the cheap bound (what `loop_work.h PixelInvocations` already reads). Without it the model
   refuses colour-only draws on a map with depth tiles (tested CONTROL). Options: pass the scissor height
   always (recommended), or port X's DrawExtentEstimator later (runs the VS on the CPU for unclipped draws).
2. **MSAA representation (D-1 in `render_targets_vk.h` §2):** true 4x images (faithful; transfers 4x↔1x are
   per-sample draws) or samples-as-pixels (FM4's alias becomes a byte copy or a no-op; coverage differs from
   the console). Recommended: true 4x for M4; the other measured at M5 with the console as oracle.
3. **20e4 depth above 1.0.** The model refuses a kD24FS8 resolve clear above 1.0 (a limitation an earlier FM4
   port attempt documented). X avoids the limit by storing guest depth halved in float32 (`:5357-5359`) with the
   viewport depth scaled to match. If the viewport math adopts X's halving, drop the refusal (one line) and the
   clear is `Float20e4To32(v) * 0.5`. Until then the refusal is correct for the plan's `D32_SFLOAT_S8_UINT` with
   an unscaled viewport.
4. **Resolve across owners** happens on our disc (draw 19504 of the §1b trace) and is handled by claiming the
   span for the source Key (§2). The alternative, X's multi-owner dump (`.cc:1074-1159`) into a staging image
   without changing owners, would avoid the transfer back at the next 4x clear; the replay shows what it costs:
   each such frame moves 720 tiles 4x → 1x at the resolve and 720 back at the clear. Measure at M5, not now.
5. **The write watch** consumes `ResolveInfo.destExtentStart/Length`; until it exists the resolve's guest write
   is unguarded (the first draw run should resolve only the front buffer, which nothing else reads before
   Present's guarded read).
6. **Trace follow-up** (§1): log the copy-mode registers, RB_DEPTHCONTROL and RB_COLOR_MASK per draw, and run
   the headless CP to the first swap, so the first-frame resolve is OBSERVED before the GPU draw run.

## 6. What was taken from where

- From Xenia (design and formulas, re-derived; cited per function): everything in `edram.{h,cpp}`.
- From an earlier FM4 port attempt: only the representation of the ownership map as a 2048-entry array; its API
  (sets with two aspects) is not used because the Key model makes it unnecessary; its tests' values were
  re-derived here from X's tile math (FM4: 16 tiles a row, 45 rows, 720 + 720) and agree.
- From the repo: `front_buffer.h TiledOffset` (reviewed against X GetTiledOffset2D, [front_buffer.md](front_buffer.md))
  for the resolve destination's addresses; the test style of `tests/runtime/test_gpu_guard.cpp` and the runner
  shape of `tests/run_renderer_guard_tests.sh`.
