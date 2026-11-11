#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Builds the libraries the alpha bundles for macOS 26 on Apple silicon (Homebrew only ships macOS 27 bottles
# of them now), at the versions FM4 Recomp was built and tested with (Homebrew's installed ones): SDL 3.4.18, Vulkan-Headers,
# Vulkan-Loader, Vulkan-Utility-Libraries and Vulkan-ValidationLayers 1.4.363, SPIRV-Headers and SPIRV-Tools of Vulkan SDK
# 1.4.363.0, glslang 16.6.0. Sources are cloned at their release tags (each recorded with its commit); the build installs into
# <prefix> (lib/, share/vulkan/), which scripts/release/bundle_kit.sh takes. MoltenVK is not built (Homebrew's targets macOS 12).
#   scripts/release/build_runtime_deps.sh <work dir> <prefix>
set -euo pipefail
[ $# = 2 ] || { echo "usage: scripts/release/build_runtime_deps.sh <work dir> <prefix>" >&2; exit 2; }
WORK="$1"; PREFIX="$2"
export MACOSX_DEPLOYMENT_TARGET=26.0
mkdir -p "$WORK/src" "$WORK/build" "$PREFIX"
WORK="$(cd "$WORK" && pwd)"; PREFIX="$(cd "$PREFIX" && pwd)"
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_NAME_DIR=@rpath -Wno-dev)

fetch() {  # <name> <repository> <tag>
  local dir="$WORK/src/$1"
  [ -d "$dir" ] || git clone -q --depth 1 --branch "$3" "$2" "$dir"
  echo "$1 $3 $(git -C "$dir" rev-parse HEAD)" >> "$WORK/sources.txt"
}
build() {  # <name> [cmake options]
  local name="$1"; shift
  cmake -S "$WORK/src/$name" -B "$WORK/build/$name" "${COMMON[@]}" "$@" > "$WORK/build/$name.configure.log"
  ninja -C "$WORK/build/$name" install > "$WORK/build/$name.build.log"
  echo "built $name"
}

: > "$WORK/sources.txt"
fetch SDL https://github.com/libsdl-org/SDL.git release-3.4.18
fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git v1.4.363
fetch Vulkan-Loader https://github.com/KhronosGroup/Vulkan-Loader.git v1.4.363
fetch SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers.git vulkan-sdk-1.4.363.0
fetch SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools.git vulkan-sdk-1.4.363.0
fetch glslang https://github.com/KhronosGroup/glslang.git 16.6.0
fetch Vulkan-Utility-Libraries https://github.com/KhronosGroup/Vulkan-Utility-Libraries.git v1.4.363
fetch Vulkan-ValidationLayers https://github.com/KhronosGroup/Vulkan-ValidationLayers.git v1.4.363

build SDL -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
build Vulkan-Headers
build Vulkan-Loader -DBUILD_TESTS=OFF
build SPIRV-Headers
build SPIRV-Tools -DSPIRV-Headers_SOURCE_DIR="$WORK/src/SPIRV-Headers" -DBUILD_SHARED_LIBS=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF
build glslang -DBUILD_SHARED_LIBS=ON -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_OPT=ON -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON
build Vulkan-Utility-Libraries -DBUILD_TESTS=OFF
build Vulkan-ValidationLayers -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF

for f in "$PREFIX"/lib/*.dylib; do
  [ -L "$f" ] && continue
  echo "$(basename "$f") minos $(otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
done
echo "runtime deps: $PREFIX (sources: $WORK/sources.txt)"
