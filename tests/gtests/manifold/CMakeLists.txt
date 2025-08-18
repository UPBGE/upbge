# SPDX-FileCopyrightText: 2024 UPBGE Contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Unit tests for Manifold 3D integration in UPBGE
# Tests the KX_ManifoldWrapper architecture improvements

if(WITH_MANIFOLD AND WITH_GTESTS)
  set(SRC
    kx_manifold_wrapper_test.cc
  )

  set(INC
    ../../../../source/gameengine/Ketsji
    ../../../../source/blender/makesdna
    ../../../../extern/gmock/include
    ../../../../extern/gtest/include
  )

  set(INC_SYS
    ${MANIFOLD_INCLUDE_DIRS}
  )

  if(WITH_TBB)
    list(APPEND INC_SYS
      ${TBB_INCLUDE_DIRS}
    )
  endif()

  set(LIB
    libge_ketsji
    ${MANIFOLD_LIBRARIES}
  )

  if(WITH_TBB)
    list(APPEND LIB
      ${TBB_LIBRARIES}
    )
  endif()

  blender_add_test_suite_lib(
    manifold
    "${SRC}"
    "${INC}"
    "${INC_SYS}"
    "${LIB}"
  )
endif()