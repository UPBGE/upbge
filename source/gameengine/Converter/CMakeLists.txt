# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is Copyright (C) 2006, Blender Foundation
# All rights reserved.
#
# The Original Code is: all of this file.
#
# Contributor(s): Jacques Beaurain.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
  .
  ../BlenderRoutines
  ../Common
  ../Device
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../Ketsji/KXNetwork
  ../Launcher
  ../Physics/Bullet
  ../Physics/Dummy
  ../Physics/Common
  ../Rasterizer
  ../Rasterizer/RAS_OpenGLRasterizer
  ../SceneGraph
  ../VideoTexture
  ../../blender
  ../../blender/ikplugin
  ../../blender/makesrna
  ../../../intern/termcolor
  # RNA_prototypes.h
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
  ../../../intern/moto/include
  ../../../extern/recastnavigation/Detour/Include
  ${EIGEN3_INCLUDE_DIRS}
  ${PTHREADS_INCLUDE_DIRS}
  ${BOOST_INCLUDE_DIR}
  ${Epoxy_INCLUDE_DIRS}
)

set(SRC
  BL_ArmatureActuator.cpp
  BL_ArmatureChannel.cpp
  BL_ArmatureConstraint.cpp
  BL_ArmatureObject.cpp
  BL_Converter.cpp
  BL_ConvertActuators.cpp
  BL_ConvertControllers.cpp
  BL_ConvertProperties.cpp
  BL_ConvertSensors.cpp
  BL_DataConversion.cpp
  BL_ScalarInterpolator.cpp
  BL_SceneConverter.cpp
  #BL_IpoConvert.cpp (everything inside BL_IpoConvert.h)

  BL_ArmatureActuator.h
  BL_ArmatureChannel.h
  BL_ArmatureConstraint.h
  BL_ArmatureObject.h
  BL_Converter.h
  BL_ConvertActuators.h
  BL_ConvertControllers.h
  BL_ConvertProperties.h
  BL_ConvertSensors.h
  BL_DataConversion.h
  BL_IpoConvert.h
  BL_ScalarInterpolator.h
  BL_SceneConverter.h
)

set(LIB
  PRIVATE bf::animrig
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::draw
  PRIVATE bf::dna
  PRIVATE bf::extern::fmtlib
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::render
  PRIVATE bf::windowmanager
  ge_physics_dummy
  ge_physics_bullet
  ge_ketsji
  ge_rasterizer

)

if(MSVC)
  add_definitions(
    /DNOMINMAX
  )
endif()

if(WITH_BULLET)
  list(APPEND INC_SYS
    ${BULLET_INCLUDE_DIRS}
  )
  add_definitions(-DWITH_BULLET)
endif()

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${AUDASPACE_C_LIBRARIES}
    ${AUDASPACE_PY_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

blender_add_lib(ge_converter "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.h
add_dependencies(ge_converter bf_rna)
