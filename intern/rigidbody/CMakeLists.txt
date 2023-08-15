# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# UPBGE - to recover single precision comment this definition, intern/rigidbody/CMakeLists.txt one
# and source/gameengine/Physics/Bullet/CMakeLists.txt one.
# Double precision is slower than float one but it will increase the precision in
# open worlds games bigger than 10Km.
add_definitions(-DBT_USE_DOUBLE_PRECISION)

set(INC
  .
)

set(INC_SYS
  ${BULLET_INCLUDE_DIRS}
)

set(SRC
  rb_bullet_api.cpp
  rb_convex_hull_api.cpp

  RBI_hull_api.h
  RBI_api.h
)

set(LIB
  ${BULLET_LIBRARIES}
)

blender_add_lib(bf_intern_rigidbody "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
