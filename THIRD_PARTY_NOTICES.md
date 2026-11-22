# Third-party notices

FM4 Recomp is licensed under GPL-3.0-or-later ([COPYING](COPYING), [NOTICE](NOTICE)). This file lists the third-party code
that FM4 Recomp contains or ships, with its copyright holders, licence, the shipped file that contains it, and where its
licence text is.

- Code taken from other projects keeps its attribution in each source file, and FM4 Recomp's changes are marked there.
- "Shipped in" paths are relative to the kit (`FM4 Recomp.app/Contents/Resources/kit/`).
- Licence texts are in the source tree under `LICENSES/`, and in the app under `licenses/LICENSES/` (the same files).
  Texts under `third-party/` are copied from each library's own source when the app is built, so they exist only in the app
  (`licenses/third-party/`).
- No game code, data or artwork is covered by these licences.

## Code inside FM4 Recomp's own programs

| Component | Copyright | Licence | Shipped in | Licence text |
|---|---|---|---|---|
| **Xenia** | Copyright (c) 2015, Ben Vanik | BSD-3-Clause | `bin/fm4recomp`: `runtime/kernel/imports_strings.cpp` (the xboxkrnl printf family, ported from `xboxkrnl_strings.cc` of xenia-canary `67f16c4e315f`). `bin/xexsurvey`: XenonRecomp's `XenonUtils/xex_patcher.cpp` and `.h` and its `xbox/*_table.inc` export tables, which carry Xenia's notice (XenonRecomp does not state their Xenia revision). Other runtime files name the Xenia file whose behaviour they re-implement; they are not copies | [LICENSES/Xenia-BSD-3-Clause.txt](LICENSES/Xenia-BSD-3-Clause.txt) |
| **XenonRecomp** (`ddd128bcca99fe8bfbb99bea583c972351fa6ace`, with the patches in `patches/xenonrecomp/`) | Copyright (c) 2025 hedge-dev and contributors | MIT | `bin/xexsurvey` (the XenonUtils XEX loader); `bin/fm4recomp` (the translated code is built with its `ppc_context.h`) | [LICENSES/XenonRecomp-MIT.md](LICENSES/XenonRecomp-MIT.md) |
| **TinySHA1** (as modified for Xenia; XenonRecomp's `thirdparty/TinySHA1`) | Copyright (c) 2012-22 Saurav Mohapatra | ISC | `bin/fm4recomp` (`runtime/kernel/stfs.cpp`, the STFS hash tree), `bin/xexsurvey`; included from XenonRecomp's tree, not copied into this repository | [LICENSES/TinySHA1-ISC.txt](LICENSES/TinySHA1-ISC.txt) |
| **SIMDe** (`a532a12ca9bbdc5e6547eb602e6256b71a5377d4`; XenonRecomp's `thirdparty/simde`) | Copyright (c) 2017 Evan Nemerson | MIT | `bin/fm4recomp` (header-only; the translated code uses it through `ppc_context.h`) | [LICENSES/SIMDe-MIT.txt](LICENSES/SIMDe-MIT.txt) |
| **tiny-AES-c** (`23856752fbd139da0b8ca6e471a13d5bcc99a08d`; XenonRecomp's `thirdparty/tiny-AES-c`) | Public domain (no copyright claimed) | Unlicense | `bin/xexsurvey` | [LICENSES/tiny-AES-c-Unlicense.txt](LICENSES/tiny-AES-c-Unlicense.txt) |

## Code from other GPL-3.0-or-later projects

These projects have the same licence as FM4 Recomp ([COPYING](COPYING)). Each file that takes code from one of them says so in
its header, with the source file and FM4 Recomp's changes.

| Project | Copyright | Licence | Where |
|---|---|---|---|
| Earlier FM4 runtime code | Copyright (C) 2026 ForzaRecomp contributors | GPL-3.0-or-later | `bin/fm4recomp`: much of `runtime/kernel/`, `runtime/gpu/`, `runtime/shader/` and the runtime's entry files; `recomp/indirect.h`; `bin/fm4_validation_control` |
| Shared runtime code and **SpeedBreaker** | Copyright (C) 2026 SpeedBreaker / ReCarbon contributors | GPL-3.0-or-later | `bin/fm4recomp`: `runtime/video/pipeline_cache_header.h`, and parts of `async_retire.h`, `gpu_read_guard.h`, `draw_record.h`, `render_scale.h`, `post_aa.h`, `zpd_report.h`, `hdr_output.h` and `runtime/kernel/input_live.h`, `rumble.h`, `extended_view.h`; `bin/xexsurvey` (`survey/xexsurvey.cpp`); `survey/xexinfo.py` and the tool setup scripts |
| **reFrozen** | Copyright (C) 2026 reFrozen contributors | GPL-3.0-or-later | The release scripts `scripts/release/make_app.sh` and `make_dmg.py` (not in the app) |

## Programs shipped beside FM4 Recomp

| Component | Copyright | Licence | Shipped in | Licence text |
|---|---|---|---|---|
| **extract-xiso** 2.7.1 (`3f5b62cfe68f000b0e3c8a30104973f3a297948e`), a separate program that reads the player's disc image | Copyright (c) 2003 in &lt;in@fishtank.com&gt; | BSD-4-Clause (the author's modified Berkeley licence) | `bin/extract-xiso` | [LICENSES/extract-xiso-BSD-4-Clause.txt](LICENSES/extract-xiso-BSD-4-Clause.txt) |

This product includes software developed by in &lt;in@fishtank.com&gt;.

## Libraries shipped in `lib/`

| Component | Copyright | Licence | Shipped in | Licence text |
|---|---|---|---|---|
| **FFmpeg** 7.1.1 (libavcodec and libavutil, built with only the `xmaframes` decoder and the WMA Pro decoder under it, with `patches/ffmpeg/0001-wmaprodec-add-xmaframes-raw-frame-decoder.patch`; that patch ports a decoder by the Xenia developers) | The FFmpeg developers (see FFmpeg's CREDITS) | LGPL-2.1-or-later (configured without GPL, version 3 or non-free parts) | `lib/libavcodec.61.dylib`, `lib/libavutil.59.dylib` (loaded by `bin/fm4recomp`; replaceable, see below) | [LICENSES/FFmpeg-LGPL-2.1.txt](LICENSES/FFmpeg-LGPL-2.1.txt); FFmpeg's notice: [LICENSES/FFmpeg-LICENSE.md](LICENSES/FFmpeg-LICENSE.md); [LICENSES/FFmpeg-CREDITS.txt](LICENSES/FFmpeg-CREDITS.txt); the build's options: [third-party/ffmpeg/build-info.txt](third-party/ffmpeg/build-info.txt) |
| **libmspack** (`305907723a4e7ab2018e58040059ffb5e77db837`, the LZX decoder `lzxd.c`; XenonRecomp's pinned `thirdparty/libmspack`) | (C) 2003-2023 Stuart Caie; contributors in its AUTHORS | LGPL-2.1 | `lib/libfm4_lzx.dylib` (loaded by `bin/fm4recomp` and `bin/xexsurvey`; replaceable, see below) | [LICENSES/libmspack-LGPL-2.1.txt](LICENSES/libmspack-LGPL-2.1.txt); [LICENSES/libmspack-lzxd-notice.txt](LICENSES/libmspack-lzxd-notice.txt); [LICENSES/libmspack-AUTHORS.txt](LICENSES/libmspack-AUTHORS.txt) |
| **SDL** 3.4.18 (`829a65d769d935c4852f8159e964312c0957260a`) | Copyright (C) 1997-2026 Sam Lantinga | Zlib | `lib/libSDL3.0.dylib` | [third-party/sdl3/LICENSE.txt](third-party/sdl3/LICENSE.txt) |
| in SDL: **miniz** v1.15 | Rich Geldreich (public domain) | Unlicense (public domain) | `lib/libSDL3.0.dylib` | [LICENSES/tiny-AES-c-Unlicense.txt](LICENSES/tiny-AES-c-Unlicense.txt) (the same Unlicense text) |
| in SDL: **HIDAPI** | Copyright (c) 2010, Alan Ott, Signal 11 Software | BSD-3-Clause (one of HIDAPI's three licences) | `lib/libSDL3.0.dylib` | [LICENSES/HIDAPI-BSD-3-Clause.txt](LICENSES/HIDAPI-BSD-3-Clause.txt); [LICENSES/HIDAPI-LICENSE.txt](LICENSES/HIDAPI-LICENSE.txt) |
| in SDL: **yuv2rgb** | Copyright (c) 2016, Adrien Descamps | BSD-3-Clause | `lib/libSDL3.0.dylib` | [LICENSES/yuv2rgb-BSD-3-Clause.txt](LICENSES/yuv2rgb-BSD-3-Clause.txt) |
| in SDL: **stb_image** | Copyright (c) 2017 Sean Barrett | MIT or public domain (Unlicense) | `lib/libSDL3.0.dylib` | [LICENSES/stb_image-MIT-or-Unlicense.txt](LICENSES/stb_image-MIT-or-Unlicense.txt) |
| **Vulkan-Loader** v1.4.363 (`9e9518d2cf00054a220ae1a40f4406804b420ef5`) | The Khronos Group Inc., Valve Corporation, LunarG, Inc. | Apache-2.0; `loader_json.c`: MIT | `lib/libvulkan.1.dylib` | [third-party/vulkan-loader/LICENSE.txt](third-party/vulkan-loader/LICENSE.txt); [LICENSES/Vulkan-Loader-loader_json-MIT.txt](LICENSES/Vulkan-Loader-loader_json-MIT.txt); the loader's own `LICENSES/` folder: [LICENSES/Vulkan-Loader/](LICENSES/Vulkan-Loader/) |
| in the loader: **cJSON** | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors | MIT | `lib/libvulkan.1.dylib` | [LICENSES/cJSON-MIT.txt](LICENSES/cJSON-MIT.txt) |
| **Vulkan-ValidationLayers** v1.4.363 (`2ea16de84194ced9f7a9fa544f185a1c2717e1b6`), used only in Debug mode | The Khronos Group Inc., Valve Corporation, LunarG, Inc. | Apache-2.0 | `lib/libVkLayer_khronos_validation.dylib` | [third-party/vulkan-validationlayers/LICENSE.txt](third-party/vulkan-validationlayers/LICENSE.txt); its own `LICENSES/` folder: [LICENSES/Vulkan-ValidationLayers/](LICENSES/Vulkan-ValidationLayers/) |
| in the validation layer: **xxHash** | Copyright (C) 2012-2023 Yann Collet | BSD-2-Clause | `lib/libVkLayer_khronos_validation.dylib` | [LICENSES/xxHash-BSD-2-Clause.txt](LICENSES/xxHash-BSD-2-Clause.txt) |
| in the validation layer: **Vulkan Memory Allocator** | Copyright (c) 2017-2024 Advanced Micro Devices, Inc. | MIT | `lib/libVkLayer_khronos_validation.dylib` | [LICENSES/VulkanMemoryAllocator-MIT.txt](LICENSES/VulkanMemoryAllocator-MIT.txt) |
| in the validation layer: **parallel-hashmap** (includes work from Abseil) | Copyright (c) 2019, Gregory Popovitch; Copyright 2018 The Abseil Authors; hash combiners Copyright 2005-2014 Daniel James (BSL-1.0) | Apache-2.0 | `lib/libVkLayer_khronos_validation.dylib` | [LICENSES/parallel-hashmap-Apache-2.0-notice.txt](LICENSES/parallel-hashmap-Apache-2.0-notice.txt); [LICENSES/Vulkan-ValidationLayers/Apache-2.0.txt](LICENSES/Vulkan-ValidationLayers/Apache-2.0.txt); [LICENSES/Vulkan-ValidationLayers/BSL-1.0.txt](LICENSES/Vulkan-ValidationLayers/BSL-1.0.txt) |
| in the validation layer: **SG14 `inplace_function`** | (the file names no copyright holder) | BSL-1.0 | `lib/libVkLayer_khronos_validation.dylib` | [LICENSES/SG14-inplace_function-BSL-1.0.txt](LICENSES/SG14-inplace_function-BSL-1.0.txt) |
| in the validation layer: **Vulkan-Utility-Libraries** v1.4.363 (`57f01541b3959f2528f769043396e882851a7e75`) | The Khronos Group Inc., Valve Corporation, LunarG, Inc. | Apache-2.0 | `lib/libVkLayer_khronos_validation.dylib` | [LICENSES/Vulkan-Utility-Libraries-LICENSE.md](LICENSES/Vulkan-Utility-Libraries-LICENSE.md); [LICENSES/Vulkan-ValidationLayers/Apache-2.0.txt](LICENSES/Vulkan-ValidationLayers/Apache-2.0.txt) |
| **glslang** 16.6.0 (`e1b562a8bed273a02f30b59b66a5d499793cede5`) | Google, Inc., The Khronos Group Inc. and others (named in its licence file) | BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 and others (listed in its licence file) | `lib/libglslang.16.dylib`, `lib/libSPIRV.16.dylib`, `lib/libglslang-default-resource-limits.16.dylib` | [third-party/glslang/LICENSE.txt](third-party/glslang/LICENSE.txt) |
| **SPIRV-Tools** vulkan-sdk-1.4.363.0 (`ef96ed763b43b59b33b31b362f09a02b729fa1c9`) | Google Inc. and others (named in its source files) | Apache-2.0 | `lib/libSPIRV-Tools.dylib`, `lib/libSPIRV-Tools-opt.dylib`; also built into `lib/libglslang.16.dylib` and `lib/libVkLayer_khronos_validation.dylib` | [third-party/spirv-tools/LICENSE](third-party/spirv-tools/LICENSE) |
| **MoltenVK** 1.4.2 (Homebrew's build), with **SPIRV-Cross** (`6c09849fe88c48eaed08413aa022aaa136a3a057`) and **SPIRV-Tools** (`0d6fd73ca73830ccab5fa1f00ed5ed40124e2c55`) | Copyright (c) 2015-2026 The Brenwill Workshop Ltd.; SPIRV-Cross and SPIRV-Tools: their authors (named in their source files) | Apache-2.0 | `lib/libMoltenVK.dylib` | [third-party/molten-vk/LICENSE](third-party/molten-vk/LICENSE) |
| in MoltenVK: **cereal** (`a56bad8bbb770ee266e930c95d37fff2a5be7fea`) | Copyright (c) 2013-2022, Randolph Voorhies, Shane Grant | BSD-3-Clause | `lib/libMoltenVK.dylib` | [LICENSES/cereal-BSD-3-Clause.txt](LICENSES/cereal-BSD-3-Clause.txt) |
| **Vulkan-Headers** v1.4.363 (`6802bb4733b63ed5efd3adb308a6c885ef180ea1`) and **SPIRV-Headers** vulkan-sdk-1.4.363.0 (`496543121ce6419f23d6fa5d7194ba66c36212d2`): headers the programs and libraries above are built with | The Khronos Group Inc. | Apache-2.0 or MIT (Vulkan-Headers); MIT, Khronos variant (SPIRV-Headers) | `bin/fm4recomp`, `bin/fm4_validation_control` and the Vulkan, glslang and SPIRV libraries | [LICENSES/Vulkan-Headers-LICENSE.md](LICENSES/Vulkan-Headers-LICENSE.md); [LICENSES/Vulkan-Headers-MIT.txt](LICENSES/Vulkan-Headers-MIT.txt); [LICENSES/SPIRV-Headers-LICENSE.txt](LICENSES/SPIRV-Headers-LICENSE.txt) |

## Source code

FM4 Recomp 0.2.0 is built from the tag `v0.2.0` of <https://github.com/retronative/fm4-recomp>. That tree holds the
complete source of FM4 Recomp and its build and release scripts: FM4 Recomp's own source for a release is the repository at
that release's tag. `scripts/setup_tools.sh` there names the exact source of XenonRecomp (`ddd128bcca99`, with our patches in
`patches/xenonrecomp/`), and `scripts/release/build_runtime_deps.sh` the sources of the libraries in `lib/`.

`bin/fm4recomp` also contains code that XenonRecomp generates from the game's executable. That generated code (`ppc/`) is
never committed to the repository or published as source: `scripts/survey.sh` regenerates it from your own Forza Motorsport 4
disc, and `scripts/build.sh` builds the program from it.

## Source code of the LGPL parts, and replacing them

FFmpeg (with the patch in `third-party/ffmpeg/`, `patches/ffmpeg/` in the repository) and libmspack's LZX decoder are under the
LGPL-2.1. They are not compiled into FM4 Recomp's programs: they are three separate libraries in the kit's `lib/`
(`libavcodec.61.dylib`, `libavutil.59.dylib`, `libfm4_lzx.dylib`), which `bin/fm4recomp` (all three) and `bin/xexsurvey` (the LZX
decoder) load by name (`@rpath/<file>`) when they start.

**Source offer.** With every binary release, FM4 Recomp publishes the complete source of these parts, at the versions in that
release, on the same GitHub Release page as the binary, for as long as the binary is offered there: for 0.2.0,
`FM4-Recomp-0.2.0-third-party-source.tar.xz`, with its SHA-256 in
`FM4-Recomp-0.2.0-third-party-source.tar.xz.sha256`. It holds FFmpeg 7.1.1
(`https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`, SHA-256 in `scripts/setup_tools.sh`), the patch and
the repository's `scripts/build_ffmpeg_xma.sh` with the options listed in `third-party/ffmpeg/build-info.txt`;
libmspack at XenonRecomp's pinned revision `305907723a4e7ab2018e58040059ffb5e77db837`; the source of the other libraries that
the repository's `scripts/release/build_runtime_deps.sh` builds, at the commits listed above, with
that script; a README with the build steps; and `SHA256SUMS`. (MoltenVK is Homebrew's build of the upstream 1.4.2 release; its
source is not in the archive.)

**Replacing a library.** You may replace any of the three with your own build, for example a modified FFmpeg. It must keep the
file name and the interface the programs use (FFmpeg 7.1's libavcodec 61 and libavutil 59 with the `xmaframes` decoder of
`patches/ffmpeg/`; libmspack's `lzxd_init`, `lzxd_set_output_length`, `lzxd_decompress`, `lzxd_free`), and its install name, `@rpath/<file>`:
`scripts/build_ffmpeg_xma.sh <ffmpeg-7.1.1.tar.xz> <prefix> <C compiler> shared` builds FFmpeg that way, and the archive's
README shows how to build the LZX decoder. Then, on your copy of the app:

1. Put your file in place of the old one: `FM4 Recomp.app/Contents/Resources/kit/lib/libavcodec.61.dylib` (or
   `libavutil.59.dylib`, `libfm4_lzx.dylib`).
2. Sign it and the app again, ad hoc: `codesign --force --sign - "FM4 Recomp.app/Contents/Resources/kit/lib/<file>"`, then
   `codesign --force --sign - "FM4 Recomp.app"`.
3. Check: `codesign --verify --deep --strict "FM4 Recomp.app"`, and
   `DYLD_PRINT_LIBRARIES=1 "FM4 Recomp.app/Contents/Resources/kit/bin/xexsurvey"` (it prints its usage and exits) lists
   `libfm4_lzx.dylib` being loaded from the kit's `lib/`. `bin/fm4recomp` checks at start-up that the libavcodec it loaded
   is this version and LGPL, and stops with a message otherwise.

Your copy then has a different signature from the released one; if macOS blocks its first launch, use **Open Anyway** in
System Settings > Privacy & Security as for the release.
