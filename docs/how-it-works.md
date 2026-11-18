---
title: How it works
description: Static recompilation explained in plain terms, and why FM4 Recomp is not an emulator.
order: 2
---

# How it works

FM4 Recomp runs *Forza Motorsport 4* natively on Apple silicon by **static recompilation**: the
game's code is translated ahead of time into native arm64 code, and a native runtime takes the
place of the Xbox 360 the game expects to find.

## The short version

The original game is an Xbox 360 program: PowerPC machine code, written for the console's
operating system, its Xenos graphics chip and its audio hardware. A Mac with Apple silicon speaks
a different machine language (arm64) and has none of those. FM4 Recomp bridges both gaps:

1. **Translate the code once.** A recompiler (built on *XenonRecomp*, an MIT-licensed open-source
   project) reads the game's PowerPC machine code and turns it into C++, which is then compiled
   into ordinary native arm64 code.
2. **Replace the platform.** A native runtime provides what the game calls into: the console's
   kernel and system functions, files, saves, input, sound (the game's XMA audio is decoded with
   FFmpeg) and graphics. The game's Xenos drawing commands and shaders are translated for Vulkan,
   which runs on Metal through MoltenVK. Windows, input and audio output use *SDL 3*.
3. **Run it.** The result is a native Mac process. There is no interpreter running underneath it.

## Static recompilation vs. emulation

| | Emulator | FM4 Recomp |
|---|---|---|
| Runs the original PowerPC code | Interpreted or JIT-translated while playing | No: translated ahead of time to arm64 |
| The console's system | A whole machine imitated, for any game | Reimplemented, only what this game uses |
| Just-in-time code generation | Usually | None for the game's code |

An **emulator** imitates a whole console while the game runs, for any game. FM4 Recomp does the
translation work before you play, for this one game version, so what runs is plain arm64 code with
a purpose-built runtime.

## Fail closed, never guess

FM4 Recomp never pretends that something works. A system function it does not implement stops the
game with a message naming the function, instead of reporting a made-up success. The recompiler
fails rather than dropping code it cannot translate. That is why an unfinished part of the game
ends with a named message and a log, not with wrong behaviour.

## Why only one version

Static recompilation works on an exact binary. Every function, jump table and data structure is
mapped for one specific build of the game: the USA disc, title ID 4D530910. A different region or
revision is a different program. See [Compatibility](compatibility.md).

## Why your own disc

FM4 Recomp contains no game data: no artwork, audio, tracks, cars or disc files. Setup reads your
own disc image on your Mac and checks every file against the supported version. The released
app's program does contain code translated by machine from the game's executable; it does nothing
without the game's data from your own disc. The translated C++ itself is never committed to the
repository or published. See the [legal notice](LEGAL.md).

## More detail

The design notes behind the recompiler, the kernel functions, the GPU path and performance work
are in the [reference notes](../reference/README.md).
