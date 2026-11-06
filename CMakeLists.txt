# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Native build of the recompiled FM4 default.xex (M2). Configure and build with scripts/build.sh
# (it runs preflight first, pins the SDK and records build time and memory).
cmake_minimum_required(VERSION 3.20)
project(fm4recomp LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "FM4 Recomp builds with Homebrew LLVM clang (/opt/homebrew/opt/llvm/bin/clang++)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(FM4_PPC ${PROJECT_SOURCE_DIR}/ppc)
set(FM4_SURVEY ${PROJECT_SOURCE_DIR}/game/survey)
set(FM4_XEX ${PROJECT_SOURCE_DIR}/game/files/default.xex)
if(NOT EXISTS ${FM4_PPC}/ppc_config.h OR NOT EXISTS ${FM4_SURVEY}/report.json)
  message(FATAL_ERROR "No translation: run scripts/setup_tools.sh and scripts/survey.sh first")
endif()

# Two targets: fm4_ppc (the translated code) and fm4recomp (the runtime), so a runtime change never
# recompiles the translation (ForzaRecomp's layout, CMakeLists.txt).
add_subdirectory(recomp)
add_subdirectory(runtime)
