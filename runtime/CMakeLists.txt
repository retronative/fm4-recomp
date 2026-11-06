# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The runtime (M2: strict stops only). Title tables are generated into the build tree from the
# survey (scripts/gen_runtime_tables.py): no title address in runtime source. Warnings are errors
# and none is disabled.
set(FM4_GENERATED ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${FM4_GENERATED}/fm4_image.h ${FM4_GENERATED}/fm4_imports.cpp
  COMMAND Python3::Interpreter -B -I ${PROJECT_SOURCE_DIR}/scripts/gen_runtime_tables.py ${FM4_SURVEY} ${FM4_PPC} ${FM4_XEX} ${FM4_GENERATED}
  DEPENDS ${PROJECT_SOURCE_DIR}/scripts/gen_runtime_tables.py ${FM4_SURVEY}/report.json ${FM4_SURVEY}/image.bin
          ${FM4_PPC}/ppc_config.h ${FM4_PPC}/ppc_recomp_shared.h ${FM4_XEX}
  COMMENT "Generating the title tables")

file(GLOB FM4_KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)
add_executable(fm4recomp main.cpp stops.cpp guest_memory.cpp ${FM4_KERNEL_SOURCES} ${FM4_GENERATED}/fm4_imports.cpp ${FM4_GENERATED}/fm4_image.h)
target_include_directories(fm4recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/kernel ${PROJECT_SOURCE_DIR}/recomp ${FM4_GENERATED})
target_link_libraries(fm4recomp PRIVATE fm4_ppc)
target_compile_options(fm4recomp PRIVATE -O2 -Wall -Wextra -Werror)
