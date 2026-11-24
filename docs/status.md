---
title: Status and releases
description: The single public record of FM4 Recomp's milestones and releases. FM4 Recomp 0.3.0, the public beta for Apple silicon, is available.
order: 3
release_headline: FM4 Recomp 0.3.0 available
release_planned: Public beta for Apple silicon Macs with macOS 26 or later, published 25 November 2026
release_url: https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0
release_version: v0.3.0
---

# Status and releases

This page is the one public source for where FM4 Recomp stands. The website's status displays and
release page read it. Development is organised in gated milestones: a milestone passes only when
its acceptance criteria have been met and observed.

## FM4 Recomp 0.3.0

The new Mac engine supports fullscreen and windowed play, Extended View for wider and taller windows, and four render sizes from 720p to 4K. A new profile starts at Full HD with Standard smoothing and HDR Auto. **Pause → GRAPHICS** or **⌘,** opens the thirteen graphics options. Select **Save & Apply** to change resolution. Spatial FSR 1 enlarges the picture automatically when needed.

One QHD playtest on an Apple silicon Mac averaged about 58 rendered frames per second over 6,600 race frames outside the Graphics menu. Results vary with the track, settings and Mac. In a parked Bernese Alps comparison on one Mac, reducing the 4K buffer reservation lowered the game's measured memory footprint from 8.03 GiB to 5.18 GiB.

See the [graphics settings and controls](getting-started.md#graphics-settings) and [upgrade and save information](getting-started.md#updating-from-a-previous-version).

## Milestones

| ID | Milestone | State | Notes |
|---|---|---|---|
| M0 | Correct input and feasibility | Passed | |
| M1 | Toolchain, fail-closed recompile, mutation gate | Passed | |
| M2 | Native compilation and link | Passed | |
| M3 | Runtime initialization to the first guest swap | Passed | |
| M4 | First frames: GPU safety, execution model, shader census | Passed | |
| M5 | Front end: title, menus, audio, movies, input | Passed | |
| M6 | Race load and first race frame | Passed | |
| M7 | Playable racing | Passed | With documented residuals at the time: about 29 frames per second in a race on an Apple M5 Mac (0.1.0), and the race sound can start up to a second before the picture (see Known issues). |
| M8 | Polish and distribution | Passed | FM4 Recomp 0.1.0 was published on 18 November 2026. |
| M9 | High-resolution rendering and Extended View | Future work | Four resolutions, HDR and Extended View ship in 0.3.0. The broader milestone is tracked separately. |

## Releases

**[FM4 Recomp 0.3.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0)** was published on 25 November 2026. The app and its matching source have separate checksums.

| File | Download and verification |
|---|---|
| [Mac app for Apple silicon](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0.dmg) | [DMG SHA-256](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0.dmg.sha256) · `dafb8a792c72a405f71c7c886bf1246db66f405062121585eb45c791942bc967` |
| [Source archive and rebuild instructions](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz) | [Source SHA-256](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz.sha256) · `c89809d9f6baba809044c506548d42f35bd7b50283cf3193284c779b080064b8` |

Previous releases are [0.2.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0) (23 November 2026) and [0.1.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.1.0) (18 November 2026). Each release page preserves its original notes and downloads.

### In the beta

- Native Mac racing with the FM4 Static Recomp host, ShiftGlue runtime, bundled audio decoders, keyboard and controller support.
- 720p, Full HD, QHD and 4K rendering, fullscreen and windowed modes, HDR Auto, MSAA, automatic upscaling and Extended View.
- Eight optional track and camera effects, graphics settings available during a race, and D-pad navigation between settings tabs.
- Local saves and settings. Existing performance playtest profiles carry forward. See [Getting started](getting-started.md).

### Known issues

- 4K requires additional graphics time and memory. Switching resolution can pause the picture while **Changing resolution…** is shown. A stable 60 FPS is not guaranteed.
- Scenery Shadows can flicker near the edge of the screen. MSAA changes apply in the next scene and Depth of Field changes on the next screen.
- Disc 2 and save migration from the public 0.2.0 engine or an Xbox 360 have not been verified on this engine. Keep the original profile and saves.
- The app is ad hoc signed and has not been notarised. See [first launch](getting-started.md#install).

### Not part of the beta

- Xbox Live and online features
- Achievements
- Kinect
- Intel Macs

A release contains FM4 Recomp: the setup app, the runtime, open-source tools and libraries. It
contains no game data; setup reads your own disc. Its program contains code translated by machine
from the game's executable, which does nothing without your disc: the [legal notice](LEGAL.md)
says exactly what the app contains. Changes by release are in [CHANGELOG.md](../CHANGELOG.md).
