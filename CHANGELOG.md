# Changelog

All notable changes to FM4 Recomp are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). Each published release also has its notes on
[GitHub Releases](https://github.com/retronative/fm4-recomp/releases).

## [Unreleased]

## [0.4.0] - 2026-11-28

[Release notes and checksums](https://github.com/retronative/fm4-recomp/releases/tag/v0.4.0).

FM4 Recomp's own engine again, in place of the fm4-static-recomp engine that 0.3.0 to 0.3.1 shipped, with that engine's speed
technique and the features players liked most in 0.3.1.

### Added

- More graphics settings in the GRAPHICS menu and in the Settings panel's new Effects tab: Scenery Shadows, Reflection Updates,
  Mirror, Soft Particles, Crowds, Bloom, Level of Detail, Depth of Field, Motion Blur and Car Dirt. Each starts at the game's own value; a choice is
  previewed at once and saved with A (B discards it).
- Keyboard prompts: after you press a key, the game's on-screen instructions name your keys ("Press SPACE to start"); after you
  use a gamepad, its buttons again. The button icons become keycaps of your keys (A, B, X, Y, BACK, START, the D-pad, the
  triggers and RB). Connecting a controller switches them to its buttons at once, disconnecting it back to the keys, and with no
  controller the game starts with the keys.
- Careers played in 0.3.1 are brought over the first time you play: the game uses the newest copy of each saved game,
  and the copy it replaces is kept in a saves-replaced folder beside your saves. A 0.3.0-only player opens 0.3.1 once first.
- Shader warm-up: after Disc 1 setup, FM4 Recomp can play ten Quick Races hidden and muted (about 45 minutes; Skip stops it at
  any time) so the game's shaders are ready before you race, and the first races no longer pause while they are prepared. It runs
  on a separate profile and never touches your saves. Start it again later from the launcher's Warm Up Shaders button.

### Changed

- The Mac's delete key is named Backspace in key bindings, menus and prompts.
- Faster: the GPU no longer waits for the game at each of its sync points (the technique of the fm4-static-recomp engine). On an
  Apple M5 Mac at QHD, in automated runs without a window and with the frame rate unlocked, a parked race scene rendered 14 to 24 %
  more frames per second than the same build without it, in back-to-back pairs. With the usual 60 Hz pacing, races stay at up to
  30 frames per second; the headroom shows as fewer slow frames.

## [0.3.1] - 2026-11-25

[Download the Apple silicon app](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1.dmg) · [Release notes and checksums](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.1).

- Fixed an upgrade that could open a fresh career and return players to the welcome race. The launcher now uses the production profile and the configured save location, including external storage.
- Import compatible public 0.2.0 saves on first launch, then recover a missing career or settings from 0.3.0. Existing production careers take priority and the original files stay available for recovery.
- Reuse recorded game-data locations when upgrading from a 0.3.0-only installation. See the [upgrade guide](docs/getting-started.md#updating-from-a-previous-version).

## [0.3.0] - 2026-11-25

Public beta for Apple silicon, macOS 26 or later. [Mac download](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0.dmg) · [release notes and checksums](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0).

- Switched the Mac app to the FM4 Static Recomp host and ShiftGlue runtime, with bundled audio decoders.
- Added Extended View, fullscreen and windowed play, automatic spatial FSR 1 upscaling and a graphics menu accessible from Pause or **⌘,**.
- Choose 720p, Full HD, QHD or 4K. Full HD is the default. Resolution changes on **Save & Apply** with a loading indicator. HDR Auto uses an HDR-capable display.
- Added controls for MSAA, scenery shadows, reflection updates, mirror, soft particles, crowds, motion blur, depth of field and car dirt. The D-pad navigates between settings tabs. Prompts follow the last input device.
- Reduced the 4K graphics buffer reservation. The app reuses the performance playtest profile for existing settings and saves. Public 0.2.0 saves stay in their original profile.

## [0.2.0] - 2026-11-23

The second public beta. The full notes are on the [release page](https://github.com/retronative/fm4-recomp/releases/tag/v0.2.0). The SHA-256 of
`FM4-Recomp-0.2.0.dmg` is `c05a16c64f81ec43de5202a07495a62101ca4a26557c4307366c407537180731`, and of
`FM4-Recomp-0.2.0-third-party-source.tar.xz` is `2213ede8f5036837139ceb6458e9c99fd3dc81761acb8820e0b19e8bdd2e26dc`.

### Added

- Rendering resolutions: 720p, Full HD, QHD and 4K, with the game's own anti-aliasing kept. Full HD
  is the default.
- Extra edge smoothing (Off, Soft, Standard, Strong) on top of the game's anti-aliasing. Standard
  is the default.
- A GRAPHICS menu in the game, in the Pause menu and on the Pre-Race screen, for the resolution,
  edge smoothing and HDR. A choice changes at once, the resolution included. The same settings
  are in the Settings panel (Command-comma in the game's menus), where a choice applies when
  the panel closes.
- HDR output (Off or Auto; Off is the default). Auto shows brighter highlights on a display that
  supports HDR and the standard picture on any other; the menu says which is active and why.
- Controller rumble: the game's vibration reaches a gamepad that has motors.

### Changed

- Faster races: about 40 frames per second while driving at Full HD and about 31 at QHD, measured
  on an Apple M5 Mac in automated runs without a window.

### Fixed

- The sun's glare no longer covers the road when the sun is hidden behind a building, a hill or
  the car: the flare now follows how much of the sun is visible.
- A Mac volume that is really full no longer stops the game during a save: the game gets "disk
  full" and shows its own message.

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
