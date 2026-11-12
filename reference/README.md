# Reference notes

Durable technical notes behind FM4 Recomp's design. Code comments link here when a decision needs
more than a few lines. Labels: VERIFIED (read in code or a specification), OBSERVED (seen in a run
or trace of the game), HYPOTHESIS, FALSIFIED, UNRESOLVED. Traces, logs and other run captures are
local and not distributed: they hold data derived from the game.

## Notes

| Area | Note | What it covers |
|---|---|---|
| Recompiler | [pipeline_map.md](recompiler/pipeline_map.md) | The survey and recompile pipeline, its checks and log markers |
| Recompiler | [unproven_switches.md](recompiler/unproven_switches.md) | Switch dispatches without a proven bound, the first diagnostic survey |
| Recompiler | [unproven_switch_c.md](recompiler/unproven_switch_c.md), [unproven_switch_d.md](recompiler/unproven_switch_d.md) | The two bounded analyses of the 0x82ADA478 dispatch |
| Kernel | [xam_ordinals.md](kernel/xam_ordinals.md) | xam ordinals FM4 resolves at run time |
| Kernel | [audio.md](kernel/audio.md) | The audio render client and output |
| Kernel | [offline_sockets.md](kernel/offline_sockets.md) | Runtime-local sockets: what FM4's callers need |
| GPU | [gpu_path.md](gpu/gpu_path.md) | VdInitializeEngines to the first VdSwap |
| GPU | [edram.md](gpu/edram.md), [edram_extent.md](gpu/edram_extent.md), [edram_xaspect.md](gpu/edram_xaspect.md) | The EDRAM render-target model, the draw extent, depth/colour transfers |
| GPU | [texture.md](gpu/texture.md) | The texture cache's CPU side and the fetch constants FM4 uses |
| GPU | [front_buffer.md](gpu/front_buffer.md) | Front-buffer detiling and the PNG writer |
| GPU | [shader_census_forecast.md](gpu/shader_census_forecast.md) | What the disc's shaders need before the first race |
| Performance | [draw_cost.md](perf/draw_cost.md) | Command-processor cost per draw |
| Performance | [memory_waits.md](perf/memory_waits.md) | FM4's GPU memory waits and the race frame |

## Sources FM4 copies or adapts code from

Citations of the form `file:line` refer to these revisions unless a citation names another.
Licences and notices: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), [LICENSES](../LICENSES).

| Project | Revision | Licence | Used for |
|---|---|---|---|
| [Xenia canary](https://github.com/xenia-canary/xenia-canary) | 67f16c4e315f (some citations: d119505, 838850fed838, named in place) | BSD-3-Clause | Kernel, XAM and GPU semantics; the xboxkrnl printf family is ported from it |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | ddd128bcca99fe8bfbb99bea583c972351fa6ace | MIT | The recompiler, with the patches in `patches/xenonrecomp/` |
| [SpeedBreaker](https://github.com/SpeedBreakerProject/speedbreaker) | 312ce69 | GPL-3.0 | The POD pipeline key (`runtime/video/draw_record.h`) |
| ShiftGlue SDK (Xenia-derived) | 72b7dd6f | BSD-style | Semantics and techniques re-implemented here and cited in place (EDRAM pack/decode rules, depth-overwrite claim, clear absorption); no code copied |
| TinySHA1 (as modified for Xenia, in XenonRecomp) | XenonRecomp's pin | ISC-style | The STFS hash tree (included, not copied) |
| [FFmpeg](https://ffmpeg.org) | 7.1.1 | LGPL-2.1 | The XMA decoder (`patches/ffmpeg/`, built by `scripts/build_ffmpeg_xma.sh`) |

Code taken from earlier ports keeps its attribution and licence in each file's header.
