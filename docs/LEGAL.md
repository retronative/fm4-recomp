# Legal notice and contents

FM4 Recomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
Microsoft, Xbox Game Studios or Turn 10 Studios. "Forza", "Forza Motorsport" and "Xbox 360" are
trademarks of Microsoft Corporation; all other trademarks belong to their respective owners.

To use FM4 Recomp you need your own legally obtained Xbox 360 copy of *Forza Motorsport 4* (USA,
title ID 4D530910). Please do not request or share game files or disc images.

## What this repository contains

Source code, build scripts, tests and documentation, under the GPL-3.0-or-later
([COPYING](../COPYING), [NOTICE](../NOTICE), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

It contains **no game assets** (textures, models, audio, movies or disc files), **no disc images**,
**no game executables** (original or decrypted) and **no recompiled game code**. The recompiled C++
and the native build are produced on your Mac from your own disc, and are git-ignored: never commit
or share them.

To recompile the supported disc, the repository includes data from analysing that disc's
executable:

- **Guest addresses:** function bounds and entry points, the 394 switch tables of
  `config/switch_tables.toml`, and the three opt-in hooks of `config/title_hooks.toml`. Each hook
  carries the SHA-256 of the function bytes it replaces, so it never applies to another build.
- **Hashes:** SHA-256 of the disc's executable, of the Disc 2 packages and of a Disc 2 file map,
  so setup can confirm your discs are the supported version.
- **Observations:** register values, shader hashes and short descriptions of a few game functions,
  observed on the developers' own copies and quoted in comments, tests and `reference/`.

## What the macOS DMG contains

The DMG is built locally (`scripts/release/make_app.sh`, `scripts/release/make_dmg.py`) and is not
published here. It holds FM4 Recomp.app: the setup app, the `fm4recomp` runtime, open-source tools
and libraries, and their licences.

It contains no game assets, disc images or original executables; setup reads your own disc. The
`fm4recomp` binary does contain native code compiled from the recompiled game executable, so a DMG
is for local testing only and must not be shared.

## Warranty

FM4 Recomp is provided "as is", without warranty of any kind, under the GNU General Public License
version 3 or later.
