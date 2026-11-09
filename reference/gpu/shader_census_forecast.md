# Race-path shader forecast

Ranked; X = xenia-canary src/xenia/gpu (file:line at the revision studied).

1. Cube maps (VERIFIED present and refused): dimension-3 tfetch in 294/491 disc PS, 10 disc VS, 39/401 zip PS (ambient/specular/env/cockpit cubemap samplers). Translator already emits samplerCube; refusal texture_info.cpp:186-187. Needs CUBE_COMPATIBLE 6-layer image + per-face untile; face stride from X texture_util.cc GetGuestTextureLayout (not pinned yet; HYPOTHESIS 4 KB-aligned slice pitch).
2. k_16_16 ownership transfers (INFERRED): X uses R16G16_UINT views (vulkan_render_target_cache.cc:1631-1649); generalise the storage-14 uint view (edram_vk.cpp:433-437, 583-584, 720-722, 792-805): per-storage transfer view format, MUTABLE_FORMAT; kTransferColor XE_SOURCE_UINT .rg -> r | g<<16 (X :3545-3566), XE_DEST_UINT uvec2 (X :3692-3711); clears via the uint pass with raw halves (X :5393-5404) removing the 0x8000 refusal. Test: halves 0x1234/0xABCD -> word 0xABCD1234.
3. k_16_16 resolves (rules VERIFIED): always the full resolve; no +5 bias on Vulkan (snorm16 full range default, render_target_cache.cc:188); per sample max(-1, int16(h) * 32/32767), b=a=0 (pixel_formats.xesli:367-371); average, *2^bias, swap; dest 25 packs uint(saturate(f)*65535+0.5) (pixel_formats.xesli:144-147, 235-240). Open conflict: draw_state expects 25 as signed integer while X packs UNORM.
4. Texture gap: FormatAccepted passes 7, 22, 25, 29, 32 but HostFormatFor lacks them (25 R16G16_UNORM/SNORM; 22 depth-unorm -> R32_SFLOAT; 29/32 R16G16B16A16_SFLOAT; 7 A2B10G10R10). Likely next: DXN (49) -> BC5, DXT2_3 (19) -> BC2, DXT5A (59) -> BC4 (normal-map heavy).
5. Anisotropic filtering: every census tfetch uses aniso 7 (fetch constant); draw_state.cpp:542-543 refuses constant aniso != 0.
6. Polygon offset (decals): refused draw_state.cpp:173-175; X draw_util.cc:87-117 + vulkan_command_processor.cc:3276-3309 (constant x (2^24-1) D24S8 / 2^24 D24FS8, slope / 16, vkCmdSetDepthBias).
7. 64 bpp targets 5/7/15 (HYPOTHESIS, unobserved): MapEdramTexel needs 64bpp tile width and 32<->64 pairing (X :2538-2543, 2607-2685, 2763-2773); clears 64-bit.
8. Others likely in 3D: alpha-to-mask (foliage), point sprites, user clip planes.
(d) "vertex format 0" census refusal: a census artefact (D3D patches vfetch formats at bind; runtime VS census refused none).
