---
title: Status and releases
description: The single public record of FM4 Recomp's milestones and releases. FM4 Recomp 0.4.0, a public beta for Apple silicon, is available.
order: 3
release_headline: FM4 Recomp 0.4.0 available
release_planned: Public beta for Apple silicon Macs with macOS 26 or later, published 28 November 2026
release_url: https://github.com/retronative/fm4-recomp/releases/tag/v0.4.0
release_version: v0.4.0
---

# Status and releases

This page is the one public source for where FM4 Recomp stands. The website's status displays and
release page read it. Development is organised in gated milestones: a milestone passes only when
its acceptance criteria have been met and observed.

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
| M9 | High-resolution rendering and Extended View | Future work | Higher resolutions, extra edge smoothing and HDR are in 0.2.0 and later; Extended View is not offered yet in our engine. |

## Releases

**FM4 Recomp 0.4.0** is a public beta on FM4 Recomp's own engine. It was published on 28 November 2026:
[download and release notes](https://github.com/retronative/fm4-recomp/releases/tag/v0.4.0). The DMG and the
source of its replaceable LGPL libraries (`FM4-Recomp-0.4.0-third-party-source.tar.xz`) are on that page, each
with a `.sha256` file, as the [legal notice](LEGAL.md) describes. To install it, follow
[Getting started](getting-started.md).

Earlier releases: [0.3.1](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.1) and
[0.3.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0) (25 November 2026) shipped the
fm4-static-recomp engine; [0.2.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0) (23 November 2026)
and [0.1.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.1.0) (18 November 2026) were this engine.

### New in 0.4.0

- Faster: the GPU no longer waits for the game at each of its sync points. On an Apple M5 Mac at
  QHD, in automated runs without a window and with the frame rate unlocked, a parked race scene
  rendered 14 to 24 % more frames per second than without it
- Graphics effects in the GRAPHICS menu and the Settings panel's Effects tab: Scenery Shadows,
  Reflection Updates, Mirror, Soft Particles, Crowds, Bloom, Level of Detail, Depth of Field,
  Motion Blur and Car Dirt; previewed at once, saved with A
- Keyboard prompts: after a key press the game's instructions name your keys and show keycaps in
  place of the gamepad's buttons; connecting a controller switches back at once
- Shader warm-up after setup (optional, about 45 minutes, Skip at any time): ten Quick Races played
  hidden and muted so the first races no longer pause while shaders are prepared
- Careers played in 0.3.1 are brought over the first time you play

### In the beta

- The game plays in a window with sound, from the title screen through the menus into a race
- A gamepad or the keyboard; the keyboard keys are changed in the game
- A setup app that reads your own Disc 1 image, checks every file and lets you choose where the
  game data and your saves are kept, an external drive included
- Optional Disc 2: the game's own content install, from your own Disc 2 image
- Saves, kept on your Mac
- The long welcome video before the first race can be skipped with A
- GPU safeguards that are always on, and a Debug mode for reports

### Known issues

- **The frame rate is below 60 frames per second in races**; QHD stays near 30. 4K is very heavy.
- **Without the shader warm-up**, the first races pause briefly whenever a new shader is prepared.
  Even after it, a new career's first title screen pauses once for about a second.
- **Not in this version** (they were in 0.3.1): MSAA, FSR upscaling, Display Mode, Extended View and
  the replay-camera blur setting.
- **A stutter before a race and an abrupt start**: the loading spinner on the last Pre-Race screen
  hitches once, the first second or so of the race arrives as a few very long frames, and the race
  sound can start up to a second before the picture. The cause is not known yet.
- **A rectangular dark shadow** sometimes surrounds the player car, in chase and bumper views.
- **Changing the resolution while playing** can hitch the picture for a moment. After any time at
  720p, some surfaces can stay at 720p sharpness until the next track loads.
- **HDR**: after HDR has been active in a session, switching it Off shows the standard picture
  through the same HDR-capable output until the game is restarted. With the sun in open sky the
  flare is as bright as the game asks; how the original console looked there is not known.
- **After a finished race**, the first scene that draws a 64-bit colour target (the stop seen
  after confirming a car in Car Select) has only passed automated tests. If the game stops there,
  please report it.
- **Unsupported features stop the game with a named message** instead of guessing: some later
  menus, online features, Kinect, and anything not reached in testing. The log names the missing
  piece.
- **Audio** can drop briefly (about 40 ms) during heavy loading.
- **Command-Q closes the game at once**, without asking. With the default keys, Q is the left
  shoulder button.
- The beta is not notarised: macOS blocks the first open until you choose **Open Anyway** (see
  [Getting started](getting-started.md#install)).

### Not part of the beta

- Xbox Live and online features
- Achievements
- Kinect
- Intel Macs

A release contains FM4 Recomp: the setup app, the runtime, open-source tools and libraries. It
contains no game data; setup reads your own disc. Its program contains code translated by machine
from the game's executable, which does nothing without your disc: the [legal notice](LEGAL.md)
says exactly what the app contains. Changes by release are in [CHANGELOG.md](../CHANGELOG.md).
