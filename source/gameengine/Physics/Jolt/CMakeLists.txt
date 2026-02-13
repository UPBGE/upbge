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
# Contributor(s): UPBGE Contributors
#
# ***** END GPL LICENSE BLOCK *****

# Jolt Physics headers may trigger warnings we cannot fix.
remove_strict_flags()

set(INC
  .
  ../Common
  ../../Common
  ../../Converter
  ../../Expressions
  ../../GameLogic
  ../../Ketsji
  ../../Rasterizer
  ../../SceneGraph
  ../../../blender/blenkernel
  ../../../blender/makesrna
)

set(INC_SYS
  ../../../../intern/moto/include
  ${JOLT_INCLUDE_DIRS}
)

set(SRC
  JoltCharacter.cpp
  JoltCollData.cpp
  JoltConstraint.cpp
  JoltDebugDraw.cpp
  JoltDefaultMotionState.cpp
  JoltGraphicController.cpp
  JoltPhysicsController.cpp
  JoltPhysicsEnvironment.cpp
  JoltShapeBuilder.cpp
  JoltSoftBody.cpp
  JoltVehicle.cpp

  JoltCharacter.h
  JoltCollData.h
  JoltConstraint.h
  JoltDebugDraw.h
  JoltDefaultMotionState.h
  JoltGraphicController.h
  JoltMathUtils.h
  JoltPhysicsController.h
  JoltPhysicsEnvironment.h
  JoltShapeBuilder.h
  JoltSoftBody.h
  JoltVehicle.h
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE Jolt
)

if(WITH_PYTHON)
  list(APPEND INC_SYS
    ${PYTHON_INCLUDE_DIRS}
  )
endif()

add_definitions(-DWITH_JOLT)
add_definitions(-DJPH_OBJECT_LAYER_BITS=32)
add_definitions(${GL_DEFINITIONS})

blender_add_lib(ge_physics_jolt "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
