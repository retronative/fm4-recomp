---
title: Compatibility
description: Which version of Forza Motorsport 4, which Macs, which controllers and which parts of the game FM4 Recomp supports, and which it does not.
order: 4
---

# Compatibility

## Game version

| Item | Status |
|---|---|
| *Forza Motorsport 4*, Xbox 360, **USA** disc (title ID 4D530910), Disc 1 (Play Disc) | Supported, the only target |
| Disc 2 (Content Install Disc) of the same version | Optional; installed through the game's own content install |
| Other regions and revisions | Not supported: setup refuses them |
| Other *Forza* games | Not supported |

FM4 Recomp is built for one exact version of the game. Setup checks your disc image against it
and refuses anything else. You supply the disc yourself; FM4 Recomp does not include or distribute
any game data.

## Macs

| Mac | Status |
|---|---|
| Apple silicon (M-series) | Target platform |
| Intel Macs | Not supported |
| Minimum macOS version | macOS 26 (tested on macOS 27.0.1) |

The game runs in a window. It renders at 720p, Full HD (the default), QHD or 4K, chosen in the
game's GRAPHICS menu or the Settings panel, with optional HDR output on a display that supports
it and ten graphics effects. The frame rate is below 60 frames per second in races: with the usual
60 Hz pacing, races run at up to 30 frames per second; 4K is very heavy.

## Controllers

| Input | Status |
|---|---|
| Gamepads that SDL recognises on macOS (Xbox layout) | Supported; the layout is fixed. The game's vibration reaches a gamepad that has motors |
| Keyboard | Supported; the key of every action can be changed in the game. After a key press the game's prompts name your keys and show keycaps |
| Kinect | Not supported |

## The game

| Part | Status |
|---|---|
| Title screen, menus, movies, sound | Works |
| Quick Race | Works: ten Quick Races on different tracks started, left through the Pause menu's QUIT RACE and back to the menus in one automated session (the shader warm-up's route). What follows a finished race has only passed automated tests (see the known issues) |
| Saves | Stored on your Mac. Careers from FM4 Recomp 0.2.0 and 0.3.1 carry over. Compatibility with saves from a real Xbox 360 is unverified: saves made here are expected to load only here |
| Disc 2 content install | Works |
| Other modes and later menus | Not all reached in testing. An unsupported feature stops the game with a named message |
| Achievements | Not available |
| Xbox Live and online features | Not available |

The known issues of the beta are on the [Status and releases](status.md#known-issues) page.
