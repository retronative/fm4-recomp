# EDRAM follow-up: the draw extent (Xenia's DrawExtentEstimator), for the draw 27833 stop

| File | Change |
|---|---|
| `runtime/video/edram.h`, `edram.cpp` | `ExtentRegisters`, `NeedsVertexMaxY`, `EstimateMaxY`, `VertexPosition`, `VertexMaxY`, `PassThroughPositions` (pure) |
| `tests/runtime/test_edram.cpp` | `TestExtent` (49 checks; 304 in all, 255 before); the replay parses `state` lines and computes each bind's height with `EstimateMaxY`; `test_edram <cp.txt> [--vertex-max-y=N]` |

Built (clang++ C++20, `-Wall -Wextra -Werror -fsanitize=address,undefined`): **304 pass, 0 fail**.
X = xenia-canary `src/xenia/gpu`, `draw_extent_estimator.{h,cc}` and `ucode.h`.

## 1. What Xenia does (X draw_extent_estimator.cc)

`RenderTargetCache::Update` binds with `height_used = min(GetRenderTargetHeight, EstimateMaxY(true, vs))`
(render_target_cache.cc:757-767). `EstimateMaxY` (:267-366):
- starts from the window scissor's bottom (+ the window y offset unless `window_offset_disable`, at most the
  screen scissor, at least 0) (:273-287);
- **clipping on** (`PA_CL_CLIP_CNTL.clip_disable` 0): also the viewport's bottom = window offset (if
  `vtx_window_offset_enable`) + 0.5 (unless `pix_center`) + y offset (if enabled) + |y scale| (if enabled, else 1),
  truncated (:318-363). No vertex data.
- **clipping off**: only when the scissor's right *and* bottom reach 8192 (D3D9's "no scissor"; cvars
  `execute_unclipped_draw_vs_on_cpu` true and `_with_scissor` false, :24-49, :294-313), it runs **the vertex
  shader on the CPU** for every index (`ShaderInterpreter`, :169-243) and takes the maximum exported position y
  (y / w unless `vtx_xy_fmt`, × y scale + y offset if enabled; points add their radius), converts it to 16.8 fixed
  point, adds 0.5 unless `pix_center` and the window offset if enabled, and applies the top-left rule (+127 at 1x,
  +255 with MSAA, >> 8) (:199-264). Otherwise the scissor stands.

Draw 27833 (of a CP trace that stopped at the first 7e3 fast clear; not distributed): clip `00010000`
(clip_disable), scissor `20002000` (8192x8192), VTE `00000300` (vtx_xy_fmt, vtx_z_fmt; viewport scale/offset
**disabled**), vtxcntl `00000004` (pix_center 0), sumode `00010000` (window offset enabled, offset 0): exactly the
case where Xenia **needs the vertices**. Our draw path had only the scissor (draw_state.cpp `targetHeight =
min(edramRows, scissor bottom)`), i.e. Xenia's result with the estimation off.

## 2. Supplying the vertex positions without a shader interpreter

The fast clear's vertex shader is `b6cb6460159a82b1` (27 words; the `shader immediate type 0 ... b6cb6460159a82b1`
line before each clear). Hand decode with X `ucode.h` (**HYPOTHESIS until the translator's GLSL confirms it**; the
census keeps no GLSL. Its report's `vertex_fetch_slots` column reads `0` for it: that column is the *list* of slots
(`tests/shader_census/census.cpp` `Join(r.t.vertexFetchSlots)`), i.e. slot 0 = vf0, **consistent** with this decode):
- CF: `exec` at 3, 2 fetches (sequence 0b0101); `alloc`; `exec` at 5, 1 ALU; `alloc`; `exec` at 6, 1 ALU;
  `exec_end` at 7, 1 ALU (ControlFlowExecInstruction, ucode.h:218-251).
- Fetch @3 (ucode.h:738-772): vertex fetch, src r0 (the index), dst **r1**, fetch constant **0** (vf0, register
  4800), format 57 (k_32_32_32_FLOAT), dst swizzle xyz1 (`0xA88`), **stride 7 dwords**, offset 0.
