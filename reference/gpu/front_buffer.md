# Review: front-buffer detiling (`front_buffer.h`) and the PNG writer (`png.h`)

One CPU-only test binary (ASan/UBSan); no game run, no GPU. Labels: VERIFIED / OBSERVED / HYPOTHESIS / FALSIFIED /
UNRESOLVED. There is no Xenos oracle for the tiling: "agrees with Xenia" is OBSERVED agreement with Xenia's source (a
second software implementation), not hardware proof. The PNG format has real oracles (the specifications and
published CRC-32 / Adler-32 vectors), so those rows can be VERIFIED.

## 1. The units and where they came from

| File | Source | Upstream of that |
|---|---|---|
| `runtime/video/front_buffer.h` `TiledOffset`, `TiledSize32`, `Detile32` | an earlier FM4 port attempt's texture untiler (GPL-3.0-or-later) | Xenia xenia-canary `src/xenia/gpu/texture_conversion.cc:94-110` `TiledOffset2DRow` / `TiledOffset2DColumn` (BSD-3-Clause) |
| `runtime/video/front_buffer.h` `DecodeFrontBuffer`, `SwapRedBlue1010102` | the same attempt's swap handler | fetch-constant layout `xenos.h:1206-1307` |
| `runtime/video/png.h` `EncodePngRgb8` | the same attempt's PNG writer | ISO/IEC 15948, RFC 1950, RFC 1951 |

