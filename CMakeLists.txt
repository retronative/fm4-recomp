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

# The modules default.xex loads (XexLoadImage), each translated by scripts/survey.sh --module <name>
# into game/modules/<name>/{survey,ppc} and built as its own library (recomp/CMakeLists.txt).
set(FM4_MODULES xmediafacade)
set(FM4_XEX_xmediafacade ${PROJECT_SOURCE_DIR}/game/files/XMediaFacade_default.xex)

# Title tables, generated into the build tree from the surveys (scripts/gen_runtime_tables.py): no
# title address in runtime source. Generated here so both the libraries and the runtime depend on them.
set(FM4_GENERATED ${CMAKE_BINARY_DIR}/generated)
set(FM4_GENERATOR_ARGS)
set(FM4_GENERATOR_INPUTS ${FM4_SURVEY}/report.json ${FM4_SURVEY}/image.bin ${FM4_PPC}/ppc_config.h
    ${FM4_PPC}/ppc_recomp_shared.h ${FM4_XEX})
set(FM4_GENERATED_FILES ${FM4_GENERATED}/fm4_image.h ${FM4_GENERATED}/fm4_imports.cpp ${FM4_GENERATED}/fm4_modules.h)
foreach(module IN LISTS FM4_MODULES)
  set(dir ${PROJECT_SOURCE_DIR}/game/modules/${module})
  if(NOT EXISTS ${dir}/ppc/ppc_config.h OR NOT EXISTS ${dir}/survey/report.json)
    message(FATAL_ERROR "No translation of ${module}: run scripts/survey.sh --module ${module} first")
  endif()
  list(APPEND FM4_GENERATOR_ARGS --module ${module} ${dir}/survey ${dir}/ppc ${FM4_XEX_${module}})
  list(APPEND FM4_GENERATOR_INPUTS ${dir}/survey/report.json ${dir}/survey/image.bin ${dir}/ppc/ppc_config.h
       ${dir}/ppc/ppc_recomp_shared.h ${FM4_XEX_${module}})
  list(APPEND FM4_GENERATED_FILES ${FM4_GENERATED}/${module}_symbols.h)
endforeach()
add_custom_command(
  OUTPUT ${FM4_GENERATED_FILES}
  COMMAND Python3::Interpreter -B -I ${PROJECT_SOURCE_DIR}/scripts/gen_runtime_tables.py ${FM4_SURVEY} ${FM4_PPC} ${FM4_XEX}
          ${FM4_GENERATED} ${FM4_GENERATOR_ARGS}
  DEPENDS ${PROJECT_SOURCE_DIR}/scripts/gen_runtime_tables.py ${FM4_GENERATOR_INPUTS}
  COMMENT "Generating the title tables")
add_custom_target(fm4_generated DEPENDS ${FM4_GENERATED_FILES})

# Two targets: fm4_ppc (the translated code) and fm4recomp (the runtime), so a runtime change never
# recompiles the translation (ForzaRecomp's layout, CMakeLists.txt).
add_subdirectory(recomp)
add_subdirectory(runtime)