- Fetch @4: mini-fetch into r0, format 38 (k_32_32_32_32_FLOAT), xyzw, offset 3 dwords (the colour).
- ALU @5: export 0 (interpolator 0) = max(r0, r0) = r0. ALU @6: export **62 (oPos)** = max(r1, r1) = r1 = (x, y,
  z, 1). ALU @7: an export with an empty write mask.

So oPos is the fetched xyz with w = 1, and with VTE `0x300` (no viewport transform, xy not divided) the screen
y is the fetched y. **The renderer already has those bytes**: the vertex window of vf0 it copies for the draw
(renderer.cpp `PlanVertexWindows`, draw_state.h `VertexSlot`: physical, size, endian). `PassThroughPositions`
reads y at dword `offset + 1` of each `stride`-dword vertex with the fetch constant's endian
(GpuSwap32 = X xenos.h:1070-1086), w = 1; `VertexMaxY` applies X :199-264; `EstimateMaxY(r, &vertexMaxY)`
takes it only when `NeedsVertexMaxY`.

**Integration (not part of this change):** in `draw_state.cpp`, `targetHeight = min(edramRows,
EstimateMaxY(extentRegisters, vertexBound))`, where `vertexBound` is supplied only when `NeedsVertexMaxY` and the
shader translator reports that the VS's oPos is a vertex fetch's xyz with w = 1 unchanged (a new, narrow
analysis on the translated shader: one vfetch into rN, oPos = rN by mov/max(rN, rN), no other write to rN) — the
fetch's slot, stride and offset come from the same analysis, the bytes from the slot's window. Any other VS
with clipping off and an 8192 scissor keeps the scissor bound (today's behaviour: it may stop as 27833 did,
fail closed). Point lists keep the scissor bound (X adds the point radius, :221-238; not modelled).

## 3. Hand-computed claims for draw 27833

Registers: surface `0A020280` (pitch 640, 4x → 16 tiles a row, host height 1024), colour `00030000` (base 0,
format 3 → storage 3), depth `00010400` (base **1024**, D24FS8). Before it: the old clear's keys own colour 0-719
(`Color(0,16,4x,2)`) and depth 720-1439 (`Depth(720,16,4x,D24FS8)`).
- **No vertex bound (the stop):** height 8192 → clamped to 1024 rows → (1024 << 1) / 16 × 16 = 2048 tiles;
  bases 0 and 1024 → distance at "64 bpp" min(1024 << 1, (2048 + 0 − 1024) << 1) = 2048 → 1024 at 32 bpp
  (render_target_cache.cc:819-857) → colour 0-1023, which meets the old depth key at tile 720 → "tile 720 would
  transfer across aspects (depth -> colour)". Reproduced in `TestExtent` (CONTROL) and by the replay.
- **Vertex bound, D3D's −0.5 convention (y up to 359.5):** 359.5 → 92032 (16.8) + 128 + 255 = 92415 >> 8 = **360
  rows** → (360 << 1) / 16 = 45 tile rows × 16 = 720 tiles → colour `min(720, 1024, 1024 − 0)` = 720: tiles
  **0-719**; depth `min(720, 1024, 2048 + 0 − 1024)` = 720: tiles **1024-1743**. Transfers: colour 0-719 from the
  old storage-2 key (same samples), depth 1024-1439 from the old depth key (same format); 1440-1743 were empty.
  (Exactly 360.0 would give 361 rows → 46 tile rows → 736 tiles: still below 1024, still no conflict.)
- **If the 7e3 pass is 512 rows** (its viewport registers say y scale −256, y offset 256 → 512, but VTE disables
  them, so only a hint): 511.5 → 512 rows → 1024 tiles → colour 0-1023 → the **same refusal**. The extent bound
  does not help then; colour overwriting the old depth's tiles needs Xenia's host-depth owners — for a clear, X
  transfers depth → colour and then clears (draws get no cutout). (That case later occurred:
  [edram_xaspect.md](edram_xaspect.md).)

## 4. Replay lines (two CP traces, not distributed)

