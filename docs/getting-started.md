---
title: Getting started
description: How to install FM4 Recomp 0.3.1 on an Apple silicon Mac and play Forza Motorsport 4 natively, from your own disc.
order: 1
---

# Getting started

**FM4 Recomp 0.3.1** is the public beta released on 25 November 2026. [Download the Mac app](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1.dmg) or read the [release notes](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.1).

## What you will need

- **An Apple silicon Mac** (M-series). Intel Macs are not supported.
- **macOS 26 or later.** The beta has been tested on macOS 27.0.1.
- **Your own copy of the game:** *Forza Motorsport 4* for Xbox 360, USA version, Disc 1 (the Play
  Disc), as a disc image you made from your own disc. Other versions are refused. See
  [Compatibility](compatibility.md).
- **About 9 GB of free disk space** for the game data.
- **A gamepad or the keyboard.** On-screen prompts follow the input device you use.

The app includes its runtime, graphics and audio libraries. Homebrew and Xcode are only needed for development.

## Install

1. [Download FM4-Recomp-0.3.1.dmg](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1.dmg) and its [SHA-256 checksum](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1.dmg.sha256). In Terminal, open the folder containing both and run `shasum -a 256 -c FM4-Recomp-0.3.1.dmg.sha256`. A matching download reports `OK`.
2. Open the DMG, drag **FM4 Recomp** into **Applications**, and open the app from there.
3. The app is ad hoc signed and has not been notarised by Apple. If macOS blocks the first launch and you have verified the download, use **System Settings → Privacy & Security → Open Anyway** for FM4 Recomp.
4. For a fresh installation, choose a storage location and select the supported USA Disc 1 image when the setup window asks. Allow about 9 GB for the extracted game data. Keep any external drive containing the data connected when playing.
5. Choose **Play**. If the app opens Repair on a later launch, first check that your game data drive is mounted.

### Updating from a previous version

Quit the earlier app, connect any external drive used for game data or saves, then replace the app in **Applications** and choose **Play**. Version 0.3.1 uses the production profile folder again. It keeps your recorded storage locations and can adopt the recorded Disc 1 setup from 0.3.0 when the production profile has no setup.

On first launch, the app checks for a career in this order.

1. An existing native career in the configured production save folder takes priority.
2. If that career is missing, the launcher imports compatible saved-game packages from the earlier public 0.2.0 layout.
3. If a career is still missing, the launcher looks in the 0.3.0 or performance playtest profile. It also copies missing settings from that profile.

If you played both versions, the careers stay separate. The launcher keeps the other career at its original location. If you see a different career than expected, quit before starting a new career and keep both profiles. The old 0.3.0 account is under `~/Library/Application Support/FM4 Recomp ShiftGlue Playtest/shiftglue/user/`. Your 0.2.0 saved-game packages remain under `content/1/00000001/` in the configured save folder. Back up the whole save tree, including its headers, before moving it. If the launcher reports an unavailable folder, reconnect the drive and try again.

## Disc 2

Disc 1 is sufficient to play. Disc 2 installation on the 0.3.1 engine has not been verified. If you already installed optional content under 0.2.0, keep that installation.

## Controls

| Action | Default keyboard input |
|---|---|
| Throttle / brake | W / S |
| Steer | A / D |
| Handbrake / clutch | Space / Shift |
| Gear up / down | E / Q |
| Change camera / rewind | Tab / R |
| Look around / look straight back | Arrow keys / C |
| Pause a race | Esc |
| Move in menus | Arrow keys or W, A, S, D |
| Select / back in menus | Enter or Space / Esc or Backspace |
| Gamepad X / Y in menus | X / Y |
| Menu tabs | Q / E |
| Show or hide the FPS counter | F3 |
| Toggle fullscreen | Option+Return or F11 |

On a controller, use the D-pad in menus, A to select and B to go back. In a race use the left stick to steer and the triggers for the pedals. Open **Pause → GRAPHICS** or press **⌘,** to reach the **Graphics**, **Keyboard**, **Mouse** and **Gameplay** tabs.

### Graphics settings

