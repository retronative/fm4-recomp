---
title: Status and releases
description: The single public record of FM4 Recomp's milestones and releases. FM4 Recomp 0.2.0, the second public beta, is available.
order: 3
release_headline: FM4 Recomp 0.2.0 available
release_planned: The second public beta, for Apple silicon Macs with macOS 26 or later, published 23 November 2026
release_url: https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0
release_version: v0.2.0
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
| M9 | High-resolution rendering and Extended View | Future work | Higher resolutions, extra edge smoothing and HDR are in 0.2.0; Extended View is planned for 0.2.1. |

## Releases

**FM4 Recomp 0.2.0** is the second public beta. It was published on 23 November 2026:
[download and release notes](https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0). The SHA-256 of `FM4-Recomp-0.2.0.dmg` is
`c05a16c64f81ec43de5202a07495a62101ca4a26557c4307366c407537180731`. To install it, follow [Getting started](getting-started.md). The source of its
replaceable LGPL libraries is published on the same page as `FM4-Recomp-0.2.0-third-party-source.tar.xz`
(SHA-256 `2213ede8f5036837139ceb6458e9c99fd3dc81761acb8820e0b19e8bdd2e26dc`), as the
[legal notice](LEGAL.md) describes.

The first public beta, [FM4 Recomp 0.1.0](https://github.com/retronative/fm4-recomp/releases/tag/v0.1.0), was published on 18 November 2026.

### New in 0.2.0

- Higher rendering resolutions: 720p, Full HD, QHD and 4K, with the game's own anti-aliasing
  kept. Full HD is the default
- Extra edge smoothing (Off, Soft, Standard, Strong) on top of the game's anti-aliasing; Standard
  is the default
- A GRAPHICS menu in the game, in the Pause menu and on the Pre-Race screen, for the resolution,
  edge smoothing and HDR; a choice changes at once. The same settings are in the Settings panel
  (Command-comma in the game's menus)
- HDR output (Off or Auto; Off is the default): Auto shows brighter highlights on a display that
  supports HDR and the standard picture on any other
- Controller rumble: the game's vibration reaches a gamepad that has motors
- Faster races: about 40 frames per second while driving at Full HD and about 31 at QHD, measured
  on an Apple M5 Mac in automated runs without a window
- Fixed: the sun's glare no longer covers the road when the sun is hidden, and a full Mac volume
  no longer stops the game during a save

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
