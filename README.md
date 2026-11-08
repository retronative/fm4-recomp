# FM4 Recomp

> [!IMPORTANT]
> An unofficial, fan-made project in early development. It contains no game files: you need
> your own legally obtained discs of *Forza Motorsport 4* (Xbox 360). Not affiliated with
> Microsoft, Xbox Game Studios or Turn 10 Studios.

A static recompilation of *Forza Motorsport 4* (Xbox 360, USA disc, title ID 4D530910) to native
arm64 macOS, following the method of ReCarbon.

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