The replay parses `state` lines (window offset, scissor TL, vtxcntl, viewport) and the draw line's clip /
sumode / vte, binds each mode-4/5 draw with `EstimateMaxY`; the traces have no vertex data, so the vertex bound is
either absent (Xenia with the estimation off) or a command-line hypothesis applied to every draw that needs it.
"Trace A" is a headless trace to swap 400; "trace B" is the trace that stopped at draw 27833.

```
trace A, no vertex bound:   21648 draws (1 unparsed lines), 2021 bound, 19145 rasterizing without targets, 48 not rasterizing, 434 resolves (434 with traced registers, 0 synthesized; 0 whose fetch constant 0 is not a 6-word vertex fetch); bind colour transfers 866 (623520 tiles), bind depth transfers 394 (283680 tiles), resolve copy transfers 1 (720 tiles), resolve clear transfers colour 0 (0 tiles) depth 0 (0 tiles); 1299 tex lines, 39 read a resolve destination, 0 mismatched; 0 refusal(s)
  extent: 21214 draw(s) with a state line, 0 bound below their scissor by EstimateMaxY, 1588 whose extent needs their vertices
trace A, --vertex-max-y=360: identical counts, 0 refusal(s); extent: 1588 bound below their scissor
trace B, no vertex bound: draw 27833: bind refused (height 8192, extent needs the vertices): tile 720 would transfer across aspects (depth -> colour): not modelled
  27834 draws (0 unparsed lines), 2879 bound, 24024 rasterizing without targets, 48 not rasterizing, 882 resolves (882 with traced registers, 0 synthesized; ...); bind colour transfers 1760 (1267200 tiles), bind depth transfers 498 (358560 tiles), resolve copy transfers 3 (2160 tiles), resolve clear transfers colour 0 (0 tiles) depth 0 (0 tiles); 2637 tex lines, 383 read a resolve destination, 0 mismatched; 1 refusal(s)
  extent: 26952 draw(s) with a state line, 0 bound below their scissor by EstimateMaxY, 2001 whose extent needs their vertices
trace B, --vertex-max-y=360: 2880 bound; bind colour transfers 1761 (1267920 tiles), bind depth transfers 499 (358976 tiles); 0 refusal(s)
  extent: 26952 draw(s) with a state line, 2001 bound below their scissor by EstimateMaxY, 2001 whose extent needs their vertices
```

Draw 27833 with 360 rows adds exactly one colour transfer of 720 tiles and one depth transfer of 416 tiles
(1024-1439): the counts' differences (1267920 − 1267200 = 720; 358976 − 358560 = 416) match §3. Clipped draws
(the UI) are never bound lower by the viewport than by their scissor in these traces ("0 bound below").

## 5. Unverified

- **The clear's vertices.** No trace logged them at the time; the 360-row result rests on D3D's −0.5 rectangle
  convention (seen for resolves in X's comment draw_util.cc:801-803, not for FM4's clears) and on the pass being
  360 rows. The viewport hint (512) points the other way. Next step: log vf0's first three vertices' xyz (3 × 7
  dwords at the fetch constant's address, 4800-4801) at clip-disabled draws with an 8192 scissor, then replay with
  the real values (the replay takes one bound for all draws; a per-draw value from the trace would be the
  extension). Done: see the addendum.
- **The VS decode** (§2) is by hand (the census agrees it fetches from slot 0 only); the ALU opcode reading
  (vector opcode 2 = max, src regs) should be confirmed against the translator's GLSL for `b6cb6460159a82b1`.
- The hypothesis run applies 360 to the old 640x360 clears too (consistent with their surface) — no new
  refusal there.
- The draw-state and renderer integration (§2) is not part of this change.

## Addendum: the clear vertices OBSERVED (a headless CP trace to swap 800, not distributed)

All 3,190 clip-disabled draws with an 8192 x 8192 scissor read vf0 (endian 2) as x {-0.5, 639.5, 639.5}, y {-0.5,
-0.5, 359.5}: max y 359.5 -> 360 rows -> 720 tiles at 640 4x. The 360-row bound is VERIFIED on our disc for every
such draw up to swap 800.
