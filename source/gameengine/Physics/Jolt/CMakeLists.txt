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

# Stops the program before main() on a processor without the instructions Jolt is built with.
# A library of its own, built without them (see its CMakeLists.txt).
add_subdirectory(ProcessorCheck)

# Jolt Physics headers may trigger warnings we cannot fix.
remove_strict_flags()

set(INC
  .
  ProcessorCheck
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
  JoltShapeQueryData.h
  JoltSoftBody.h
  JoltVehicle.h
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE Jolt
  PRIVATE ge_physics_jolt_processor_check
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

if(WITH_GTESTS)
  set(TEST_SRC
    tests/JoltCharacter_test.cpp
    tests/JoltConstraint_test.cpp
    tests/JoltDebugDraw_test.cpp
    tests/JoltLogicNodesQueries_test.cpp
    tests/JoltNavigation_test.cpp
    tests/JoltProcessorCheck_test.cpp
  )
  # JoltLogicNodesQueries_test runs Logic Nodes trees (their test rigs) against a real Jolt world.
  set(TEST_INC
    ${INC}
    ../../../../extern/recastnavigation
    ../../LogicNodes
    ../../LogicNodes/intern
    ../../LogicNodes/tests
    ../../Device
    ../../Ketsji/KXNetworkMessage
    ../../../blender
    ../../../blender/blenlib
    ../../../blender/makesdna
    ../../../blender/nodes
    ../../../blender/windowmanager
  )
  set(TEST_LIB
    ${LIB}
    ge_physics_jolt
    ge_logic_nodes
  )
  blender_add_test_suite_lib(
    gameengine_physics_jolt
    "${TEST_SRC}"
    "${TEST_INC}"
    "${INC_SYS}"
    "${TEST_LIB}"
  )
  target_compile_definitions(gameengine_physics_jolt_tests
    PRIVATE LOGIC_NODES_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
endif()
