# SPDX-License-Identifier: GPL-2.0-or-later

add_subdirectory(monocypher)

set(INC
  PUBLIC .
  intern
  # Only the #FileReader type from BLI_filereader.hh is used, no blenlib symbols.
  ../../source/blender/blenlib
)

set(INC_SYS
)

set(SRC
  intern/bc_filereader.cc
  intern/bc_runtime.cc

  BC_binarycrypt.hh
  binarycrypt_format.h
  intern/bc_runtime.hh
)

set(LIB
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::intern::monocypher
)

blender_add_lib(bf_intern_binarycrypt "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::binarycrypt ALIAS bf_intern_binarycrypt)

if(WITH_GTESTS)
  set(TEST_SRC
    tests/binarycrypt_test.cc
  )
  set(TEST_INC
  )
  set(TEST_LIB
    bf_intern_binarycrypt
    bf::blenlib
    bf::blenloader_core
    bf::dependencies::zstd
  )
  blender_add_test_suite_executable(binarycrypt "${TEST_SRC}" "${INC};${TEST_INC}" "${INC_SYS}" "${LIB};${TEST_LIB}")
endif()
