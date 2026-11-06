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
# stops with its guest address instead of calling a null host pointer.
target_compile_options(fm4_ppc PRIVATE -O2 -fwrapv -fno-strict-aliasing -Werror -Werror=asm-operand-widths
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/indirect.h")
