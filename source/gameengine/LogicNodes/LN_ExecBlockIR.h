/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_ExecBlockIR.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LN_RuntimeSemantics.h"
#include "LN_Types.h"

static constexpr uint32_t LN_EXEC_BLOCK_IR_CACHE_GENERATION = 1;
static constexpr uint32_t LN_EXEC_BLOCK_IR_FEATURE_MASK = 0u;

enum class LN_ExecOpKind : uint8_t {
  Nop = 0,
  BranchRoute,
  TimeFlowControl,
  VectorCommand,
  PhysicsCommand,
  ObjectStateCommand,
  ObjectColorCommand,
  CameraCommand,
  LightCommand,
  WindowCommand,
  InputMotionCommand,
  AudioCommand,
  ActionCommand,
  BoneCommand,
  MaterialCommand,
  GlobalCommand,
  FileCommand,
  SceneCommand,
  LifecycleCommand,
  ObjectReferenceCommand,
  GamePropertyCommand,
  TreePropertyCommand,
  TreeControlCommand,
  SendEvent,
  GlobalPropertyCommand,
  VariableCommand,
  NavigationCommand,
  SceneCollectionCommand,
  DebugDrawCommand,
  PhysicsConstraintCommand,
  SpawnPoolCommand,
};

struct LN_ExecOp {
  LN_ExecOpKind kind = LN_ExecOpKind::Nop;
  LN_OpCode opcode = LN_OpCode::Nop;
  uint32_t instruction_index = LN_INVALID_INDEX;
  uint32_t source_ref_index = 0;
  uint32_t guard_bool_expr_index = LN_INVALID_INDEX;
  uint32_t payload_index = LN_INVALID_INDEX;
  uint32_t fallback_requirements = LN_RUNTIME_FALLBACK_NONE;
};

struct LN_ExecBlock {
  uint32_t first_instruction = 0;
  uint32_t instruction_count = 0;
  uint32_t first_op = 0;
  uint32_t op_count = 0;
  uint32_t loop_frame_index = LN_INVALID_INDEX;
  uint32_t source_ref_index = 0;
};

struct LN_ExecBlockProgram {
  LN_Event event = LN_Event::OnFixedUpdate;
  uint32_t program_version = LN_PROGRAM_VERSION;
  uint32_t schema_version = LN_PROGRAM_SCHEMA_VERSION;
  uint32_t runtime_build_flags = 0;
  uint32_t cache_generation = LN_EXEC_BLOCK_IR_CACHE_GENERATION;
  uint32_t feature_mask = LN_EXEC_BLOCK_IR_FEATURE_MASK;
  std::vector<LN_ExecBlock> blocks;
  std::vector<LN_ExecOp> ops;
  std::vector<uint32_t> source_refs;
  uint32_t direct_instruction_count = 0;
  uint32_t fallback_instruction_count = 0;
  uint32_t fallback_block_count = 0;
  bool valid = true;
  std::vector<std::string> validation_errors;
};
