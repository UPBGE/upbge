# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  intern
  ../memutil
)

set(INC_SYS
)

if(WIN32)
  add_definitions(-D_USE_MATH_DEFINES)
endif()

set(SRC
  intern/IK_QJacobian.cpp
  intern/IK_QJacobianSolver.cpp
  intern/IK_QSegment.cpp
  intern/IK_QTask.cpp
  intern/IK_Solver.cpp

  extern/IK_solver.h
  intern/IK_Math.h
  intern/IK_QJacobian.h
  intern/IK_QJacobianSolver.h
  intern/IK_QSegment.h
  intern/IK_QTask.h
)

set(LIB
  PRIVATE bf::dependencies::eigen
)

blender_add_lib(bf_intern_iksolver "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
