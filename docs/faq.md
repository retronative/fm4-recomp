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

Not yet. The first public beta, FM4 Recomp 0.1.0, is in preparation. The current state
is on the [Status and releases](status.md) page.

## Which version of the game do I need?

The Xbox 360 USA version (title ID 4D530910). Disc 1 is required; Disc 2 is optional. Other
regions and revisions are refused by setup, because static recompilation translates one exact
build of the game. See [Compatibility](compatibility.md).

## How well does it run?

It is an early, unoptimised beta: about 20 frames per second in the menus and about 29 in a
race, measured on an Apple M5 Mac at the game's 1280x720 resolution. Performance work continues.

## Does it run on Intel Macs?

No. FM4 Recomp is built for Apple silicon and needs macOS 26 or later.

## Does it work online?

No. Xbox Live and the game's online features are not available. Achievements are
not available either.

## Can I use my Xbox 360 saves?

That is unverified. Saves made in FM4 Recomp are stored on your Mac and are expected to load only
there.

## Why did the game stop with a message?

FM4 Recomp stops with a named message when the game reaches something it does not implement yet,
instead of guessing. The session's log names the missing piece. Please report that stop line and
the text log, as described in [SUPPORT.md](../SUPPORT.md). Look through the log first, and do not
attach captured frames of the game.

## Is it legal?

FM4 Recomp ships no game data: you must own the game, you supply your own disc, and the game's
files are read from it on your own Mac. The released app's program does contain code translated
by machine from the game's executable, which does nothing without your disc; the
[legal notice](LEGAL.md) says exactly what the repository and the app contain. This is not legal
advice, and laws and licence terms differ by country. If you are unsure, read the game's licence
terms and seek independent advice.

## Is it open source?

Yes. FM4 Recomp is free software under the GNU General Public License, version 3 or later. The
third-party code it uses is listed in the
[third-party notices](../THIRD_PARTY_NOTICES.md).
