# FM4 Recomp 0.3.1 source and build guide

Version 0.3.1 (bundle build 10) updates the macOS launcher, disc setup and player-profile migration. The 16 files under the shipped ShiftGlue engine directory are byte-identical to 0.3.0. The engine source, decoder sources, dependency archives, licenses and build instructions for those bytes are included together in `engine-source/FM4-Recomp-0.3.0-source.tar.xz`.

The public source for this release is available in the [v0.3.1 Git tree](https://github.com/retronative/fm4-recomp/tree/v0.3.1). The [0.3.1 release](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.1) provides the installer, full source companion and checksums.

## Verify the sources

With the downloaded 0.3.1 source archive and its checksum side by side, run:

```sh
shasum -a 256 -c FM4-Recomp-0.3.1-source.tar.xz.sha256
tar -xf FM4-Recomp-0.3.1-source.tar.xz
cd FM4-Recomp-0.3.1-source
shasum -a 256 -c SHA256SUMS
```

The launcher source is under `app/setup/`. The scripts and Swift converter for adopting existing disc locations and importing older profiles are under `scripts/alpha/`. To test the launcher, migration and save-conflict handling on an Apple silicon Mac with macOS 26 or later and Apple developer tools, run:

```sh
bash tests/run_app_tests.sh
```

The suite creates temporary profiles for comparisons, including conflicting and partially imported saves. It does not open the game.

## Build the macOS app

Download the [official 0.3.0 installer](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0.dmg) beside the extracted source directory. From the source root:

```sh
bash scripts/release/profile-update/make_app.sh \
  ../FM4-Recomp-0.3.0.dmg "build/FM4 Recomp.app"
codesign --verify --deep --strict "build/FM4 Recomp.app"
```

The script verifies the 0.3.0 installer against SHA-256 `dafb8a792c72a405f71c7c886bf1246db66f405062121585eb45c791942bc967`, mounts it read-only, compiles the two Swift executables, installs the updated scripts and documentation, signs the app locally and runs its setup prerequisites check. The output path must be new. This build needs no game image. Playing requires the owner's supported USA Xbox 360 Disc 1 image. Local signatures and compiler versions can produce different app and installer checksums. The shipped app's changed-file hashes are recorded in `scripts/release/profile-update/release-inputs.json`.

## Build or replace the engine and decoders

```sh
tar -xf engine-source/FM4-Recomp-0.3.0-source.tar.xz
cd FM4-Recomp-0.3.0-source
shasum -a 256 -c SHA256SUMS
```

Follow the inner README for the host, SDK, Vulkan libraries and independently replaceable decoder libraries. The pinned host is `160ef51f716ebc6f90692d368d68f3a820fcecfe`. The pinned SDK is `d16189ff0488352f26b66a8fc3cbb723533fb3b6`. The host build regenerates game code from the owner's disc. The inner archive is also available as a [separate source download](https://github.com/retronative/fm4-recomp/releases/download/v0.3.0/FM4-Recomp-0.3.0-source.tar.xz). The port's full changed host and SDK files are [browsable on GitHub](https://github.com/retronative/fm4-recomp/tree/v0.3.1/ports/v0.3.0).

To use your rebuilt engine, stage its complete, signed engine directory as described in the inner guide. Copy that directory to `Contents/Resources/kit/engines/shiftglue/` in a copy of your 0.3.1 app, preserving the directory structure and runtime library names. Re-sign the outer app with `codesign --force --sign - "FM4 Recomp.app"` and run `codesign --verify --deep --strict "FM4 Recomp.app"`.

## Package the installer or reproduce the source companion

From the 0.3.1 source root, the following optional commands generate the DMG artwork and installer. Artwork generation uses Node 22 or later, Google Chrome and macOS imaging tools. Packaging requires Apple's development tools and a Python 3 installation with pip and venv.

```sh
node assets/dmg/render.mjs --out build/release-art
python3 -m venv build/dmg-env
build/dmg-env/bin/python -m pip install --require-hashes --no-deps \
  -r scripts/release/dmg-requirements.txt
build/dmg-env/bin/python scripts/release/make_dmg.py \
  "build/FM4 Recomp.app" build/release-art build/FM4-Recomp-0.3.1.dmg
```

To generate the source archive and checksum from the release tree or extracted sources, provide the verified 0.3.0 source archive and an output directory:

```sh
python3 scripts/release/profile-update/make_source.py \
  engine-source/FM4-Recomp-0.3.0-source.tar.xz build/source-release
```
