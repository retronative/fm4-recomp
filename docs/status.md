---
title: Status and releases
description: The single public record of FM4 Recomp's milestones and releases. The first public beta, 0.1.0, is in preparation; no release is published yet.
order: 3
release_headline: First beta in preparation
release_planned: FM4 Recomp 0.1.0 for Apple silicon Macs, macOS 26 or later
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
| M7 | Playable racing | Under review | About 29 frames per second in a race on an Apple M5 Mac (see Known issues). |
| M8 | Polish and distribution | Under review | The beta's disk image, its fresh install and its licence audit. |
| M9 | High-resolution rendering and Extended View | Future work | After the beta. |

## Releases

**No release is published yet.** The first public beta, **FM4 Recomp 0.1.0**, is in
preparation. It will be published as a disk image on the project's
[releases page](https://github.com/retronative/fm4-recomp/releases), with its SHA-256 and release
notes. [Getting started](getting-started.md) describes how it installs.

### In the first beta

- The game plays in a window with sound, from the title screen through the menus into a race
- A gamepad or the keyboard; the keyboard keys are changed in the game
- A setup app that reads your own Disc 1 image, checks every file and lets you choose where the
  game data and your saves are kept, an external drive included
- Optional Disc 2: the game's own content install, from your own Disc 2 image
- Saves, kept on your Mac
- The long welcome video before the first race can be skipped with A
- GPU safeguards that are always on, and a Debug mode for reports

### Known issues

- **Low frame rate**: about 20 frames per second in the menus and about 29 in a race, measured on
  an Apple M5 Mac at the game's 1280x720 resolution.
- **A stutter before a race and an abrupt start**: the loading spinner on the last Pre-Race screen
  hitches once, the first second or so of the race arrives as a few very long frames, and the race
  sound can start up to a second before the picture. The cause is not known yet.
- **A rectangular dark shadow** sometimes surrounds the player car, in chase and bumper views.
- **After a finished race**, the first scene that draws a 64-bit colour target (the stop seen
  after confirming a car in Car Select) has only passed automated tests. If the game stops there,
  please report it.
- **Unsupported features stop the game with a named message** instead of guessing: some later
  menus, online features, Kinect, and anything not reached in testing. The log names the missing
  piece.
- **Audio** can drop briefly (about 40 ms) during heavy loading.
- The beta is not notarised: macOS blocks the first open until you choose **Open Anyway** (see
  [Getting started](getting-started.md#install)).

### Not part of the beta

- Xbox Live and online features
- Achievements
- Kinect
- Intel Macs
- High-resolution rendering (M9)

A release contains FM4 Recomp: the setup app, the runtime, open-source tools and libraries. It
contains no game data; setup reads your own disc. Its program contains code translated by machine
from the game's executable, which does nothing without your disc: the [legal notice](LEGAL.md)
says exactly what the app contains. Changes by release are in [CHANGELOG.md](../CHANGELOG.md).
