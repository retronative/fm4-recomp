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

**No release is published yet.** The first public beta, FM4 Recomp 0.1.0, for Apple
silicon Macs (macOS 26 or later), is in preparation. It plays the game in a window with sound,
from the title screen through the menus into a race, with a gamepad or the keyboard, from your own
Disc 1 (Disc 2 is optional). It is an early, unoptimised build: expect a low frame rate.
[Getting started](docs/getting-started.md) describes how it installs.

Changes by release are in [CHANGELOG.md](CHANGELOG.md). The public status of each milestone, the
beta's contents and its known issues are in [docs/status.md](docs/status.md). See also
[Compatibility](docs/compatibility.md) and the [FAQ](docs/faq.md).

## Approach

FM4 Recomp statically recompiles the game's executable code for arm64 and runs it on a native
runtime that reimplements the platform it was written for. Here the guest is the **Xbox 360**
(PowerPC code, the console's kernel and system functions, the Xenos GPU, XMA audio), so the
runtime replaces those, drawing through Vulkan on Metal. A release ships FM4 Recomp: the setup
app, the runtime and open-source tools and libraries. It contains no game data; setup reads your
own disc. Its program contains code translated by machine from the game's executable
([docs/LEGAL.md](docs/LEGAL.md)). See [How it works](docs/how-it-works.md); the design notes are in
[reference/](reference/README.md).

## Building from source

```sh
scripts/setup_tools.sh                                    # pinned tools into tools/
scripts/extract_game.sh <dir with your ISOs> <out dir>    # unpack your discs outside the repo
mkdir -p game && ln -sfn <out dir>/disc1 game/files
```

The remaining steps and the rules for changes are in [CONTRIBUTING.md](CONTRIBUTING.md). Saves of
a development build are stored in `game/user-data/` and use a fixed, built-in compatibility key
(the console's own key is unknown). Compatibility with real Xbox 360 saves is **unverified**:
saves made here are expected to load only here.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) and [SUPPORT.md](SUPPORT.md) for support and reporting
channels. Report security problems privately, as described in [SECURITY.md](SECURITY.md).
Community conduct reports are covered in [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

FM4 Recomp is free software under the GNU General Public License, version 3 or (at your option)
any later version (GPL-3.0-or-later): see [COPYING](COPYING) and [NOTICE](NOTICE). Third-party
components and their licences are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
