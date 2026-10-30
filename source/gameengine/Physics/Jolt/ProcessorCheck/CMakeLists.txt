# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# The processor check runs before main(), ahead of Jolt's start-up code, on processors that may
# lack the instructions Jolt and Blender are built with. So it is built without them: no Jolt link
# (Jolt's compile options would come with it) and without Blender's SIMD flags, as Blender's own
# check in source/blender/cpucheck. It learns which instructions Jolt uses from Jolt's
# JPH_USE_* definitions.

if(COMPILER_SIMD_FLAGS)
  remove_c_and_cxx_flag("${COMPILER_SIMD_FLAGS}")
endif()

set(INC
  .
)

set(INC_SYS
)

set(SRC
  JoltProcessorCheck.cpp

  JoltProcessorCheck.h
)

set(LIB
)

blender_add_lib(ge_physics_jolt_processor_check "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

get_target_property(_jolt_definitions Jolt INTERFACE_COMPILE_DEFINITIONS)
if(_jolt_definitions)
  list(FILTER _jolt_definitions INCLUDE REGEX "^JPH_USE_")
  target_compile_definitions(ge_physics_jolt_processor_check PRIVATE ${_jolt_definitions})
endif()
unset(_jolt_definitions)
