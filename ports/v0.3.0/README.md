# FM4 Recomp 0.3.0 source for Apple silicon

This directory contains the source files changed for the Mac release, **Build 9**, in full. Open a file below to read it on GitHub. Each component uses the pinned public project in the last column for its remaining files.

| Component | Release implementation | Base revision |
|---|---|---|
| Game host | [Graphics menu](host/src/ui/graphics_page.cpp), [game hooks](host/src/game_hooks.cpp), [menu input](host/src/ui/hostui/menu.cpp), [graphics tests](host/tests/graphics_page_test.cpp) | [FM4 Static Recomp at 160ef51](https://github.com/i2kiselev/fm4-static-recomp/tree/160ef51f716ebc6f90692d368d68f3a820fcecfe) |
| Rendering and audio | [Vulkan command processor](sdk/src/graphics/vulkan/command_processor.cpp), [presentation and upscaling](sdk/src/ui/vulkan/vulkan_presenter.cpp), [HDR and edge smoothing](sdk/src/graphics/vulkan/fm4_bench_output.inc), [audio driver](sdk/src/audio/sdl/sdl_audio_driver.cpp) | [ShiftGlue SDK at d16189f](https://github.com/i2kiselev/shiftglue-sdk/tree/d16189ff0488352f26b66a8fc3cbb723533fb3b6) |
| Mac launcher | [Swift app](launcher/app/setup/Main.swift), [launch decision](launcher/app/setup/LaunchDecision.swift), [game launch script](launcher/scripts/alpha/shiftglue-playtest.sh) | [FM4 Recomp at bdec538](https://github.com/retronative/fm4-recomp/tree/bdec53892484889a73fba7c0fc428626f237b2ff) |
| Packaging | [Decoder build](rebuild/build-libraries.py), [Mac app assembly](rebuild/packaging/stage_release.py), [bundle metadata](packaging/Info.plist), [graphics settings schema](packaging/kit-files/engines/shiftglue/graphics-settings.json) | [Release inputs and binary hashes](CANDIDATE-INPUTS.json) |

## Working with the source

From the root of this repository, these commands reconstruct the host worktree in a fresh directory.

```sh
release_source="$PWD/ports/v0.3.0"
git clone --no-checkout https://github.com/i2kiselev/fm4-static-recomp.git fm4-host-0.3.0
git -C fm4-host-0.3.0 checkout --detach 160ef51f716ebc6f90692d368d68f3a820fcecfe
cp -Rp "$release_source/host/." fm4-host-0.3.0/
```

For the SDK, check out its linked revision, initialize its recursive submodules, then copy the contents of `sdk/` into that checkout. For the launcher, check out the linked FM4 Recomp revision and copy the contents of `launcher/` into it. These three source directories preserve the component-relative paths. The [SDK pins](SOURCE-PINS.json) identify the dependencies and record the expected hashes of the changed SDK files.

To build the shaders, regenerate the guest code from your own supported USA Disc 1, replace the audio libraries or package the app, follow the [release build instructions](../../scripts/release/README.md). The [versioned source companion](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz) includes the upstream dependency archives, patches and complete directory layout expected by the packaging scripts.

The [host](host/LICENSE), [SDK](sdk/LICENSE) and [launcher](launcher/COPYING) retain their respective licences. The [app notices](APP-NOTICES.md) document the dependencies and the ReCarbon and SpeedBreaker shader attributions.
