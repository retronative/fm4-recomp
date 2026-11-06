# Depth -> colour ownership transfer (X kDepthToColor)

No game build, no GPU. X = xenia-canary `src/xenia/gpu` (`vulkan/vulkan_render_target_cache.cc`,
`render_target_cache.cc`, `spirv_shader_translator_rb.cc`, `shaders/edram.xesli`). MVK = MoltenVK v1.4.2
(`MVKImage.mm`, `MVKPixelFormats.mm`).

## What the trace needs (OBSERVED, replay)

Draw 25343 of a CP trace with vf0 vertex lines (not distributed): surface 0A020280, colour 00030000 = 7e3 at tile 0,
depth 00010400 = D24FS8 at tile 1024, 4x fast clear, clip disabled, 8192 scissor; vf0 third vertex y = 511.5 -> 512
rows -> colour claims 1024 tiles. The replay (reading the `vf0` lines) shows the transfer it needs:
`cross-aspect draw 25343 (height 512): tiles 720-1023 from key 6082D0 to key D08000`, i.e. from the **1x** kD24FS8
key at base 720 (the front-buffer resolve 25342's depth clear claimed tiles 720-1439 for it, with RB_COPY_CONTROL
00100300) into the **4x** 7e3 colour key at base 0. **Not "4x depth, equal sample counts"** as first assumed: the
transfer is 1x depth -> 4x colour, at different bases. Replay: 25344 draws, 807 resolves, **0 refusals**, 1
depth -> colour transfer (304 tiles); an earlier trace unchanged (0 refusals, 0 cross-aspect).

## Changes

1. `edram.cpp Ownership::Change`: depth -> colour accepted (X needs no host-depth owner for a colour destination:
   `host_depth_encoding_different = dest.is_depth && ...`, render_target_cache.cc:1553-1555; transfer emitted as for
   colour, :1595-1611). Colour -> depth still refused by name ("X keeps host-depth owners for it"); no trace
   needs it up to the stop. Header comments updated.
2. `edram_vk_plan`: `TransferKind::DepthToColor`; `ClassifyTransfer` accepts depth -> colour from a **1x kD24FS8**
   source into a 1x or 4x colour key of storage 0/2/3 with the same pitch, **any bases**; refuses by name a 4x depth
   source, kD24S8, other pitches, other storage. `MapCrossAspectTexel` = X's full mapping: destination tile + tile pixel
   (:2516-2577), 1x -> 4x samples-to-pixels on the tile pixel (:2823-2828, :2857-2864), the 40-sample column swap
   (half = 80 >> (1 + source 4x), :2916-2936), source tile = destination tile + (dest base - source base) & 2047
   (:2938-2954); cross-checked by hand against edram.xesli:23-59. `Depth32To20e4` = PreClampedDepthTo20e4
   (spirv_shader_translator_rb.cc:236-326) truncating (`depth_float24_round` default false, render_target_cache.cc:46)
   and **without** X's [0, 0.5) remap (X passes `true` at :3606-3608 because X stores guest depth halved; this renderer
   does not, see DecodeDepthClear). `PackDepthStencilWord` = stencil | depth24 << 8 (:3614-3617); the colour side is the
   existing `UnpackTransferColor` (:3621-3690).
3. `edram_vk.cpp`: the colour packing helpers split out as `kTransferPacking`; a new fragment module
   `kTransferDepthToColor` (depth view at binding 0, stencil view at binding 1 of the depth target's existing set;
   `XeCrossAspectPixel`, `XeDepthTo20e4`, `XeUnpack`); push constants gain `pitchTiles` and `tileOffset`;
   `PipelineKind::DepthToColor` drawn per destination sample like a colour transfer.
4. `test_edram.cpp`: the cross-aspect refusals that are now acceptances rewritten (expected transfers by hand), a
   colour -> depth CONTROL added; the replay reads `vf0` lines (VertexMaxY via PassThroughPositions, stride 7, position at
   dword 0; the printed big-endian words byte-swapped back to the GPU's little-endian loads) and lists cross-aspect
   transfers. `test_edram_vk.cpp`: `TestDepthToColor` (classification, 11 mapping vectors incl. wrap and a CONTROL
   without the swap, 20e4 of 0, 1.0, 0.5, 0.75, 1/3 (truncated), 2^-16 (denormal), words with stencil, the 7e3 unpack).

## Stencil-aspect sampling on MoltenVK

VERIFIED (source, not run): MVKImage.mm:2131-2150 turns a STENCIL-aspect view of Depth32Float_Stencil8 into
MTLPixelFormatX32_Stencil8; MVKPixelFormats.mm:653-670 gives a combined depth/stencil image with SAMPLED usage
MTLTextureUsagePixelFormatView, which that view needs. The existing 1x -> 4x depth transfer already samples the same
stencil view. Runtime behaviour UNRESOLVED until a GPU run. X's fallback if it were missing: none needed here (X's
`kDepthToColor` always samples both aspects; its alternative is the EDRAM-buffer path, not implemented).

## Run here (CPU only)

On a copy of runtime/ with the change overlaid, replaying the trace above: test_edram 304 pass, test_edram_vk 186
pass, 0 failures, ASan/UBSan clean; replays as above; `edram_vk.cpp` `-fsyntax-only -Wall -Wextra -Werror` OK. All
six transfer shaders through glslangValidator -V + spirv-val: OK. Two test binaries were built (the replay lives in
test_edram, the plan tests in test_edram_vk).

## Unverified

Everything on the GPU (the new pipeline, the stencil view, sample masks into the 4x R16G16B16A16_SFLOAT target); that
FM4's real depth values at tiles 720-1023 matter (here they are the resolve clear's 0 / stencil 0 -> word 0 -> 7e3 0);
the replay after draw 25343 (the trace ends there); whether a later frame needs colour -> depth (the 4x D24FS8 key at
720 returning over the 7e3 tiles would): if so, that is the next stop, refused by name.
