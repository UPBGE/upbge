# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  intern
  ../Common
  ../Converter
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
  ../../blender/windowmanager
)

set(INC_SYS
  ../../../extern/json/include
  ../../../extern/recastnavigation/Recast/Include
  ../../../extern/recastnavigation/Detour/Include
  ../../../intern/moto/include
  ../../../intern/ghost
)

set(SRC
  intern/LN_BindingSource.cpp
  intern/LN_Columns.cpp
  intern/LN_CommandBuffer.cpp
  intern/LN_CommandDiagnostics.cpp
  intern/LN_DenseIds.cpp
  intern/LN_EventBus.cpp
  intern/LN_GameState.cpp
  intern/LN_GameStateCodec.cpp
  intern/LN_GameStateGeometryNodes.cpp
  intern/LN_GameStateLogicTrees.cpp
  intern/LN_GameStateMaterial.cpp
  intern/LN_GameStateNodeTrees.cpp
  intern/LN_GameStateStorage.cpp
  intern/LN_InputMap.cpp
  intern/LN_JobPool.cpp
  intern/LN_KindCatalog.cpp
  intern/LN_LeanProgram.cpp
  intern/LN_Inventory.cpp
  intern/LN_PersistenceCapability.cpp
  intern/LN_Manager.cpp
  intern/LN_NodeMetadata.cpp
  intern/LN_NodeRegistry.cpp
  intern/LN_Performance.cpp
  intern/LN_RuntimeTree.cpp
  intern/LN_ProgramDump.cpp
  intern/LN_StrictDump.cpp
  intern/LN_QueryDiagnostics.cpp
  intern/LN_RuntimeSemantics.cpp
  intern/LN_Snapshot.cpp
  intern/LN_SoundGroups.cpp
  intern/LN_TreeBatch.cpp
  intern/LN_TreeCompiler.cpp
  intern/LN_UIService.cpp
  intern/LN_FormulaEval.cc
  intern/compile/LN_TreeCompiler_helpers_inputs.cc
  intern/compile/LN_TreeCompiler_reachability.cc
  intern/compile/LN_TreeCompiler_expression_outputs_by_kind.cc
  intern/compile/LN_TreeCompiler_output_expressions.cc
  # Archive this implementation after its users so static linking can resolve its symbols.
  intern/LN_Program.cpp

  LN_BindingSource.h
  LN_Columns.h
  LN_CommandBuffer.h
  LN_CommandDiagnostics.h
  LN_CompileReport.h
  LN_DebugMap.h
  LN_DenseIds.h
  LN_EventBus.h
  LN_GameState.h
  LN_InputMap.h
  LN_JobPool.h
  LN_Inventory.h
  LN_PersistenceCapability.h
  LN_ExecBlockIR.h
  LN_ExecFrame.h
  LN_LeanProgram.h
  LN_LeanReader.h
  LN_Manager.h
  LN_NodeMetadata.h
  LN_NodeRegistry.h
  LN_Performance.h
  LN_Program.h
  LN_QueryDiagnostics.h
  LN_RegisterExpressionIR.h
  LN_Rotation.h
  LN_BonePoseRotation.h
  LN_RuntimeTree.h
  LN_RuntimeSemantics.h
  LN_Snapshot.h
  LN_SoundGroups.h
  LN_StateBlock.h
  LN_TreeBatch.h
  LN_TreeCompiler.h
  LN_UIService.h
  LN_TypeConversion.h
  LN_ValueInterpolation.h
  intern/LN_FormulaEval.hh
  intern/LN_GameStateGeometryNodes.h
  intern/LN_GameStateInternal.h
  intern/LN_GameStateMaterial.h
  intern/LN_GameStateNodeTrees.h
  LN_Types.h
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::dna
  PRIVATE bf::depsgraph
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
    tests/LN_DenseIds_test.cpp
    tests/LN_EventBus_test.cpp
    tests/LN_GameState_test.cpp
    tests/LN_InputMap_test.cpp
    tests/LN_Inventory_test.cpp
    tests/LN_JobPool_test.cpp
    tests/LN_LeanProgram_test.cpp
    tests/LN_LeanNative_test.cpp
    tests/LN_TreeBlock_test.cpp
    tests/LN_Columns_test.cpp
    tests/LN_LeanRoute_test.cpp
    tests/LN_ObjectAttribute_test.cpp
    tests/LN_KindCatalog_test.cpp
    tests/LN_LeanDifferential_test.cpp
    tests/LN_ConstantObjects_test.cpp
    tests/LN_Manager_test.cpp
    tests/LN_NodeCoverage_test.cpp
    tests/LN_TransformApply_test.cpp
    tests/LN_NodeRegistry_test.cpp
    tests/LN_NodeValues_test.cpp
    tests/LN_ReadOrder_test.cpp
    tests/LN_Performance_test.cpp
    tests/LN_Program_test.cpp
    tests/LN_Rotation_test.cpp
    tests/LN_RuntimeTree_test.cpp
    tests/LN_Semantics_test.cpp
    tests/LN_Snapshot_test.cpp
    tests/LN_StrictDump_test.cpp
    tests/LN_Sound_test.cpp
    tests/LN_TreeCompiler_test.cpp
    tests/LN_UIService_test.cpp
    tests/LN_DiffTrees.h
    tests/LN_SemRig.h
    tests/LN_TestTrees.h
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
