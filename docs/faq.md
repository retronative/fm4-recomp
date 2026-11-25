---
title: FAQ
description: Answers to common questions about FM4 Recomp, the native Forza Motorsport 4 port for Apple silicon Macs.
order: 5
---

# Frequently asked questions

## Is FM4 Recomp an emulator?

No. An emulator imitates a console while the game runs. FM4 Recomp translates the game's PowerPC
code into native arm64 code before you play, and pairs it with a native runtime that replaces the
Xbox 360 functions the game uses. See [How it works](how-it-works.md).

## Can you give me the game, or a disc image?

No, and please don't ask. FM4 Recomp never distributes game data: no disc images, disc files,
artwork, audio, tracks or cars. You need your own legally obtained Xbox 360 copy of *Forza
Motorsport 4* (USA); setup reads a disc image you made from it.

## Is there a release?

Yes. [Download FM4 Recomp 0.3.1 for Apple silicon](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1.dmg). The public beta was released on 25 November 2026. Follow [Getting started](getting-started.md), or read the [release notes and checksums](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.1).

## Which version of the game do I need?

The Xbox 360 USA Play Disc (Disc 1), title ID 4D530910. Setup verifies the disc image. Disc 1 is sufficient, and other regions and revisions are unsupported. See [Compatibility](compatibility.md).

## How well does it run?

One QHD playtest on an Apple silicon Mac averaged about 58 rendered frames per second across 6,600 race frames outside the Graphics menu. Driving, loading and menus can perform differently. Use **F3** to show the FPS counter, or try Full HD when QHD or 4K misses your target on your Mac.

## Where are the graphics settings?

Open **Pause → GRAPHICS** in a race, or press **⌘,**. Use **Y / Save & Apply** to confirm. The [graphics guide](getting-started.md#graphics-settings) explains the thirteen controls, resolution switching and automatic FSR 1.

## What happens to my old saves?

Version 0.3.1 uses the production profile. It keeps an existing native career, then checks for a compatible 0.2.0 career and finally the 0.3.0 playtest career. Missing playtest settings are copied. If you have two different careers, both originals remain available. See [Updating from a previous version](getting-started.md#updating-from-a-previous-version) for their locations.

## Does it run on Intel Macs?

No. FM4 Recomp is built for Apple silicon and needs macOS 26 or later.

## Does it work online?

No. Xbox Live and the game's online features are not available. Achievements are
not available either.

## Can I use my Xbox 360 saves?

That is unverified. Saves made in FM4 Recomp are stored on your Mac and are expected to load only
there.

## Why did the game stop with a message?

Check the launcher message and the session log. If the launcher reports a GPU safeguard, read the `GPU_STOP` file in the [profile folder](getting-started.md#sessions-and-gpu-safeguards). Include the version, settings, steps and relevant lines in a [support report](../SUPPORT.md). Review the log for local paths and omit game files and captures.

## Is it legal?

FM4 Recomp ships no game data: you must own the game, you supply your own disc, and the game's
files are read from it on your own Mac. The released app's program does contain code translated
by machine from the game's executable, which does nothing without your disc; the
[legal notice](LEGAL.md) says exactly what the repository and the app contain. This is not legal
advice, and laws and licence terms differ by country. If you are unsure, read the game's licence
terms and seek independent advice.

## Is it open source?

FM4 Recomp’s project code is GPL-3.0-or-later. The host, SDK, decoders and other third-party components carry their respective licences. The [0.3.1 source archive](https://github.com/retronative/fm4-recomp/releases/download/v0.3.1/FM4-Recomp-0.3.1-source.tar.xz) contains the versioned source changes, decoder sources, licences and rebuild instructions. See [Third-party notices](../THIRD_PARTY_NOTICES.md).
