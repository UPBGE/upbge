# SPDX-License-Identifier: GPL-2.0-or-later

# Vendored third-party code: built with relaxed warnings, like the libraries in extern/.
remove_strict_flags()

set(INC
  PUBLIC .
)

set(INC_SYS
)

set(SRC
  monocypher.c

  monocypher.h
)

set(LIB
)

blender_add_lib(bf_intern_monocypher "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::monocypher ALIAS bf_intern_monocypher)
