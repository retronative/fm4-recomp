# Rebuilding FM4 Recomp 0.3.0 for macOS

Version 0.3.0 packages the FM4 Static Recomp host and ShiftGlue SDK with the Mac launcher. The source companion supplies the exact upstream revisions, host and SDK changes, decoder sources, notices and packaging scripts. The original engine and its build scripts are retained in this repository for earlier releases.

Download the archive and its checksum into an empty directory.

```sh
base=https://github.com/retronative/fm4-recomp/releases/download/v0.3.0
curl -fLO "$base/FM4-Recomp-0.3.0-source.tar.xz"
curl -fLO "$base/FM4-Recomp-0.3.0-source.tar.xz.sha256"
shasum -a 256 -c FM4-Recomp-0.3.0-source.tar.xz.sha256
tar -xf FM4-Recomp-0.3.0-source.tar.xz
cd FM4-Recomp-0.3.0-source
shasum -a 256 -c SHA256SUMS
```

The official archive SHA-256 is `c89809d9f6baba809044c506548d42f35bd7b50283cf3193284c779b080064b8`. Follow its README to build the replaceable decoder libraries, regenerate the game host from your own supported Xbox 360 USA Disc 1 and assemble the Mac app. The packaging recipe uses the verified 0.2.0 installer for setup tools and artwork. The archive records the shipped binaries' hashes in `CANDIDATE-INPUTS.json` and the SDK changes in `SOURCE-PINS.json`.

[0.3.0 release and downloads](https://github.com/retronative/fm4-recomp/releases/tag/v0.3.0)
