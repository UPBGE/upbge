# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  intern
  ../Common
  ../Device
  ../Expressions
  ../GameLogic
  ../Ketsji
  ../Ketsji/KXNetworkMessage
  ../Physics/Common
  ../Rasterizer
  ../SceneGraph
  ../../blender
  ../../blender/blenkernel
  ../../blender/blenlib
  ../../blender/makesdna
  ../../blender/makesrna
  ../../blender/nodes
  ../../blender/python
  ../../blender/python/generic
  ../../blender/python/mathutils
  ../../blender/python/intern
)

set(INC_SYS
  ../../../extern/recastnavigation/Recast/Include
  ../../../extern/recastnavigation/Detour/Include
  ../../../intern/moto/include
  ../../../intern/ghost
)

set(SRC
  intern/LN_BindingSource.cpp
  intern/LN_CommandBuffer.cpp
  intern/LN_CommandDiagnostics.cpp
  intern/LN_DenseIds.cpp
  intern/LN_EventBus.cpp
  intern/LN_Manager.cpp
  intern/LN_NodeMetadata.cpp
  intern/LN_NodeRegistry.cpp
  intern/LN_Performance.cpp
  intern/LN_Program.cpp
  intern/LN_RuntimeTree.cpp
  intern/LN_ParallelTreeExecutor.cpp
  intern/LN_QueryDiagnostics.cpp
  intern/LN_RuntimeSemantics.cpp
  intern/LN_Snapshot.cpp
  intern/LN_TreeCompiler.cpp
  intern/LN_FormulaEval.cc
  intern/compile/LN_TreeCompiler_helpers_inputs.cc
  intern/compile/LN_TreeCompiler_reachability.cc
  intern/compile/LN_TreeCompiler_expression_outputs_by_kind.cc
  intern/compile/LN_TreeCompiler_output_expressions.cc

  LN_BindingSource.h
  LN_CommandBuffer.h
  LN_CommandDiagnostics.h
  LN_CompileReport.h
  LN_DebugMap.h
  LN_DenseIds.h
  LN_EventBus.h
  LN_ExecBlockIR.h
  LN_ParallelTreeExecutor.h
  LN_Manager.h
  LN_NodeMetadata.h
  LN_NodeStateArena.h
  LN_NodeRegistry.h
  LN_Performance.h
  LN_Program.h
  LN_QueryDiagnostics.h
  LN_RegisterExpressionIR.h
  LN_RuntimeTree.h
  LN_RuntimeSemantics.h
  LN_Snapshot.h
  LN_TreeCompiler.h
  intern/LN_FormulaEval.hh
  LN_Types.h
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  ge_common
  ge_expressions
)

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${AUDASPACE_C_LIBRARIES}
  )
  add_definitions(-DWITH_AUDASPACE)
endif()

blender_add_lib(ge_logic_nodes "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

if(WITH_GTESTS)
  set(TEST_SRC
    tests/LN_CommandBuffer_test.cpp
    tests/LN_EventBus_test.cpp
    tests/LN_Manager_test.cpp
    tests/LN_NodeRegistry_test.cpp
    tests/LN_Performance_test.cpp
    tests/LN_Program_test.cpp
    tests/LN_RuntimeTree_test.cpp
    tests/LN_Snapshot_test.cpp
    tests/LN_TreeCompiler_test.cpp
  )
  set(TEST_LIB
    ${LIB}
    ge_logic_nodes
  )
  blender_add_test_suite_lib(
    gameengine_logic_nodes
    "${TEST_SRC}"
    "${INC}"
    "${INC_SYS}"
    "${TEST_LIB}"
  )
  target_compile_definitions(gameengine_logic_nodes_tests
    PRIVATE LOGIC_NODES_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
endif()
