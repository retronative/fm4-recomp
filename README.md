# FM4 Recomp

A static recompilation of *Forza Motorsport 4* (Xbox 360, USA disc, title ID 4D530910) to native
arm64 macOS, built on [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

> [!IMPORTANT]
> FM4 Recomp is an unofficial fan project, not affiliated with, endorsed by or sponsored by
> Microsoft, Xbox Game Studios or Turn 10 Studios. **No game files are included or distributed.**
> You need your own legally obtained Xbox 360 copy of *Forza Motorsport 4*; please do not request
> or share game files or disc images. What this repository and the macOS DMG contain:
> [docs/LEGAL.md](docs/LEGAL.md).

## Status

Early development.

Saves are stored in `game/user-data/` and use a fixed, built-in compatibility key (the console's own key is unknown).
Compatibility with real Xbox 360 saves is **unverified**: saves made here are expected to load only here.

## Getting started (developers)

```sh
scripts/setup_tools.sh                                    # pinned tools into tools/
scripts/extract_game.sh <dir with your ISOs> <out dir>    # unpack your discs outside the repo
mkdir -p game && ln -sfn <out dir>/disc1 game/files
```

Design notes: [reference/](reference/README.md).

## License

GPL-3.0-or-later ([COPYING](COPYING), [NOTICE](NOTICE)).
