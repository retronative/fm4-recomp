#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The third-party source archive published beside each binary release (THIRD_PARTY_NOTICES.md, "Source offer"; the layout
# follows ReCarbon's): the complete source of the LGPL libraries in the kit at the versions built (FFmpeg's release tarball
# with our patch and build script; libmspack at XenonRecomp's pinned revision), the source of the other libraries
# scripts/release/build_runtime_deps.sh built (a `git archive` of each clone, at the commit it recorded), that script, the
# two LGPL texts, a README with the build steps, and SHA256SUMS. Nothing is downloaded: every source is the copy the release
# was built from, checked against its pin (the FFmpeg tarball's SHA-256 in scripts/setup_tools.sh; each revision must be
# named in THIRD_PARTY_NOTICES.md), and a missing or different one fails.
#   scripts/release/make_source_archive.sh <version> <out dir> <deps work dir (build_runtime_deps.sh's)>
# Writes <out dir>/FM4-Recomp-<version>-third-party-source.tar.xz and its .sha256; refuses to overwrite.
set -euo pipefail
[ $# = 3 ] || { echo "usage: scripts/release/make_source_archive.sh <version> <out dir> <deps work dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="$1"; OUT="$2"; WORK="$3"
NAME="FM4-Recomp-$VERSION-third-party-source"
die() { echo "make_source_archive: $*" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
[ ! -e "$OUT/$NAME.tar.xz" ] || die "$OUT/$NAME.tar.xz exists (never overwritten)"
[ -f "$WORK/sources.txt" ] || die "$WORK/sources.txt is missing (scripts/release/build_runtime_deps.sh)"
NOTICES="$ROOT/THIRD_PARTY_NOTICES.md"
STAGE="$(mktemp -d "$OUT/.source-archive.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
S="$STAGE/$NAME"
mkdir -p "$S/sources" "$S/patches/ffmpeg" "$S/scripts/release" "$S/licenses"

# FFmpeg: the release tarball of scripts/setup_tools.sh, our patch, the build script.
FFMPEG_VERSION="$(sed -n 's/^FFMPEG_VERSION=//p' "$ROOT/scripts/setup_tools.sh")"
FFMPEG_SHA256="$(sed -n 's/^FFMPEG_SHA256=//p' "$ROOT/scripts/setup_tools.sh")"
TARBALL="$ROOT/tools/ffmpeg-src/ffmpeg-$FFMPEG_VERSION.tar.xz"
echo "$FFMPEG_SHA256  $TARBALL" | shasum -a 256 -c --quiet || die "$TARBALL is missing or does not match its SHA-256"
cp "$TARBALL" "$S/sources/"
cp "$ROOT"/patches/ffmpeg/*.patch "$S/patches/ffmpeg/"
cp "$ROOT/scripts/build_ffmpeg_xma.sh" "$S/scripts/"
cp "$ROOT/scripts/release/build_runtime_deps.sh" "$S/scripts/release/"
cp "$ROOT/LICENSES/FFmpeg-LGPL-2.1.txt" "$S/licenses/FFmpeg-COPYING.LGPLv2.1"
cp "$ROOT/LICENSES/libmspack-LGPL-2.1.txt" "$S/licenses/libmspack-COPYING.LIB"

archive() {  # <clone> <name>: sources/<name>-<commit>.tar.gz of the clone's HEAD, which must be clean and named in the notices
  local dir="$1" name="$2" rev
  rev="$(git -C "$dir" rev-parse HEAD)" || die "$dir is not a git clone"
  [ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ] || die "$dir has local changes"
  grep -q "$rev" "$NOTICES" || die "$name $rev is not the revision THIRD_PARTY_NOTICES.md names"
  git -C "$dir" archive --format=tar.gz --prefix="$name-$rev/" -o "$S/sources/$name-$rev.tar.gz" HEAD
  echo "$name $rev"
}
MSPACK="$(archive "$ROOT/tools/XenonRecomp/thirdparty/libmspack" libmspack)"
DEPS=""
while read -r name tag rev; do
  [ "$(git -C "$WORK/src/$name" rev-parse HEAD)" = "$rev" ] || die "$WORK/src/$name is not at $rev (sources.txt)"
  archive "$WORK/src/$name" "$name" > /dev/null
  DEPS="$DEPS  sources/$name-$rev.tar.gz   $name $tag"$'\n'
done < "$WORK/sources.txt"

cat > "$S/README.txt" <<README
FM4 Recomp $VERSION: source of the third-party libraries in the app

FM4 Recomp's own source is its repository at the release's tag (v$VERSION):
https://github.com/retronative/fm4-recomp. This archive holds the source of the libraries the app ships in
FM4 Recomp.app/Contents/Resources/kit/lib, at the versions it was built from. SHA256SUMS lists every file here.

The LGPL-2.1 libraries (replaceable: the steps are in the app's licenses/THIRD_PARTY_NOTICES.md)

  libavcodec.61.dylib, libavutil.59.dylib: FFmpeg $FFMPEG_VERSION (LGPL-2.1-or-later; licenses/FFmpeg-COPYING.LGPLv2.1)
    sources/ffmpeg-$FFMPEG_VERSION.tar.xz   https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
    patches/ffmpeg/                 our change (the xmaframes decoder)
    scripts/build_ffmpeg_xma.sh     the build. It applies the patches in patches/ffmpeg/ beside scripts/, so run it from
                                    this folder:
        scripts/build_ffmpeg_xma.sh sources/ffmpeg-$FFMPEG_VERSION.tar.xz <new prefix> <C compiler> shared
    (it works in a tools/ffmpeg-src/ folder it makes here, and fails unless FFmpeg configures as LGPL with only that decoder).
    The libraries are then in <new prefix>/lib.

  libfm4_lzx.dylib: libmspack's LZX decoder, mspack/lzxd.c (LGPL-2.1; licenses/libmspack-COPYING.LIB)
    sources/${MSPACK/ /-}.tar.gz   https://github.com/kyz/libmspack at that revision, unmodified
        tar -xzf sources/${MSPACK/ /-}.tar.gz
        cc -O2 -dynamiclib -arch arm64 -mmacosx-version-min=26.0 -install_name @rpath/libfm4_lzx.dylib \\
           -Wl,-headerpad_max_install_names -I ${MSPACK/ /-}/libmspack/mspack \\
           ${MSPACK/ /-}/libmspack/mspack/lzxd.c -o libfm4_lzx.dylib

The other libraries (Zlib, Apache-2.0, BSD and MIT terms), each a git archive of its project at the release tag

$DEPS
    scripts/release/build_runtime_deps.sh   the build (it clones these projects at the same tags; to build from the archives
                                            instead, unpack each as <work dir>/src/<name> first).

MoltenVK (Apache-2.0) is Homebrew's build of the upstream 1.4.2 release (https://github.com/KhronosGroup/MoltenVK); its source
is not in this archive.

The code that XenonRecomp generates from the game's executable is not source that can be published: FM4 Recomp's repository
regenerates it from your own disc (scripts/survey.sh).
README
( cd "$S" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs shasum -a 256 > SHA256SUMS )
# No account names, extended attributes or resource forks in the archive.
COPYFILE_DISABLE=1 tar -cJf "$OUT/$NAME.tar.xz" --no-xattrs --no-mac-metadata --uid 0 --gid 0 --uname root --gname wheel -C "$STAGE" "$NAME"
( cd "$OUT" && shasum -a 256 "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
echo "source archive: $OUT/$NAME.tar.xz ($(du -h "$OUT/$NAME.tar.xz" | cut -f1)), $(cat "$OUT/$NAME.tar.xz.sha256" | cut -d' ' -f1)"