FM4 Recomp changes (in the files' headers): namespace `runtime::video`; the decode is a pure function returning a
reason; `Detile32` takes the source size and refuses a short buffer; the PNG writer returns bytes instead of writing a
file and uses `runtime::Crc32` (`runtime/layout.h`).

Citation correction: the `front_buffer.h` header said the formula is "Xenia's texture_util TiledOffset2DOuter/Inner".
No function of that name exists in Xenia. Our `TiledOffset` is, token for token, `texture_conversion.cc:94-110`
(`TiledOffset2DRow` + `TiledOffset2DColumn`, the legacy CPU untiler), which is the row/column split of
`texture_util.cc:443-459` `GetTiledOffset2D`. OBSERVED by reading both.

Xenia sources used (fetched unmodified from xenia-canary `src/xenia/gpu/...`, SHA-256 of `texture_util.cc`
`79ded3fc...dd93`, `texture_util.h` `255787b4...1f4a`, `texture_conversion.cc` `50ce8590...5005`; not copied into the
repository): `texture_util.{h,cc}`, `texture_conversion.cc`, `shaders/texture_address.xesli`,
`shaders/texture_load.xesli`, `shaders/endian.xesli`, `vulkan/vulkan_texture_cache.cc`,
`vulkan/vulkan_command_processor.cc`, `xenos.h`.

## 2. The oracle: Xenia's `GetTiledOffset2D`

Quoted with attribution (Xenia, BSD-3-Clause), `src/xenia/gpu/texture_util.cc:443-459`:

```
443 int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch,
444                          uint32_t bytes_per_block_log2) {
445   // https://github.com/gildor2/UModel/blob/.../Unreal/UnTexture.cpp#L489
446   pitch = xe::align(pitch, xenos::kTextureTileWidthHeight);
447   // Top bits of coordinates.
448   int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5))
449                   << (bytes_per_block_log2 + 7);
450   // Lower bits of coordinates (result is 6-bit value).
451   int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
452   // Mix micro/macro + add few remaining x/y bits.
453   int32_t offset =
454       macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
455   // Mix bits again.
456   return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
457          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
458 }
```

`kTextureTileWidthHeight` = 32 (`xenos.h:1175-1176`). This is the same text as
`shaders/texture_address.xesli:15-30` `XeTextureTiledOffset2D`, which is what Xenia's Vulkan
swap path executes: `vulkan_command_processor.cc:1267-1288` `IssueSwap` ->
`vulkan_texture_cache.cc:871-896` `RequestSwapTexture` -> `LoadTextureData` (the load shaders
address tiled guest memory with that function). So the oracle is the formula Xenia actually
presents the front buffer with, not only the CPU untiler.

The oracle script transcribes lines 443-458 and `GetTiledAddressUpperBound2D` (`texture_util.cc:490-518`) only; it
was written without looking at `front_buffer.h`. Its output table is embedded in the test.

## 3. Tiling: claims

### 3.1 Expected values (hand checks of the oracle)

Rows of the table recomputed by hand from the quoted lines (4 bytes per texel, log2 = 2):

- (31, 0), pitch 32: macro 0; micro = (31 & 7) << 2 = 28; offset = ((28 & ~0xF) << 1) + (28 & 0xF)
  = 32 + 12 = 44; result = 0 + 0 + 0 + (((0 + 3) & 3) << 6) + (44 & 0x3F) = 192 + 44 = **236**.
- (0, 31), pitch 32: micro = ((31 & 0xE) << 2) << 2 = 224; offset = (224 << 1) + 0 + 16 = 464;
  result = 0 + (16 << 7) + ((464 & 0x1C0) << 2) + (((24 >> 2) & 3) << 6) + (464 & 0x3F)
  = 2048 + 1792 + 128 + 16 = **3984**.
- Upper bound of 1280x720 at pitch 1280 (`texture_util.cc:490-518`): the tile holding (1279, 719)
  starts at (1248, 704): macro = (39 + 22 * 40) << 9 = 919 * 512 = 470528, every other term 0,
  result = 470528 << 3 = 3 764 224; plus `0x400 << 2` = 4096: **3 768 320**.

All agree with the script. The full table (22 points) is embedded in the test as `kXeniaAligned` /
`kXeniaUnaligned`.

### 3.2 Results (`TestTiledOffsetAgainstXenia`)

| Claim | Evidence | Label |
|---|---|---|
| `TiledOffset(x, y, pitch, 2)` = `GetTiledOffset2D` at 20 points with pitch 32 and 1280: the four corners of the first tile, its interior (1,0) (0,1) (7,0) (8,0) (0,7) (0,8) (0,16) (8,8) (16,16), the second tile column (32,0) = 4096, the second tile row (0,32) = 163 840 = 1280*32*4, (33,1), the last column (1279,0), the last row (0,719), the last pixel (1279,719) = 3 766 140, the centre (640,360) | `pass` | OBSERVED agreement with Xenia |
| The same over whole regions: the weighted digest `sum offset*(1+x+pitch*y)` over the first tile at pitch 32 (1 430 556 672) and over rows 0-63 at pitch 1280 (689 766 811 893 760; 81 920 points, 80 tiles) | `pass` | OBSERVED agreement (the row/column split of `texture_conversion.cc` is the same function as `GetTiledOffset2D` on every point tested; the `(y & 8) << (3 + log2)` term of the split form is the `(y & 0xE)` bit 3 of the combined form) |
| `TiledSize32(1280, 720)` = 3 768 320 = Xenia `GetTiledAddressUpperBound2D(1280, 720, 1280, 2)`; `TiledSize32(32, 32)` = 4096 | `pass`; hand derivation §3.1 | OBSERVED agreement: `Detile32`'s refusal of a shorter source is exactly Xenia's extent |
| `Detile32` end to end on 1280x720: with tiled word i = i, the linear texel at each of the 7 Xenia points at pitch 1280 holds `offset / 4` (row-major output, whole texels) | `pass` | OBSERVED |
| A source one byte short of 3 768 320 is refused; the padded tile (4096 - 4) is refused | `pass` (CONTROLs) | OBSERVED |
| **Pitch not a multiple of 32.** Xenia aligns the pitch up (`xe::align`, line 446): at pitch 1000, (0,32) = 131 072 (32 tiles per row) and (999,32) = 258 092; the unit as reviewed truncated (`pitch >> 5` = 31 tiles) and gave 126 976 and 253 996; `TiledSize32(1000, 64)` = 256 000 vs Xenia's extent 262 144 | `FAIL` with the reviewed header; `pass` with the pitch-alignment fix (D1) | **FALSIFIED** for unaligned pitches (D1, §5). Unreachable from the present path: `DecodeFrontBuffer` yields `field * 32` and is `Detile32`'s only caller (`renderer.cpp`), so no presented frame was affected |
| `TiledOffset` on a 32x32 tile is a permutation of [0, 4096) in steps of 4 | `pass`; the oracle asserts the same for Xenia's function | OBSERVED (formula-independent, kept) |

Not VERIFIED anywhere: no Xenon hardware oracle; `GetTiledOffset2D` itself is Xenia's
reconstruction (its comment cites UModel's `UnTexture.cpp`).

### 3.3 The front buffer the decoder assumes

| Claim in `front_buffer.h` / `renderer.cpp` | Xenia / evidence | Label |
|---|---|---|
| Format 54 is `k_2_10_10_10_AS_16_16_16_16`; 6 is `k_8_8_8_8` | `xenos.h:498, 571` (`TextureFormat`), `:589, 608` (`ColorFormat`). The guess "54 = k_8_8_8_8 with gamma" is wrong: gamma is `ColorRenderTargetFormat::k_8_8_8_8_GAMMA` = 1 (`:301`) and `TextureFormat::k_8_8_8_8_GAMMA_EDRAM` = 62 (`:579`), never 54 | VERIFIED against Xenia's tables (FALSIFIED: "54 = 8_8_8_8 gamma") |
| Channel order of a format-54 word: R bits 0-9, G 10-19, B 20-29, A 30-31 (`Capture` in `renderer.cpp`; `SwapRedBlue1010102` exchanges bits 0-9 and 20-29; the display image is `VK_FORMAT_A2B10G10R10_UNORM_PACK32`) | `vulkan_texture_cache.cc:355-359`: 54 -> `kLoadShaderIndex32bpb` (a copy of the 32-bit word, no component rearrangement) into `VK_FORMAT_A2B10G10R10_UNORM_PACK32` with host swizzle RGBA; Vulkan defines that format with R in the low 10 bits | OBSERVED agreement with Xenia (the hardware's channel order is Xenia's reconstruction; the legal screen in an earlier FM4 port attempt's capture was recognisable, but no FM4 pixel then had red different from blue, so the R/B order was not yet observable on FM4) |
| Fetch dword 0: pitch bits 22-30 in units of 32 texels, tiled bit 31; dword 1: format 0-5, endian 6-7, base 12-31; dword 3: swizzle bits 1-12, 3 bits per component | `xenos.h:1244-1245` (`pitch : 9` at +22, `tiled : 1` at +31), `:1247-1252` (`format : 6`, `endianness : 2` at +6, `base_address : 20` at +12), `:1276` (`swizzle : 12` at +1) | VERIFIED against Xenia's struct |
| Swizzle values: XYZW 0x688, XYZ1 0xA88, ZYXW 0x60A, ZYX1 0xA0A; ZYX* = red and blue exchanged | `xenos.h:1032-1047`: 3 bits per component, X 0, Y 1, Z 2, W 3, one 5: XYZW = 0 \| 1<<3 \| 2<<6 \| 3<<9 = 0x688, XYZ1 = 0xA88, ZYXW = 2 \| 1<<3 \| 0<<6 \| 3<<9 = 0x60A, ZYX1 = 0xA0A; Xenia applies the fetch swizzle to the swap texture's image view (`vulkan_texture_cache.cc:888-889` `GuestToHostSwizzle(fetch.swizzle, ...)`), so ZYX* presents B as R: the same exchange | VERIFIED (values) / OBSERVED (Xenia applies it at present) |
| Endian 0 (`kNone`) means the 32-bit word is read from guest memory as the GPU's native little-endian word: a 4-byte `memcpy` into `uint32_t` on arm64 | `xenos.h:193-198` (`kNone` 0, `k8in16` 1, `k8in32` 2, `k16in32` 3); Xenia's load shader reads the guest bytes as little-endian uints and applies `XeEndianSwap32` per the fetch endian (`texture_load.xesli:84`, `endian.xesli:38-46`): with 0 no swap | OBSERVED agreement with Xenia |
| FM4's front buffer has endian 0 (not 2, 8in32) and is tiled | An earlier FM4 port attempt's swap handler stopped on `endian != 0` or linear and its record reports 60 swaps without a stop; here any other value stops (fail-closed, tested) | OBSERVED on that attempt's runs of the first screens; UNRESOLVED for later screens (a byte-swapped or linear front buffer would stop with its reason, never present wrongly) |
| FM4's front buffer is 1280x720 | `VdGetCurrentDisplayInformation` answers 1280x720 (`imports_video.cpp`); `VdSwap` checks the fetch size against the packet (`imports_video.cpp`); the renderer stops on any other size (`renderer.cpp`). Xenia presents any size (`IssueSwap` passes the fetch's size; the 1280, 720 at `vulkan_command_processor.cc:1294` is the display size) | OBSERVED (the earlier attempt's captures are 1280x720); fail-closed here |
| FM4 presents format 54 | The earlier attempt presented only 54 (its presenter stopped on anything else; no stop recorded). `VdSwap` accepts 6 and 54 (`imports_video.cpp`); 6 stops here | OBSERVED for the screens that attempt reached; UNRESOLVED beyond |
| Gap: Xenia refuses the swap texture unless `dimension` (dword 5 bits 9-10, `xenos.h:1302`) is `k2DOrStacked` and the base page is non-zero (`vulkan_texture_cache.cc:879-881`); `DecodeFrontBuffer` reads dwords 0, 1 and 3 only | - | OBSERVED gap, minor (a non-2D front buffer would detile as 2D). Optional: refuse `((fetch[5] >> 9) & 3) != 1` and `physical == 0` |

## 4. PNG: claims

### 4.1 Expected bytes, derived from the specifications

A separate script builds the expected files from ISO/IEC 15948 (5.2 signature, 5.3 chunk = length, type, data, CRC
over type + data; 11.2.2 IHDR; 11.2.4 IDAT; 11.2.5 IEND; 10.2 filter byte), RFC 1950 (2.2 CMF/FLG, 8.2 Adler-32) and
RFC 1951 (3.2.4 stored blocks). The writer is 8-bit **RGB** (colour type 2, no alpha), not RGBA: the input is
`width * height * 3` bytes. The 2x2 test is therefore RGB with 12 distinct samples.

- Scanlines of a 2x2 RGB image with samples 1..12 (filter None): `00 01 02 03 04 05 06 | 00 07 08
  09 0A 0B 0C` (14 bytes).
- Adler-32 by hand (RFC 1950 8.2, a starts at 1): a after each byte 1, 2, 4, 7, 11, 16, 22, 22,
  29, 37, 46, 56, 67, 79; b = their sum = 399 (0x18F); Adler-32 = 0x018F004F. Cross-checked with
  `zlib.adler32` and the published vector `adler32("Wikipedia")` = 11E60398.
- zlib header `78 01`: CM 8, CINFO 7, FLEVEL 0, no dictionary, FCHECK such that 0x7801 = 30721 =
  31 * 991 is a multiple of 31 (RFC 1950 2.2).
- One stored block, final: `01` (BFINAL 1, BTYPE 00), LEN `0E 00`, NLEN `F1 FF` (RFC 1951 3.2.4).
- IHDR data: width `00 00 00 02`, height `00 00 00 02`, depth 8, colour type 2, compression 0,
  filter 0, interlace 0 (13 bytes).
- CRC-32s: the PNG CRC is the ISO 3309 / ITU-T V.42 CRC that `zlib.crc32` implements; zlib was
  anchored on the published vectors `crc32("123456789")` = CBF43926 and `crc32("IEND")` =
  AE426082 before use (the script asserts both). IHDR FDD49A73, IDAT 9E358949, IEND AE426082.
  `zlib.crc32` is independent of `runtime::Crc32`; a 17- or 29-byte CRC-32 by hand is not
  credible, so this is a computed oracle, not a hand derivation.
- Total 8 + 25 + 37 + 12 = 82 bytes; the full sequence is in the test as `expected2x2`.
- Multi-block case 256x86 RGB: raw = 86 * (1 + 768) = 66 134 = 65 535 + 599 bytes -> two stored
  blocks: header 1 `00 FF FF 00 00` (not final, LEN 65535, NLEN 0), header 2 `01 57 02 A8 FD`
  (final, LEN 599 = 0x257, NLEN 0xFDA8); IDAT data 2 + 5 + 65535 + 5 + 599 + 4 = 66 150 bytes
  (length field `00 01 02 66`); Adler-32 78208681 (script); file 66 207 bytes. Every real
  capture (1280x720: 2 764 800 + 720 bytes = 43 blocks) goes through this split, which the 1x1
  test never exercised.

### 4.2 Results (`TestPng`)

| Claim | Evidence | Label |
|---|---|---|
| 1x1 black: 72 bytes, Adler-32 00040001, IEND CRC AE426082 (existing test) | `pass` | VERIFIED (spec + published CRC vector) |
| 2x2 RGB: the writer produces exactly the 82 bytes of §4.1 | `pass` | VERIFIED (spec-derived bytes; CRC-32 via an anchored independent implementation) |
| 256x86: two stored blocks with the headers, payloads (the raw scanlines, compared byte for byte), IDAT length and Adler-32 of §4.1; IEND at 66 195; 66 207 bytes | `pass` | VERIFIED |
| Wiring (not an oracle): Python `zlib.decompress` inflates each file's IDAT to the expected scanline bytes and every chunk CRC matches `zlib.crc32`; macOS `sips` reads `pixelWidth 2 / pixelHeight 2 / hasAlpha no / space RGB` and `256 / 86` | run by hand on the files the test leaves in its work directory | OBSERVED |
| An `rgb` shorter than `width * height * 3` | `rgb.begin() + ...` past the end: undefined behaviour (the earlier attempt's writer has the same shape); no caller passed a wrong size | OBSERVED defect by reading (D2, §5); fixed and tested |
| A 0x0 image | the block loop runs zero times: a zlib stream with no deflate block, invalid (RFC 1951 3.2.3 requires at least one block) | OBSERVED by reading (D3, §5); same fix |

## 5. Disagreements and defects found

- **D1 (unit vs Xenia, FALSIFIED for unaligned pitches).** `TiledOffset` and `TiledSize32` used
  `pitch >> 5` / `pitch`; Xenia aligns the pitch up to 32 first (`texture_util.cc:446`;
  `texture_address.xesli:19` takes `pitch_aligned`). The earlier attempt's untiler and Xenia's own
  `texture_conversion.cc:96` share the truncation, but the function Xenia presents with aligns.
  Observed: at pitch 1000 the unit gave 126 976 / 253 996 against Xenia's 131 072 / 258 092.
  Not reachable from the present path (the decoder always produces a multiple of 32). Fix:
  `TilesPerRow(pitch) = (pitch + 31) >> 5` in both functions, header citation corrected (now in
  `runtime/video/front_buffer.h`); with it every tiling check passes.
- **D2 (defect by reading).** `EncodePngRgb8` never checked `rgb.size()`: a short buffer reads
  past the end. Fix: return no bytes for a size mismatch (now in `runtime/video/png.h`). Both
  callers (`Capture` and the self-check in `renderer.cpp`) pass exactly `width * height * 3` bytes, so this closes a
  latent path only. `CaptureStore::Add` (`capture_store.h`) accepts a 0-byte record and the writer would create an
  empty file: with the fix, `Capture` should stop on `png.empty()` (no caller can reach it). Tests: two CONTROLs.
- **D3 (defect by reading).** A 0x0 image yields a zlib stream without a block. Same fix (empty
  result), same tests.
- **Citation (defect in a comment).** "TiledOffset2DOuter/Inner" does not exist in Xenia;
  corrected to `texture_conversion.cc:94-110` / `texture_util.cc:443-459`.
- **Test before fix.** The pitch-1000 expectation uses Xenia's values; on the unmodified header it failed
  (1 failure). The expected values were not changed: without the D1 fix the runner stays red by design
  (fail-closed on a FALSIFIED claim).

## 6. Test list (`tests/runtime/test_gpu_guard.cpp`, via `tests/run_renderer_guard_tests.sh`)

New or extended (the existing 55 checks unchanged):

1. `TiledOffset` = Xenia at 20 aligned points (table `kXeniaAligned`, one check; a mismatch prints the point).
2. `TiledOffset` and `TiledSize32` = Xenia at pitch 1000 (`kXeniaUnaligned`; fails without D1).
3. Weighted digest of the first tile at pitch 32 = 1 430 556 672.
4. Weighted digest of rows 0-63 at pitch 1280 = 689 766 811 893 760.
5. `TiledSize32` = Xenia's upper bound (1280x720, 32x32).
6. `Detile32` 1280x720 end to end at the 7 pitch-1280 points.
7. CONTROL: 1280x720 one byte short is refused.
8. PNG 2x2 RGB = the 82 spec-derived bytes.
9. PNG 256x86: two stored blocks, headers, payloads, Adler-32, length.
10. (with the fix) CONTROL: wrong `rgb` size refused; CONTROL: empty image refused.

Runtime of the whole binary (ASan/UBSan, `-O1`, measured once with `/usr/bin/time -p`): real 0.10 s.

Candidate mutants: the pitch-alignment term (`(pitch + 31) >> 5` -> `pitch >> 5`, killed by test 2), the bit mix
`(offset & 0x1C0) << 2` -> `<< 3` (killed by tests 1, 3, 4), the Adler modulus `65521` -> `65535` (killed by test 9:
the 2x2 and 1x1 sums never reach the modulus, the 66 134-byte case does), the stored-block final flag
`at + n == raw.size()` -> `true` (killed by test 9).

## 7. UNRESOLVED

- Xenos hardware tiling: Xenia's formula is a reconstruction; no hardware oracle exists here.
- FM4's front-buffer endian, format (54 vs 6) and size on screens beyond those first observed: each unexpected
  value stops with its reason (fail-closed), none is silently presented.
- The R/B channel order on real FM4 content: not yet observable at review time (no pixel with R != B).
- The fetch `dimension` and zero-base checks Xenia makes before a swap (§3.3 last row): not decoded here.
- Whether the capture store should treat an empty PNG (after D2/D3) as an error: the fix returns no bytes; the
  caller's handling of a 0-byte record is outside this review.
