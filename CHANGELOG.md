# Changelog

All notable changes to FM4 Recomp are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). Each published release also has its notes on
[GitHub Releases](https://github.com/retronative/fm4-recomp/releases).

## [Unreleased]

## [0.1.0] - 2026-11-18

The first public beta. The full notes are on the [release page](https://github.com/retronative/fm4-recomp/releases/tag/v0.1.0). The SHA-256 of
`FM4-Recomp-0.1.0.dmg` is `b471494684c0f409875908a3bc8cfdd2aff21a1a7810e861395b42924f02dfca`.

### Added

- The game plays in a window with sound, from the title screen through the menus into a race,
  with a gamepad or the keyboard. The keyboard keys are changed in the game.
- FM4 Recomp.app: a setup app that reads your own Disc 1 image, checks every file against the
  supported version (USA, title ID 4D530910) and lets you choose where the game data and your
  saves are kept.
- Optional Disc 2: the game's own content install, from your own Disc 2 image.
- Saves, kept on your Mac. Compatibility with saves from a real Xbox 360 is unverified.
- A skips the long welcome video before the first race.
- GPU safeguards that are always on (one session at a time under a time limit, a check for GPU
  hangs after each session) and a Debug mode with the Vulkan validation layer.
- The LGPL libraries (FFmpeg's XMA decoder and libmspack's LZX decoder) are separate files in the
  app that you may replace with your own builds; their source is published beside each release.

### Known issues

- Low frame rate: about 20 frames per second in the menus and about 29 in a race, measured on an
  Apple M5 Mac.
- A stutter before a race and an abrupt start; the cause is not known yet.
- A rectangular dark shadow sometimes surrounds the player car.
- After a finished race, the first scene that draws a 64-bit colour target has only passed
  automated tests.
- Unsupported features stop the game with a named message: some later menus, online features,
  Kinect, and anything not reached in testing.
- Audio can drop briefly (about 40 ms) during heavy loading.
- Achievements and online features are not available.
