<p align="center"><img src="logo.svg" width="320" alt="FM4 Recomp"></p>

# FM4 Recomp: macOS alpha

A static recompilation of *Forza Motorsport 4* (Xbox 360, USA Disc 1) to native Apple-silicon macOS. This alpha plays the game
in a window with sound, a gamepad or the keyboard. It is an early, unoptimised build: expect a low frame rate and the known
issues below.

**You must supply your own game.** Nothing from the game is included. Setup reads your own Forza Motorsport 4 (USA) Disc 1 ISO
and checks every file it derives against the SHA-256 this build was made for. Other disc versions are refused.

## Requirements

- An Apple-silicon Mac with macOS 26 or later.
- Your own *Forza Motorsport 4* (USA) Disc 1 (Play Disc) as an ISO, and about 9 GB free disk space.
- Optional: your Disc 2 (Content Install Disc) as an ISO: about 3.2 GB for its files, and about 3 GB more in the user-data
  folder when the game installs its content. Disc 1 alone still plays. If you choose to install Disc 2 in the game before it
  is set up, FM4 Recomp's Disc 2 panel opens over the game's "Insert Disc 2" screen: drop or choose your Disc 2 ISO and
  where its files go, watch the progress (Cancel and Try Again are there), and the panel closes once Disc 2 is ready; the
  game then shows its own "Install Additional Content" screen (confirm with A). The game keeps running meanwhile, and its
  Back (B) returns to the menu.
- Setup asks where to keep the game data, and where to keep your saves and installed content: your Library by default, or
  any folder, an external SSD included (keep that drive connected to play). It checks the free space on each drive first.

Nothing else: the app includes everything it runs (SDL 3, the Vulkan loader, MoltenVK, the Vulkan validation layer, glslang,
SPIRV-Tools and the setup tools). No Homebrew, Xcode or developer tools are needed.

## Install and first run

1. Open `FM4-Recomp-<version>.dmg` and drag **FM4 Recomp** onto **Applications**.
2. Open **FM4 Recomp** from Applications. This alpha is not notarised: the first time, macOS may refuse to open it. Then open
   System Settings > Privacy & Security, choose **Open Anyway** for FM4 Recomp, and open it again.
3. The setup guides you: it checks your Mac (Apple silicon, macOS 26 or later), asks for your Disc 1 ISO (choose it or drop it on the window), then copies and
   checks the game files (a few minutes; the ISO is only read).
4. Press **Play**. Each session runs with the GPU safeguards below and ends at its time limit or when you close the game window.

Your data, saves and session logs are kept in `~/Library/Application Support/FM4 Recomp` (`data/`, `data/user-data`,
`logs/<date>/`). To remove FM4 Recomp, move the app to the Bin and delete that folder.

**Without the app** (the `.tar.gz` package): `./setup.sh [--data <dir>] [--user-data <dir>] "<Disc 1 .iso>"` once (optionally
`./setup.sh --disc2 [--dest <dir>] "<Disc 2 .iso>"` after it), then `./play.sh [minutes]`; the data stays next to the scripts (or in
`$FM4_RECOMP_HOME`) unless you choose other folders; `locations.conf` there records them.

## Controls

| Pad (SDL gamepad, Xbox layout) | Keyboard |
|---|---|
| Left stick: steer | A / D |
| Right trigger: throttle | W |
| Left trigger: brake | S |
| A / B / X | Enter / Backspace or Escape / X |
| D-pad | Arrow keys |

A skips the long welcome video before the first race. Retail FM4 cannot skip it; this build adds that.

## Performance (measured on the developers' M5, 1280x720 game resolution)

| Scene | Frame rate |
|---|---|
| Menus | about 20 fps |
| Race, in a window | about 29 fps (median frame 35 ms; 90 % of frames under 50 ms) |

Normal play runs without the Vulkan validation layer (it is in Debug mode, below), which cost about two thirds of the
renderer's CPU time.

## GPU safeguards (always on)

- One session at a time, under a hard deadline, in its own process group, with the game's own GPU fence timeouts.
- After each session the system log and crash reports are searched for a GPU hang or restart. On any evidence `play.sh`
  writes `GPU_STOP` (in the data folder above) and refuses to start again until you read it and delete it yourself. Please send
  that file and the session's `logs/` folder to the developers.
- **Debug mode** (the launcher's checkbox, or `./play.sh --debug`): the Vulkan validation layer and Metal API validation, proven
  live first by a deliberate misuse they must report; the game stops at the first validation message. Much slower; for reports.

## Known issues

- **Low frame rate in the window** (see Performance).
- **A rectangular dark shadow** sometimes surrounds the player car, in chase and bumper views. Under investigation; it is not
  caused by how render results are copied back.
- **After a finished race**, the first scene that draws a 64-bit colour target (the stop seen after confirming a car in Car
  Select) is new in this build and has only passed automated tests. If the game stops there, please send the log.
- **Unsupported features stop the game with a named message** instead of guessing: some later menus, online features, Kinect,
  and anything not reached in testing. The log names the missing piece; please send it.
- **Achievements and online** are not available.
- **Audio** can drop briefly (about 40 ms) during heavy loading.

## Before publishing

This package contains code translated from the game's executable. It is for local testing only;
it must not be published.

## Licence

GPL-3.0-or-later (see COPYING and NOTICE). Third-party notices: THIRD_PARTY_NOTICES.md and LICENSES/
