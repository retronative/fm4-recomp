# FM4 Recomp

A static recompilation of *Forza Motorsport 4* (Xbox 360, USA disc, title ID 4D530910) to native
arm64 macOS. The 0.3.0 Mac app uses the FM4 Static Recomp host and the ShiftGlue SDK.

> [!IMPORTANT]
> FM4 Recomp is an unofficial fan project, not affiliated with, endorsed by or sponsored by
> Microsoft, Xbox Game Studios or Turn 10 Studios. **No game files are included or distributed.**
> You need your own legally obtained Xbox 360 copy of *Forza Motorsport 4*; please do not request
> or share game files or disc images. What this repository and the macOS DMG contain:
> [docs/LEGAL.md](docs/LEGAL.md).

## Status

**FM4 Recomp 0.3.0** is available for Apple silicon Macs running macOS 26 or later. Download the [Mac app](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0.dmg), follow [Getting started](docs/getting-started.md), or read the [release notes](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0).

This public beta adds Extended View for wider and taller screens and an in-race Graphics menu with fullscreen and windowed modes, 720p through 4K rendering, MSAA, HDR and eight track and camera effects. New profiles start at Full HD. Spatial FSR 1 enlarges the image automatically when needed. Press **⌘,** to open settings and **Save & Apply** to confirm your choices.

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

For the 0.3.0 engine and macOS app, use the [versioned source archive](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz) and its [checksum](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz.sha256). The included README identifies the exact source revisions, launcher changes, decoder dependencies and commands. Regenerating the game's code requires the supported disc. The [contributor guide](CONTRIBUTING.md) describes the tooling in this repository.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) and [SUPPORT.md](SUPPORT.md) for support and reporting
channels. Report security problems privately, as described in [SECURITY.md](SECURITY.md).
Community conduct reports are covered in [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

FM4 Recomp is free software under the GNU General Public License, version 3 or (at your option)
any later version (GPL-3.0-or-later): see [COPYING](COPYING) and [NOTICE](NOTICE). Third-party
components and their licences are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