Open **Pause → GRAPHICS** during a single-player race, or press **Command-comma (⌘,)**. On a controller, press **Up** from the first setting to reach the tabs. **Left** and **Right** switch tabs there. Press **Down** or **A** to enter the selected tab. In the settings list, **Up** and **Down** select a row and **Left** and **Right** change its value.

Press **Y (Save & Apply)** to use your choices. Browsing resolutions keeps the current picture on screen. The **Changing resolution…** indicator waits for a new frame at the requested size. The Graphics page stays open so you can choose when to resume.

| Setting | What it changes |
| --- | --- |
| Display Mode | Switch between fullscreen and a window. |
| Resolution | Choose the game's render size. The choices are 720p, 1080p (Full HD), 1440p (QHD) and 4K (2160p). A new profile starts at Full HD. You can choose 4K even on a smaller screen. |
| MSAA | Smooths the edges of 3D objects. Game uses 2× in races and 4× in the showroom. Off, 2× and 4× set both. Takes effect in the next scene. |
| HDR | Auto uses HDR on a compatible display. Off uses standard brightness. |
| Extended View | Matches the camera to the window shape. A wider window shows more at the sides and a taller window shows more above and below. |
| Scenery Shadows | Adds shadows for trackside cones and barriers during races. These can flicker near the edge of the screen. |
| Reflection Updates | Controls how often reflections on cars refresh. Console follows the original game. Every Race also updates them in 16-car and split-screen races. Double updates twice as many reflection faces per frame. |
| Mirror | Full refreshes the rear-view mirror every frame and can show up to 16 cars. Console shows up to six at half rate in 12- and 16-car races. |
| Soft Particles | Softens the edge of smoke and dust where they touch cars or the road. |
| Crowds | Every Race adds spectators to solo and 16-car races and uses more memory. Console follows the original game's race settings. |
| Motion Blur | Blurs the road surface as the car gains speed. |
| Depth of Field | Blurs areas outside the camera's focus in the showroom, pre-race sequences, replays and Photo Mode. Takes effect from the next screen. |
| Car Dirt | Shows dirt accumulating on the car during a race. |

**Automatic upscaling.** When the rendered picture is smaller than the area it fills in the game window, spatial FSR 1 enlarges it. At matching sizes the picture passes through at its rendered size. A larger rendered picture is scaled down. The comparison uses the visible game area in display pixels and excludes black bars. MSAA remains available at every resolution.

A new profile uses Full HD, Standard edge smoothing and HDR Auto. HDR and Extended View can be used together in fullscreen.

## Sessions and GPU safeguards

The launcher runs one session at a time and checks system diagnostics for a GPU hang or restart. If it creates `GPU_STOP` in the profile folder, read it before playing again. A session log can contain local paths. Review the text before sharing a bug report, and omit game files and captures.

The default profile is `~/Library/Application Support/FM4 Recomp/`. Use Finder’s **Go → Go to Folder** to open it.

| Inside the profile | Contents |
|---|---|
| `data/user-data/` | Default save folder. A custom or external save folder is recorded as `user_data_root` in `locations.conf`. |
| `config/graphics.toml` | Graphics settings |
| `logs/` | Session logs |

## Known issues

Frame rate and memory depend on resolution, track and Mac. Choosing 4K can substantially increase memory use. See [Status and releases](status.md#known-issues) for current limitations.

## Uninstall

Quit the app and move it from Applications to the Bin. Keep the profile and disc data if you intend to reinstall. Back up your saves before removing a profile or an external data folder. The production and old playtest folders can contain different careers.

## Building from source

Browse the [0.3.1 launcher and save migration on GitHub](https://github.com/retronative/fm4-recomp/tree/v0.3.1/scripts/alpha) or the [game host and renderer used by this release](https://github.com/retronative/fm4-recomp/tree/v0.3.1/ports/v0.3.0).

Download the [0.3.1 source archive](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1-source.tar.xz) and its [SHA-256 checksum](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1-source.tar.xz.sha256). Its README identifies the upstream host and SDK revisions, the local changes and the steps to regenerate the game code from your own supported disc. See also [Third-party notices](../THIRD_PARTY_NOTICES.md).
