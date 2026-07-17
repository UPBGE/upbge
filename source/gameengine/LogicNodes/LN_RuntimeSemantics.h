/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeSemantics.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "LN_Types.h"

enum LN_RuntimeSemanticRead : uint32_t {
  LN_RUNTIME_SEMANTIC_READ_NONE = 0u,
  LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE = 1u << 0,
  LN_RUNTIME_SEMANTIC_READ_SNAPSHOT = 1u << 1,
  LN_RUNTIME_SEMANTIC_READ_INPUT = 1u << 2,
  LN_RUNTIME_SEMANTIC_READ_EVENT_BUS = 1u << 3,
  LN_RUNTIME_SEMANTIC_READ_QUERY_CACHE = 1u << 4,
  LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE = 1u << 5,
  LN_RUNTIME_SEMANTIC_READ_SCENE = 1u << 6,
  LN_RUNTIME_SEMANTIC_READ_GLOBAL_STATE = 1u << 7,
  LN_RUNTIME_SEMANTIC_READ_FILE = 1u << 8,
};

enum LN_RuntimeSemanticWrite : uint32_t {
  LN_RUNTIME_SEMANTIC_WRITE_NONE = 0u,
  LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM = 1u << 0,
  LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE = 1u << 1,
  LN_RUNTIME_SEMANTIC_WRITE_EVENT_BUS = 1u << 2,
  LN_RUNTIME_SEMANTIC_WRITE_OBJECT_STATE = 1u << 3,
  LN_RUNTIME_SEMANTIC_WRITE_SCENE = 1u << 4,
  LN_RUNTIME_SEMANTIC_WRITE_AUDIO = 1u << 5,
  LN_RUNTIME_SEMANTIC_WRITE_FILE = 1u << 6,
  LN_RUNTIME_SEMANTIC_WRITE_GLOBAL_STATE = 1u << 7,
  LN_RUNTIME_SEMANTIC_WRITE_RENDER = 1u << 8,
};

enum class LN_RuntimeSemanticTiming : uint8_t {
  DeferredCommandFlush = 0,
  ImmediateMainThreadRecord,
  RuntimeTreeState,
  BranchRoute,
};

enum class LN_RuntimeSemanticThreading : uint8_t {
  WorkerSafeRecord = 0,
  MainThreadRecord,
  MainThreadFlush,
};

enum class LN_RuntimeSemanticOrdering : uint8_t {
  NotObservable = 0,
  ObservablePerTarget,
  ObservableGlobal,
};

enum class LN_RuntimeSemanticCoalescing : uint8_t {
  Forbidden = 0,
  LastWriteWins,
};

enum LN_RuntimeFallbackRequirement : uint32_t {
  LN_RUNTIME_FALLBACK_NONE = 0u,
  LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION = 1u << 0,
  LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP = 1u << 1,
  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ = 1u << 2,
  LN_RUNTIME_FALLBACK_UNSUPPORTED_RUNTIME = 1u << 3,
  LN_RUNTIME_FALLBACK_LEGACY_INTERPRETER = 1u << 4,
  LN_RUNTIME_FALLBACK_MISSING_SNAPSHOT_CHANNEL = 1u << 5,
  LN_RUNTIME_FALLBACK_STALE_HANDLE = 1u << 6,
  LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY = 1u << 7,
};

enum class LN_RuntimeCommandFamily : uint8_t {
  Unknown = 0,
  None,
  ObjectTransform,
  ObjectPhysics,
  ObjectState,
  ObjectProperty,
  ObjectLifecycle,
  SceneState,
  Parenting,
  Animation,
  Audio,
  Camera,
  Light,
  Render,
  Window,
  TreeState,
  GameLifecycle,
  Events,
  Datablock,
  Collection,
  Navigation,
  PhysicsConstraint,
  SpawnPool,
  File,
  GlobalState,
  DebugDraw,
};

enum class LN_RuntimeSideEffectDelivery : uint8_t {
  Unknown = 0,
  None,
  DeferredCommandBuffer,
  ImmediateMainThread,
  ImmediateAndDeferred,
  RuntimeTreeState,
};

struct LN_RuntimeInstructionSemantics {
  LN_OpCode opcode = LN_OpCode::Nop;
  const char *name = "Nop";
  uint32_t reads = LN_RUNTIME_SEMANTIC_READ_NONE;
  uint32_t writes = LN_RUNTIME_SEMANTIC_WRITE_NONE;
  LN_RuntimeSemanticTiming timing = LN_RuntimeSemanticTiming::DeferredCommandFlush;
  LN_RuntimeSemanticThreading threading = LN_RuntimeSemanticThreading::WorkerSafeRecord;
  LN_RuntimeSemanticOrdering ordering = LN_RuntimeSemanticOrdering::ObservablePerTarget;
  LN_RuntimeSemanticCoalescing coalescing = LN_RuntimeSemanticCoalescing::Forbidden;
  uint32_t fallback_requirements = LN_RUNTIME_FALLBACK_NONE;
};

enum class LN_RuntimeExpressionFamily : uint8_t {
  Bool = 0,
  Float,
  Int,
  String,
  Vector,
  Color,
  Value,
  Query,
};

struct LN_RuntimeExpressionSemantics {
  LN_RuntimeExpressionFamily family = LN_RuntimeExpressionFamily::Bool;
  uint32_t kind = 0;
  const char *name = "";
  uint32_t reads = LN_RUNTIME_SEMANTIC_READ_NONE;
  uint32_t writes = LN_RUNTIME_SEMANTIC_WRITE_NONE;
  LN_RuntimeSemanticThreading threading = LN_RuntimeSemanticThreading::WorkerSafeRecord;
  bool known = false;
  uint32_t fallback_requirements = LN_RUNTIME_FALLBACK_NONE;
};

const LN_RuntimeInstructionSemantics *LN_GetRuntimeInstructionSemantics(LN_OpCode opcode);
const LN_RuntimeInstructionSemantics *LN_GetRuntimeInstructionSemanticsCatalog(size_t *r_count);
bool LN_RuntimeInstructionSemanticsCatalogHasDuplicates();
LN_RuntimeCommandFamily LN_GetRuntimeCommandFamily(LN_OpCode opcode);
LN_RuntimeSideEffectDelivery LN_GetRuntimeSideEffectDelivery(LN_OpCode opcode);

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_BoolExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_FloatExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_IntExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_StringExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_VectorExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_ColorExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_ValueExpressionKind kind);
LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(LN_QueryExpressionKind kind);
