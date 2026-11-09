# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The translated guest code: every ppc/*.cpp written by XenonRecomp (scripts/survey.sh), as one static
# library. Warnings are errors and none is disabled. The semantic flags are those the translator
# fixtures are verified under (tests/run_translator_tests.sh: -fwrapv -fno-strict-aliasing); -O2 is
# the release level (at -O1 simde's `clang loop vectorize(enable)` hints are reported as not honoured).
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/ppc/*.cpp)
if(NOT PPC_SOURCES)
  message(FATAL_ERROR "ppc/ is empty: run scripts/survey.sh first")
endif()
add_library(fm4_ppc STATIC ${PPC_SOURCES})
target_include_directories(fm4_ppc PUBLIC
  ${PROJECT_SOURCE_DIR}/ppc
  ${PROJECT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde)
# Every indirect call goes through the runtime's checked lookup (recomp/indirect.h): a missing target
# stops with its guest address instead of calling a null host pointer. Device accesses the translator marks
# (eieio-ordered stores, lwbrx) go through the runtime too (recomp/mmio.h).
# -fvisibility=hidden (FM4 Recomp, M7): the translated functions are weak (PPC_WEAK_FUNC); with default visibility the
# executable exports them and every call between them goes through a dyld stub (a sampled race:
# about 12% of the game thread in DYLD-STUB$$ entries). Hidden, they still resolve weak/strong at the static link, called directly.
target_compile_options(fm4_ppc PRIVATE -O2 -fvisibility=hidden -fwrapv -fno-strict-aliasing -Werror -Werror=asm-operand-widths
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/indirect.h" "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/mmio.h")

# Each module default.xex loads, as its own library from game/modules/<name>/ppc, with the same flags.
# <name>_symbols.h (generated) renames the names every translation defines alike (PPCFuncMappings,
# _xstart, the save/restore helpers); scripts/check_module_symbols.py then fails the build if two
# libraries still define one symbol (the libraries' functions are weak: a duplicate would link silently).
set(FM4_MODULE_LIBRARIES $<TARGET_FILE:fm4_ppc> ${PROJECT_SOURCE_DIR}/ppc/ppc_recomp_shared.h)
foreach(module IN LISTS FM4_MODULES)
  file(GLOB sources CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/game/modules/${module}/ppc/*.cpp)
  add_library(${module}_ppc STATIC ${sources})
  target_include_directories(${module}_ppc PRIVATE
    ${PROJECT_SOURCE_DIR}/game/modules/${module}/ppc
    ${PROJECT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde)
  target_compile_options(${module}_ppc PRIVATE -O2 -fvisibility=hidden -fwrapv -fno-strict-aliasing -Werror -Werror=asm-operand-widths
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/indirect.h" "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/mmio.h"
    "SHELL:-include ${FM4_GENERATED}/${module}_symbols.h")
  add_dependencies(${module}_ppc fm4_generated)
  list(APPEND FM4_MODULE_LIBRARIES $<TARGET_FILE:${module}_ppc> ${PROJECT_SOURCE_DIR}/game/modules/${module}/ppc/ppc_recomp_shared.h)
endforeach()
add_custom_target(check_module_symbols
  COMMAND Python3::Interpreter -B -I ${PROJECT_SOURCE_DIR}/scripts/check_module_symbols.py ${CMAKE_NM} ${FM4_MODULE_LIBRARIES}
  COMMENT "Checking that no two module libraries define one symbol")
add_dependencies(check_module_symbols fm4_ppc)
foreach(module IN LISTS FM4_MODULES)
  add_dependencies(check_module_symbols ${module}_ppc)
endforeach()
