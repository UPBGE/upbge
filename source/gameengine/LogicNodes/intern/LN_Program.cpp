/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Program.cpp
 *  \ingroup logicnodes
 */

#include "LN_Program.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "LN_RuntimeSemantics.h"

namespace {

bool OpcodeEmitsCommands(LN_OpCode opcode);
LN_MainThreadOnlyReason MainThreadOnlyReasonForOpcode(LN_OpCode opcode);
LN_MainThreadOnlyReason MainThreadOnlyReasonForBoolExpression(LN_BoolExpressionKind kind);
LN_MainThreadOnlyReason MainThreadOnlyReasonForValueExpression(LN_ValueExpressionKind kind);

void PromotePurity(LN_SchedulerSummary &summary, const LN_SchedulerPurity purity)
{
  if (uint8_t(purity) > uint8_t(summary.purity)) {
    summary.purity = purity;
  }
}

void PromoteWorkClass(LN_SchedulerSummary &summary, const LN_EstimatedWorkClass work_class)
{
  if (uint8_t(work_class) > uint8_t(summary.estimated_work_class)) {
    summary.estimated_work_class = work_class;
  }
}

void AddResourceAccess(LN_SchedulerSummary &summary, const uint32_t resource_access)
{
  summary.resource_access |= resource_access;
}

uint32_t MainThreadReasonBit(const LN_MainThreadOnlyReason reason)
{
  if (reason == LN_MainThreadOnlyReason::None) {
    return 0u;
  }
  return 1u << uint8_t(reason);
}

void AddDependencyAccess(LN_ProgramDependencySummary &summary, const uint32_t access)
{
  summary.access_classes |= access;
}

void AddSnapshotDependency(LN_ProgramDependencySummary &summary, const uint32_t channels)
{
  summary.snapshot_channels |= channels;
  AddDependencyAccess(summary, LN_DEP_ACCESS_SNAPSHOT_READ);
}

void AddInputDependency(LN_ProgramDependencySummary &summary, const uint32_t channels)
{
  summary.input_channels |= channels;
  AddDependencyAccess(summary, LN_DEP_ACCESS_IMMUTABLE_READ);
}

void AddEventDependency(LN_ProgramDependencySummary &summary, const uint32_t channels)
{
  summary.event_channels |= channels;
  AddDependencyAccess(summary, LN_DEP_ACCESS_IMMUTABLE_READ);
}

void AddQueryDependency(LN_ProgramDependencySummary &summary, const uint32_t channels)
{
  summary.query_channels |= channels;
  AddDependencyAccess(summary, LN_DEP_ACCESS_QUERY_CACHE_READ);
}

void AddCommandDependency(LN_ProgramDependencySummary &summary, const uint32_t classes)
{
  summary.command_classes |= classes;
  AddDependencyAccess(summary, LN_DEP_ACCESS_COMMAND_WRITE);
}

void AddRuntimeStateDependency(LN_ProgramDependencySummary &summary, const uint32_t state)
{
  summary.state_preservation |= state;
  AddDependencyAccess(summary, LN_DEP_ACCESS_RUNTIME_STATE);
}

void AddDynamicDependency(LN_ProgramDependencySummary &summary, const uint32_t flags)
{
  summary.dynamic_flags |= flags;
  AddDependencyAccess(summary, LN_DEP_ACCESS_DYNAMIC_UNKNOWN);
}

void AddMainThreadDependency(LN_ProgramDependencySummary &summary,
                             const LN_MainThreadOnlyReason reason)
{
  summary.worker_lane_eligible = false;
  summary.main_thread_reason_mask |= MainThreadReasonBit(reason);
  AddDependencyAccess(summary, LN_DEP_ACCESS_IMMEDIATE_MAIN_THREAD_SIDE_EFFECT);
}

uint32_t CommandClassForRuntimeFamily(const LN_RuntimeCommandFamily family)
{
  switch (family) {
    case LN_RuntimeCommandFamily::ObjectTransform:
    case LN_RuntimeCommandFamily::Parenting:
    case LN_RuntimeCommandFamily::Navigation:
      return LN_DEP_COMMAND_TRANSFORM;
    case LN_RuntimeCommandFamily::ObjectPhysics:
    case LN_RuntimeCommandFamily::PhysicsConstraint:
    case LN_RuntimeCommandFamily::SpawnPool:
      return LN_DEP_COMMAND_PHYSICS;
    case LN_RuntimeCommandFamily::ObjectProperty:
    case LN_RuntimeCommandFamily::TreeState:
    case LN_RuntimeCommandFamily::GlobalState:
      return LN_DEP_COMMAND_PROPERTY;
    case LN_RuntimeCommandFamily::Events:
      return LN_DEP_COMMAND_EVENT;
    case LN_RuntimeCommandFamily::Audio:
      return LN_DEP_COMMAND_AUDIO;
    case LN_RuntimeCommandFamily::Camera:
    case LN_RuntimeCommandFamily::Light:
    case LN_RuntimeCommandFamily::Render:
    case LN_RuntimeCommandFamily::Window:
    case LN_RuntimeCommandFamily::Animation:
    case LN_RuntimeCommandFamily::Datablock:
    case LN_RuntimeCommandFamily::Collection:
    case LN_RuntimeCommandFamily::DebugDraw:
      return LN_DEP_COMMAND_RENDER_DATABLOCK;
    case LN_RuntimeCommandFamily::ObjectLifecycle:
    case LN_RuntimeCommandFamily::SceneState:
    case LN_RuntimeCommandFamily::GameLifecycle:
      return LN_DEP_COMMAND_SCENE_LIFECYCLE;
    case LN_RuntimeCommandFamily::File:
      return LN_DEP_COMMAND_FILE_GLOBAL;
    default:
      return LN_DEP_COMMAND_NONE;
  }
}

bool StringExpressionIndexIsConstant(const std::vector<LN_StringExpression> &expressions,
                                     const uint32_t index)
{
  return index < expressions.size() && expressions[index].kind == LN_StringExpressionKind::Constant;
}

void AddSemanticReadDependencies(LN_ProgramDependencySummary &summary, const uint32_t reads)
{
  if ((reads & LN_RUNTIME_SEMANTIC_READ_SNAPSHOT) != 0u) {
    AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_OBJECT_GRAPH);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_INPUT) != 0u) {
    AddInputDependency(summary, LN_DEP_INPUT_KEYBOARD | LN_DEP_INPUT_MOUSE |
                                    LN_DEP_INPUT_GAMEPAD_BUTTON | LN_DEP_INPUT_GAMEPAD_AXIS);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_EVENT_BUS) != 0u) {
    AddEventDependency(summary, LN_DEP_EVENT_SUBJECT_READ);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_QUERY_CACHE) != 0u) {
    AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE) != 0u) {
    AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_GLOBAL_STATE) != 0u) {
    AddCommandDependency(summary, LN_DEP_COMMAND_FILE_GLOBAL);
    AddDynamicDependency(summary, LN_DEP_DYNAMIC_FILE_GLOBAL_STATE);
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_FILE) != 0u) {
    AddCommandDependency(summary, LN_DEP_COMMAND_FILE_GLOBAL);
    AddDynamicDependency(summary, LN_DEP_DYNAMIC_FILE_GLOBAL_STATE);
  }
}

void AddInstructionDependencyDetails(LN_ProgramDependencySummary &summary,
                                     const LN_Instruction &instruction,
                                     const std::vector<LN_StringExpression> &string_expressions)
{
  const LN_RuntimeCommandFamily family = LN_GetRuntimeCommandFamily(instruction.opcode);
  const uint32_t command_class = CommandClassForRuntimeFamily(family);
  if (family == LN_RuntimeCommandFamily::Unknown ||
      LN_GetRuntimeSideEffectDelivery(instruction.opcode) == LN_RuntimeSideEffectDelivery::Unknown)
  {
    AddDynamicDependency(summary, LN_DEP_DYNAMIC_AMBIGUOUS_SIDE_EFFECT);
  }
  if (command_class != LN_DEP_COMMAND_NONE &&
      (OpcodeEmitsCommands(instruction.opcode) ||
       LN_GetRuntimeSideEffectDelivery(instruction.opcode) != LN_RuntimeSideEffectDelivery::None))
  {
    AddCommandDependency(summary, command_class);
  }

  if (instruction.bool_guard_expr_index != LN_INVALID_INDEX) {
    AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
  }

  switch (instruction.opcode) {
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
      break;
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TIME_FLOW);
      break;
    case LN_OpCode::BranchRoute:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
      break;
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::InstallLogicTree:
      AddCommandDependency(summary, LN_DEP_COMMAND_RUNTIME_TREE);
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_OpCode::GamepadLook:
      AddInputDependency(summary, LN_DEP_INPUT_GAMEPAD_AXIS);
      break;
    case LN_OpCode::MouseLook:
      AddInputDependency(summary, LN_DEP_INPUT_MOUSE);
      break;
    case LN_OpCode::SendEvent:
      AddCommandDependency(summary, LN_DEP_COMMAND_EVENT);
      AddEventDependency(summary, LN_DEP_EVENT_SUBJECT_WRITE);
      if (instruction.secondary_value_expr_index != LN_INVALID_INDEX ||
          instruction.value_expr_index != LN_INVALID_INDEX ||
          instruction.int_expr_index != LN_INVALID_INDEX)
      {
        AddEventDependency(summary, LN_DEP_EVENT_TARGETED_SEND);
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      }
      if (!StringExpressionIndexIsConstant(string_expressions, instruction.string_expr_index)) {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_EVENT_SUBJECT);
      }
      break;
    case LN_OpCode::SetGameProperty:
      AddCommandDependency(summary, LN_DEP_COMMAND_PROPERTY);
      if (instruction.property_ref_index == LN_INVALID_INDEX) {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_PROPERTY_NAME);
      }
      if (instruction.value_expr_index != LN_INVALID_INDEX) {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      }
      break;
    case LN_OpCode::SetTreeProperty:
      AddCommandDependency(summary, LN_DEP_COMMAND_PROPERTY | LN_DEP_COMMAND_RUNTIME_TREE);
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      if (instruction.property_ref_index == LN_INVALID_INDEX) {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_PROPERTY_NAME);
      }
      break;
    case LN_OpCode::SetGlobalProperty:
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
    case LN_OpCode::LoadBlendFile:
      AddCommandDependency(summary, LN_DEP_COMMAND_FILE_GLOBAL);
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_FILE_GLOBAL_STATE);
      break;
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_SPAWN_POOL);
      break;
    case LN_OpCode::PlayAction:
    case LN_OpCode::StopAction:
    case LN_OpCode::SetActionFrame:
    case LN_OpCode::SetMaterialSlot:
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::ReplaceMesh:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      break;
    case LN_OpCode::AssignGeometryNodesModifier:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    default:
      break;
  }

  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForOpcode(instruction.opcode);
  if (reason != LN_MainThreadOnlyReason::None) {
    AddMainThreadDependency(summary, reason);
  }
}

void AddBoolExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                        const LN_BoolExpression &expression)
{
  constexpr int collision_contact_payload_detail = 2;
  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForBoolExpression(expression.kind);
  if (reason != LN_MainThreadOnlyReason::None) {
    if (expression.kind == LN_BoolExpressionKind::CollisionDetected ||
        expression.kind == LN_BoolExpressionKind::CollisionEnter ||
        expression.kind == LN_BoolExpressionKind::CollisionStay ||
        expression.kind == LN_BoolExpressionKind::CollisionExit)
    {
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
      AddQueryDependency(summary, LN_DEP_QUERY_COLLISION_CONTACT);
      if (expression.int_value >= collision_contact_payload_detail || expression.bool_value) {
        AddQueryDependency(summary,
                           LN_DEP_QUERY_RESULT_FIELDS | LN_DEP_QUERY_COLLISION_CONTACT_DETAILS);
      }
    }
    AddMainThreadDependency(summary, reason);
  }

  switch (expression.kind) {
    case LN_BoolExpressionKind::SnapshotGameProperty:
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GAME_PROPERTY);
      break;
    case LN_BoolExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_BoolExpressionKind::SnapshotVisibility:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_VISIBILITY);
      break;
    case LN_BoolExpressionKind::SnapshotCharacterOnGround:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_CHARACTER);
      break;
    case LN_BoolExpressionKind::InputStatus:
    case LN_BoolExpressionKind::KeyboardActive:
    case LN_BoolExpressionKind::KeyLoggerPressed:
      AddInputDependency(summary, LN_DEP_INPUT_KEYBOARD);
      break;
    case LN_BoolExpressionKind::WindowFullscreen:
      AddInputDependency(summary, LN_DEP_INPUT_WINDOW);
      break;
    case LN_BoolExpressionKind::MouseMoved:
      AddInputDependency(summary, LN_DEP_INPUT_MOUSE);
      break;
    case LN_BoolExpressionKind::MouseWheelMoved:
      AddInputDependency(summary, LN_DEP_INPUT_WHEEL);
      break;
    case LN_BoolExpressionKind::GamepadActive:
    case LN_BoolExpressionKind::GamepadButton:
      AddInputDependency(summary, LN_DEP_INPUT_GAMEPAD_BUTTON);
      break;
    case LN_BoolExpressionKind::EventReceived:
      AddEventDependency(summary, LN_DEP_EVENT_SUBJECT_READ |
                                      (expression.bool_value ? LN_DEP_EVENT_TARGETED_RECEIVE : 0u));
      if (expression.input0 == LN_INVALID_INDEX) {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_EVENT_SUBJECT);
      }
      break;
    case LN_BoolExpressionKind::PhysicsQueryDone:
    case LN_BoolExpressionKind::PhysicsQueryHit:
    case LN_BoolExpressionKind::PhysicsQueryBlocked:
    case LN_BoolExpressionKind::PhysicsQueryHasUV:
    case LN_BoolExpressionKind::PhysicsQueryStartedOverlapping:
      AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_BoolExpressionKind::MouseOverEnter:
    case LN_BoolExpressionKind::MouseOverOver:
    case LN_BoolExpressionKind::MouseOverExit:
      AddInputDependency(summary, LN_DEP_INPUT_MOUSE);
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary,
                         LN_DEP_QUERY_MOUSE_OVER | LN_DEP_QUERY_RAY |
                             LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_BoolExpressionKind::Once:
    case LN_BoolExpressionKind::OnNextTick:
    case LN_BoolExpressionKind::Timer:
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo:
    case LN_BoolExpressionKind::InstructionExecuted:
    case LN_BoolExpressionKind::InstructionReached:
    case LN_BoolExpressionKind::StoreValueDone:
    case LN_BoolExpressionKind::TweenReached:
    case LN_BoolExpressionKind::BooleanEdge:
    case LN_BoolExpressionKind::BooleanEdgeFalling:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
      break;
    case LN_BoolExpressionKind::TimerElapsed:
    case LN_BoolExpressionKind::DelayDone:
    case LN_BoolExpressionKind::PulsifyPulse:
    case LN_BoolExpressionKind::BarrierPassed:
    case LN_BoolExpressionKind::CooldownAccepted:
    case LN_BoolExpressionKind::CooldownBlocked:
    case LN_BoolExpressionKind::CooldownCompleted:
    case LN_BoolExpressionKind::CooldownReady:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TIME_FLOW);
      break;
    case LN_BoolExpressionKind::SpawnPoolSpawnedPulse:
    case LN_BoolExpressionKind::SpawnPoolHitPulse:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_SPAWN_POOL);
      break;
    case LN_BoolExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
      break;
    case LN_BoolExpressionKind::MaterialSlotFound:
    case LN_BoolExpressionKind::MaterialNodeValueFound:
    case LN_BoolExpressionKind::EditorNodeValueFound:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_BoolExpressionKind::RigidBodyAttribute:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_BoolExpressionKind::RigidBodyConstraintFound:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}

void AddFloatExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                         const LN_FloatExpression &expression)
{
  switch (expression.kind) {
    case LN_FloatExpressionKind::SnapshotGameProperty:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GAME_PROPERTY);
      break;
    case LN_FloatExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_FloatExpressionKind::SnapshotTimeScale:
    case LN_FloatExpressionKind::SnapshotElapsedTime:
    case LN_FloatExpressionKind::SnapshotFrameDelta:
    case LN_FloatExpressionKind::SnapshotFPS:
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TIMING);
      break;
    case LN_FloatExpressionKind::SnapshotLightPower:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_LIGHT);
      break;
    case LN_FloatExpressionKind::GamepadButtonStrength:
      AddInputDependency(summary, LN_DEP_INPUT_GAMEPAD_AXIS);
      break;
    case LN_FloatExpressionKind::StoreValue:
    case LN_FloatExpressionKind::TweenFactor:
    case LN_FloatExpressionKind::TweenFloatResult:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_FLOAT_EXPRESSION);
      break;
    case LN_FloatExpressionKind::CooldownRemaining:
    case LN_FloatExpressionKind::CooldownProgress:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TIME_FLOW);
      break;
    case LN_FloatExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    case LN_FloatExpressionKind::ObjectDistance:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_FloatExpressionKind::PhysicsQueryDistance:
    case LN_FloatExpressionKind::PhysicsQueryFraction:
    case LN_FloatExpressionKind::PhysicsQueryPenetrationDepth:
      AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_FloatExpressionKind::RigidBodyAttribute:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    default:
      break;
  }
}

void AddIntExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                       const LN_IntExpression &expression)
{
  switch (expression.kind) {
    case LN_IntExpressionKind::SnapshotGameProperty:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GAME_PROPERTY);
      break;
    case LN_IntExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_IntExpressionKind::SnapshotCollisionGroup:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
      break;
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
    case LN_IntExpressionKind::SnapshotCharacterJumpCount:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_CHARACTER);
      break;
    case LN_IntExpressionKind::MouseWheelDelta:
      AddInputDependency(summary, LN_DEP_INPUT_WHEEL);
      break;
    case LN_IntExpressionKind::WindowResolutionWidth:
    case LN_IntExpressionKind::WindowResolutionHeight:
    case LN_IntExpressionKind::WindowVSyncMode:
      AddInputDependency(summary, LN_DEP_INPUT_WINDOW);
      break;
    case LN_IntExpressionKind::LoopIndex:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_LOOP);
      break;
    case LN_IntExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    case LN_IntExpressionKind::MaterialSlotCount:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_IntExpressionKind::CollisionContactCount:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
      AddQueryDependency(summary, LN_DEP_QUERY_COLLISION_CONTACT | LN_DEP_QUERY_RESULT_FIELDS);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_IntExpressionKind::PhysicsQueryFaceIndex:
    case LN_IntExpressionKind::PhysicsQueryHitCount:
      AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
      break;
    default:
      break;
  }
}

void AddStringExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                          const LN_StringExpression &expression)
{
  switch (expression.kind) {
    case LN_StringExpressionKind::SnapshotGameProperty:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GAME_PROPERTY);
      break;
    case LN_StringExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_StringExpressionKind::KeyLoggerCharacter:
      AddInputDependency(summary, LN_DEP_INPUT_TEXT);
      break;
    case LN_StringExpressionKind::KeyLoggerKeycode:
      AddInputDependency(summary, LN_DEP_INPUT_KEYBOARD);
      break;
    case LN_StringExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    case LN_StringExpressionKind::MasterFolder:
      AddCommandDependency(summary, LN_DEP_COMMAND_FILE_GLOBAL);
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_FILE_GLOBAL_STATE);
      break;
    case LN_StringExpressionKind::MaterialName:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_StringExpressionKind::RigidBodyConstraintName:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}

void AddVectorExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                          const LN_VectorExpression &expression)
{
  switch (expression.kind) {
    case LN_VectorExpressionKind::SnapshotWorldPosition:
    case LN_VectorExpressionKind::SnapshotLocalPosition:
    case LN_VectorExpressionKind::SnapshotWorldOrientation:
    case LN_VectorExpressionKind::SnapshotLocalOrientation:
    case LN_VectorExpressionKind::SnapshotWorldScale:
    case LN_VectorExpressionKind::SnapshotLocalScale:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      break;
    case LN_VectorExpressionKind::SnapshotLinearVelocity:
    case LN_VectorExpressionKind::SnapshotLocalLinearVelocity:
    case LN_VectorExpressionKind::SnapshotAngularVelocity:
    case LN_VectorExpressionKind::SnapshotLocalAngularVelocity:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_VELOCITY);
      break;
    case LN_VectorExpressionKind::SnapshotGravity:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GRAVITY);
      break;
    case LN_VectorExpressionKind::WindowResolution:
      AddInputDependency(summary, LN_DEP_INPUT_WINDOW);
      break;
    case LN_VectorExpressionKind::WorldToScreen:
    case LN_VectorExpressionKind::ScreenToWorld:
    case LN_VectorExpressionKind::BoneHeadWorld:
    case LN_VectorExpressionKind::BoneTailWorld:
    case LN_VectorExpressionKind::BoneCenterWorld:
    case LN_VectorExpressionKind::BoneHeadPoseWorld:
    case LN_VectorExpressionKind::BoneTailPoseWorld:
    case LN_VectorExpressionKind::BoneCenterPoseWorld:
    case LN_VectorExpressionKind::BonePoseLocation:
    case LN_VectorExpressionKind::BonePoseScale:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM | LN_DEP_SNAPSHOT_OBJECT_GRAPH);
      break;
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_VectorExpressionKind::SnapshotCharacterGravity:
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection:
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_CHARACTER);
      break;
    case LN_VectorExpressionKind::CursorPosition:
    case LN_VectorExpressionKind::CursorMovement:
      AddInputDependency(summary, LN_DEP_INPUT_MOUSE);
      break;
    case LN_VectorExpressionKind::GamepadStick:
      AddInputDependency(summary, LN_DEP_INPUT_GAMEPAD_AXIS);
      break;
    case LN_VectorExpressionKind::PhysicsQueryPoint:
    case LN_VectorExpressionKind::PhysicsQueryNormal:
    case LN_VectorExpressionKind::PhysicsQueryCastPosition:
    case LN_VectorExpressionKind::PhysicsQueryDirection:
    case LN_VectorExpressionKind::PhysicsQueryEndPoint:
    case LN_VectorExpressionKind::PhysicsQueryUV:
      AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_VectorExpressionKind::CollisionHitPoint:
    case LN_VectorExpressionKind::CollisionHitNormal:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
      AddQueryDependency(summary,
                         LN_DEP_QUERY_COLLISION_CONTACT | LN_DEP_QUERY_RESULT_FIELDS |
                             LN_DEP_QUERY_COLLISION_CONTACT_DETAILS);
      break;
    case LN_VectorExpressionKind::SpawnPoolHitPoint:
    case LN_VectorExpressionKind::SpawnPoolHitNormal:
    case LN_VectorExpressionKind::SpawnPoolHitDirection:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
      AddQueryDependency(summary, LN_DEP_QUERY_COLLISION_CONTACT | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_VectorExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    case LN_VectorExpressionKind::GroupCenterPosition:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF | LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_VectorExpressionKind::InstructionNextPoint:
    case LN_VectorExpressionKind::TweenVectorResult:
    case LN_VectorExpressionKind::TweenRotationResult:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_FLOAT_EXPRESSION);
      break;
    case LN_VectorExpressionKind::RigidBodyAttribute:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    default:
      break;
  }
}

void AddQueryExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                         const LN_QueryExpression &expression)
{
  AddRuntimeStateDependency(summary, LN_DEP_STATE_QUERY_CACHE);
  switch (expression.kind) {
    case LN_QueryExpressionKind::Raycast:
    case LN_QueryExpressionKind::RaycastAll:
      summary.ray_query_detail_requirements |= expression.ray_query_detail_flags;
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary, LN_DEP_QUERY_RAY | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_QueryExpressionKind::ShapeCast:
    case LN_QueryExpressionKind::ShapeCastAll:
      summary.ray_query_detail_requirements |= expression.ray_query_detail_flags;
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary, LN_DEP_QUERY_RAY | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_QueryExpressionKind::MouseRay:
    case LN_QueryExpressionKind::CameraRay:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary, LN_DEP_QUERY_RAY | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_QueryExpressionKind::MouseOver:
      AddInputDependency(summary, LN_DEP_INPUT_MOUSE);
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary,
                         LN_DEP_QUERY_MOUSE_OVER | LN_DEP_QUERY_RAY |
                             LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_QueryExpressionKind::ProjectileRay:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_TRANSFORM);
      AddQueryDependency(summary, LN_DEP_QUERY_PROJECTILE | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    default:
      break;
  }
}

void AddColorExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                         const LN_ColorExpression &expression)
{
  switch (expression.kind) {
    case LN_ColorExpressionKind::SnapshotObjectColor:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLOR);
      break;
    case LN_ColorExpressionKind::SnapshotLightColor:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_LIGHT);
      break;
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_ColorExpressionKind::FromGenericValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_GENERIC_VALUE);
      break;
    default:
      break;
  }
}

void AddValueExpressionDependencyDetails(LN_ProgramDependencySummary &summary,
                                         const LN_ValueExpression &expression)
{
  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForValueExpression(expression.kind);
  if (reason == LN_MainThreadOnlyReason::GlobalPropertyPersistence ||
      reason == LN_MainThreadOnlyReason::FilePersistence)
  {
    AddCommandDependency(summary, LN_DEP_COMMAND_FILE_GLOBAL);
    AddDynamicDependency(summary, LN_DEP_DYNAMIC_FILE_GLOBAL_STATE);
    AddMainThreadDependency(summary, reason);
    return;
  }
  if (reason == LN_MainThreadOnlyReason::CollisionOrPhysicsQuery) {
    AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_COLLISION);
    AddQueryDependency(summary, LN_DEP_QUERY_COLLISION_CONTACT | LN_DEP_QUERY_RESULT_FIELDS);
    if (expression.kind == LN_ValueExpressionKind::CollisionHitPoints ||
        expression.kind == LN_ValueExpressionKind::CollisionHitNormals)
    {
      AddQueryDependency(summary, LN_DEP_QUERY_COLLISION_CONTACT_DETAILS);
    }
    AddMainThreadDependency(summary, reason);
    return;
  }

  switch (expression.kind) {
    case LN_ValueExpressionKind::SnapshotGameProperty:
    case LN_ValueExpressionKind::ObjectGameProperty:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_GAME_PROPERTY);
      break;
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_TREE_PROPERTY);
      break;
    case LN_ValueExpressionKind::OwnerObject:
    case LN_ValueExpressionKind::ObjectByName:
    case LN_ValueExpressionKind::ObjectParent:
    case LN_ValueExpressionKind::ObjectChild:
    case LN_ValueExpressionKind::ObjectChildByName:
    case LN_ValueExpressionKind::ObjectAttribute:
    case LN_ValueExpressionKind::BoneAttribute:
    case LN_ValueExpressionKind::BonePoseRotation:
    case LN_ValueExpressionKind::CurrentScene:
    case LN_ValueExpressionKind::CollectionObjects:
    case LN_ValueExpressionKind::CollectionObjectNames:
      AddSnapshotDependency(summary, LN_DEP_SNAPSHOT_OBJECT_GRAPH);
      if (expression.kind == LN_ValueExpressionKind::ObjectByName ||
          expression.kind == LN_ValueExpressionKind::ObjectChildByName)
      {
        AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      }
      break;
    case LN_ValueExpressionKind::RigidBodyConstraintNames:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_OBJECT_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_ValueExpressionKind::EventContent:
      AddEventDependency(summary, LN_DEP_EVENT_SUBJECT_READ | LN_DEP_EVENT_CONTENT_READ |
                                      (expression.bool_expr_index != LN_INVALID_INDEX ?
                                           LN_DEP_EVENT_TARGETED_RECEIVE :
                                           0u));
      break;
    case LN_ValueExpressionKind::EventMessenger:
      AddEventDependency(summary, LN_DEP_EVENT_SUBJECT_READ | LN_DEP_EVENT_MESSENGER_READ |
                                      (expression.bool_expr_index != LN_INVALID_INDEX ?
                                           LN_DEP_EVENT_TARGETED_RECEIVE :
                                           0u));
      break;
    case LN_ValueExpressionKind::PhysicsQueryObject:
    case LN_ValueExpressionKind::PhysicsQueryObjects:
    case LN_ValueExpressionKind::PhysicsQueryPoints:
    case LN_ValueExpressionKind::PhysicsQueryNormals:
    case LN_ValueExpressionKind::PhysicsQueryDistances:
    case LN_ValueExpressionKind::PhysicsQueryCastPositions:
    case LN_ValueExpressionKind::PhysicsQueryFractions:
    case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
    case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
    case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
    case LN_ValueExpressionKind::PhysicsQueryHasUVs:
    case LN_ValueExpressionKind::PhysicsQueryUVs:
      AddQueryDependency(summary, LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_ValueExpressionKind::ProjectileParabola:
      AddQueryDependency(summary, LN_DEP_QUERY_PROJECTILE | LN_DEP_QUERY_RESULT_FIELDS);
      break;
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_BOOL_EXPRESSION);
      break;
    case LN_ValueExpressionKind::LoopCurrentValue:
      AddRuntimeStateDependency(summary, LN_DEP_STATE_LOOP);
      break;
    case LN_ValueExpressionKind::MaterialSlot:
    case LN_ValueExpressionKind::MaterialNodeValue:
    case LN_ValueExpressionKind::EditorNodeValue:
      AddDynamicDependency(summary, LN_DEP_DYNAMIC_DATABLOCK_REF);
      AddMainThreadDependency(summary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}

void MarkMainThreadOnly(LN_SchedulerSummary &summary, const LN_MainThreadOnlyReason reason)
{
  PromotePurity(summary, LN_SchedulerPurity::MainThreadOnly);
  summary.worker_lane_eligible = false;
  summary.reads_snapshot_only = false;
  if (summary.main_thread_only_reason == LN_MainThreadOnlyReason::None) {
    summary.main_thread_only_reason = reason;
  }
}

void MarkSnapshotRead(LN_SchedulerSummary &summary)
{
  PromotePurity(summary, LN_SchedulerPurity::ReadsSnapshot);
  AddResourceAccess(summary, LN_SCHEDULER_RESOURCE_SNAPSHOT);
}

void MarkRuntimeTreeStateRead(LN_SchedulerSummary &summary)
{
  AddResourceAccess(summary, LN_SCHEDULER_RESOURCE_LOGIC_MANAGER);
  summary.reads_snapshot_only = false;
}

void MarkEventBusRead(LN_SchedulerSummary &summary)
{
  AddResourceAccess(summary, LN_SCHEDULER_RESOURCE_LOGIC_MANAGER);
  summary.reads_snapshot_only = false;
}

void MarkInputRead(LN_SchedulerSummary &summary)
{
  PromotePurity(summary, LN_SchedulerPurity::ReadsInput);
  AddResourceAccess(summary, LN_SCHEDULER_RESOURCE_INPUT);
}

void MarkQueryCacheRead(LN_SchedulerSummary &summary,
                        const LN_MainThreadOnlyReason reason)
{
  PromotePurity(summary, LN_SchedulerPurity::ReadsQueryCache);
  AddResourceAccess(summary,
                    LN_SCHEDULER_RESOURCE_SNAPSHOT | LN_SCHEDULER_RESOURCE_QUERY_CACHE |
                        LN_SCHEDULER_RESOURCE_PHYSICS);
  summary.uses_jolt_queries_or_contacts = true;
  MarkMainThreadOnly(summary, reason);
  PromoteWorkClass(summary, LN_EstimatedWorkClass::Heavy);
}

void MarkSnapshotQueryCacheRead(LN_SchedulerSummary &summary)
{
  PromotePurity(summary, LN_SchedulerPurity::ReadsQueryCache);
  AddResourceAccess(summary,
                    LN_SCHEDULER_RESOURCE_SNAPSHOT | LN_SCHEDULER_RESOURCE_QUERY_CACHE |
                        LN_SCHEDULER_RESOURCE_PHYSICS);
  summary.uses_jolt_queries_or_contacts = true;
  PromoteWorkClass(summary, LN_EstimatedWorkClass::Heavy);
}

void MarkCommandWrite(LN_SchedulerSummary &summary,
                      const uint32_t resource_access,
                      const LN_EstimatedWorkClass work_class = LN_EstimatedWorkClass::Small)
{
  PromotePurity(summary, LN_SchedulerPurity::WritesCommands);
  AddResourceAccess(summary, LN_SCHEDULER_RESOURCE_COMMAND_BUFFER | resource_access);
  summary.emits_commands = true;
  PromoteWorkClass(summary, work_class);
}

bool FloatEqual(const float a, const float b)
{
  return a == b;
}

bool VectorEqual(const MT_Vector3 &a, const MT_Vector3 &b)
{
  return FloatEqual(a.x(), b.x()) && FloatEqual(a.y(), b.y()) && FloatEqual(a.z(), b.z());
}

bool ColorEqual(const MT_Vector4 &a, const MT_Vector4 &b)
{
  return FloatEqual(a.x(), b.x()) && FloatEqual(a.y(), b.y()) && FloatEqual(a.z(), b.z()) &&
         FloatEqual(a.w(), b.w());
}

template<typename Expression, typename EqualsFn>
uint32_t FindEquivalentExpression(const std::vector<Expression> &expressions,
                                  const Expression &expression,
                                  const EqualsFn &equals)
{
  for (uint32_t index = 0; index < expressions.size(); index++) {
    if (equals(expressions[index], expression)) {
      return index;
    }
  }
  return LN_INVALID_INDEX;
}

LN_BoolExpression MakeConstantBoolExpression(const bool value)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Constant;
  expression.bool_value = value;
  return expression;
}

LN_FloatExpression MakeConstantFloatExpression(const float value)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::Constant;
  expression.float_value = value;
  return expression;
}

LN_IntExpression MakeConstantIntExpression(const int32_t value)
{
  LN_IntExpression expression;
  expression.kind = LN_IntExpressionKind::Constant;
  expression.int_value = value;
  return expression;
}

LN_StringExpression MakeConstantStringExpression(const std::string &value)
{
  LN_StringExpression expression;
  expression.kind = LN_StringExpressionKind::Constant;
  expression.string_value = value;
  return expression;
}

LN_VectorExpression MakeConstantVectorExpression(const MT_Vector3 &value)
{
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::Constant;
  expression.vector_value = value;
  return expression;
}

LN_ColorExpression MakeConstantColorExpression(const MT_Vector4 &value)
{
  LN_ColorExpression expression;
  expression.kind = LN_ColorExpressionKind::Constant;
  expression.color_value = value;
  return expression;
}

bool GetConstantBool(const std::vector<LN_BoolExpression> &expressions,
                     const uint32_t index,
                     bool &r_value)
{
  if (index >= expressions.size() || expressions[index].kind != LN_BoolExpressionKind::Constant) {
    return false;
  }
  r_value = expressions[index].bool_value;
  return true;
}

bool GetConstantFloat(const std::vector<LN_FloatExpression> &expressions,
                      const uint32_t index,
                      float &r_value)
{
  if (index >= expressions.size() || expressions[index].kind != LN_FloatExpressionKind::Constant) {
    return false;
  }
  r_value = expressions[index].float_value;
  return true;
}

bool GetConstantString(const std::vector<LN_StringExpression> &expressions,
                       const uint32_t index,
                       std::string &r_value)
{
  if (index >= expressions.size() || expressions[index].kind != LN_StringExpressionKind::Constant) {
    return false;
  }
  r_value = expressions[index].string_value;
  return true;
}

bool GetConstantVector(const std::vector<LN_VectorExpression> &expressions,
                       const uint32_t index,
                       MT_Vector3 &r_value)
{
  if (index >= expressions.size() || expressions[index].kind != LN_VectorExpressionKind::Constant) {
    return false;
  }
  r_value = expressions[index].vector_value;
  return true;
}

bool GetConstantColor(const std::vector<LN_ColorExpression> &expressions,
                      const uint32_t index,
                      MT_Vector4 &r_value)
{
  if (index >= expressions.size() || expressions[index].kind != LN_ColorExpressionKind::Constant) {
    return false;
  }
  r_value = expressions[index].color_value;
  return true;
}

bool BoolExpressionsEqual(const LN_BoolExpression &a, const LN_BoolExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.float_expr_index == b.float_expr_index &&
         a.secondary_float_expr_index == b.secondary_float_expr_index &&
         a.int_expr_index == b.int_expr_index && a.property_ref_index == b.property_ref_index &&
         a.float_compare_operation == b.float_compare_operation &&
         a.rigid_body_constraint_match_mode == b.rigid_body_constraint_match_mode &&
         FloatEqual(a.float_value, b.float_value) && a.int_value == b.int_value &&
         a.secondary_int_value == b.secondary_int_value && a.string_value == b.string_value &&
         a.bool_value == b.bool_value;
}

bool FloatExpressionsEqual(const LN_FloatExpression &a, const LN_FloatExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.input_indices == b.input_indices &&
         a.bool_expr_index == b.bool_expr_index && a.string_expr_index == b.string_expr_index &&
         a.property_ref_index == b.property_ref_index && a.int_value == b.int_value &&
         a.threshold_operation == b.threshold_operation && a.range_operation == b.range_operation &&
         FloatEqual(a.float_value, b.float_value) && a.bool_value == b.bool_value &&
         a.component_index == b.component_index;
}

bool IntExpressionsEqual(const LN_IntExpression &a, const LN_IntExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.property_ref_index == b.property_ref_index &&
         a.int_value == b.int_value;
}

bool StringExpressionsEqual(const LN_StringExpression &a, const LN_StringExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.input3 == b.input3 && a.input4 == b.input4 &&
         a.int_expr_index == b.int_expr_index && a.property_ref_index == b.property_ref_index &&
         a.value_expr_index == b.value_expr_index &&
         a.rigid_body_constraint_match_mode == b.rigid_body_constraint_match_mode &&
         a.string_value == b.string_value;
}

bool VectorExpressionsEqual(const LN_VectorExpression &a, const LN_VectorExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.float_expr_index == b.float_expr_index &&
         a.bool_expr_index == b.bool_expr_index && a.property_ref_index == b.property_ref_index &&
         VectorEqual(a.vector_value, b.vector_value) && FloatEqual(a.float_value, b.float_value) &&
         a.bool_value == b.bool_value;
}

bool ColorExpressionsEqual(const LN_ColorExpression &a, const LN_ColorExpression &b)
{
  return a.kind == b.kind && a.input0 == b.input0 && a.input1 == b.input1 &&
         a.input2 == b.input2 && a.input3 == b.input3 &&
         a.property_ref_index == b.property_ref_index && ColorEqual(a.color_value, b.color_value);
}

bool ValueExpressionsEqual(const LN_ValueExpression &a, const LN_ValueExpression &b)
{
  if (a.kind != LN_ValueExpressionKind::Constant || b.kind != LN_ValueExpressionKind::Constant) {
    return false;
  }
  if (a.value.type != b.value.type || a.value.exists != b.value.exists) {
    return false;
  }
  switch (a.value.type) {
    case LN_ValueType::Bool:
      return a.value.bool_value == b.value.bool_value;
    case LN_ValueType::Int:
      return a.value.int_value == b.value.int_value;
    case LN_ValueType::Float:
      return FloatEqual(a.value.float_value, b.value.float_value);
    case LN_ValueType::Vector:
      return VectorEqual(a.value.vector_value, b.value.vector_value);
    case LN_ValueType::Color:
      return ColorEqual(a.value.color_value, b.value.color_value);
    case LN_ValueType::Rotation:
      return VectorEqual(a.value.rotation_euler_value, b.value.rotation_euler_value);
    case LN_ValueType::String:
      return a.value.string_value == b.value.string_value;
    case LN_ValueType::None:
      return true;
    default:
      return false;
  }
}

bool ValueExpressionCanCSE(const LN_ValueExpression &expression)
{
  if (expression.kind != LN_ValueExpressionKind::Constant) {
    return false;
  }
  switch (expression.value.type) {
    case LN_ValueType::Bool:
    case LN_ValueType::Int:
    case LN_ValueType::Float:
    case LN_ValueType::Vector:
    case LN_ValueType::Color:
    case LN_ValueType::Rotation:
    case LN_ValueType::String:
    case LN_ValueType::None:
      return true;
    default:
      return false;
  }
}

bool BoolExpressionCanCSE(const LN_BoolExpressionKind kind)
{
  switch (kind) {
    case LN_BoolExpressionKind::Constant:
    case LN_BoolExpressionKind::Not:
    case LN_BoolExpressionKind::And:
    case LN_BoolExpressionKind::Or:
    case LN_BoolExpressionKind::FloatCompare:
    case LN_BoolExpressionKind::StringContains:
    case LN_BoolExpressionKind::StringStartsWith:
    case LN_BoolExpressionKind::StringEndsWith:
    case LN_BoolExpressionKind::ValueIsNone:
      return true;
    default:
      return false;
  }
}

bool FloatExpressionCanCSE(const LN_FloatExpressionKind kind)
{
  switch (kind) {
    case LN_FloatExpressionKind::Constant:
    case LN_FloatExpressionKind::Add:
    case LN_FloatExpressionKind::Subtract:
    case LN_FloatExpressionKind::Multiply:
    case LN_FloatExpressionKind::Divide:
    case LN_FloatExpressionKind::Power:
    case LN_FloatExpressionKind::Minimum:
    case LN_FloatExpressionKind::Maximum:
    case LN_FloatExpressionKind::Absolute:
    case LN_FloatExpressionKind::Sign:
    case LN_FloatExpressionKind::Round:
    case LN_FloatExpressionKind::Floor:
    case LN_FloatExpressionKind::Ceil:
    case LN_FloatExpressionKind::Truncate:
    case LN_FloatExpressionKind::Fraction:
    case LN_FloatExpressionKind::Modulo:
    case LN_FloatExpressionKind::Sine:
    case LN_FloatExpressionKind::Cosine:
    case LN_FloatExpressionKind::Radians:
    case LN_FloatExpressionKind::Degrees:
    case LN_FloatExpressionKind::Negate:
    case LN_FloatExpressionKind::Clamp:
    case LN_FloatExpressionKind::Threshold:
    case LN_FloatExpressionKind::RangedThreshold:
    case LN_FloatExpressionKind::VectorComponent:
    case LN_FloatExpressionKind::ColorComponent:
    case LN_FloatExpressionKind::Select:
      return true;
    default:
      return false;
  }
}

bool IntExpressionCanCSE(const LN_IntExpressionKind kind)
{
  switch (kind) {
    case LN_IntExpressionKind::Constant:
    case LN_IntExpressionKind::StringCount:
      return true;
    default:
      return false;
  }
}

bool StringExpressionCanCSE(const LN_StringExpressionKind kind)
{
  switch (kind) {
    case LN_StringExpressionKind::Constant:
    case LN_StringExpressionKind::Join:
    case LN_StringExpressionKind::Replace:
    case LN_StringExpressionKind::ToUppercase:
    case LN_StringExpressionKind::ToLowercase:
    case LN_StringExpressionKind::ZeroFill:
    case LN_StringExpressionKind::Format:
      return true;
    default:
      return false;
  }
}

bool VectorExpressionCanCSE(const LN_VectorExpressionKind kind)
{
  switch (kind) {
    case LN_VectorExpressionKind::Constant:
    case LN_VectorExpressionKind::Add:
    case LN_VectorExpressionKind::Subtract:
    case LN_VectorExpressionKind::Multiply:
    case LN_VectorExpressionKind::Divide:
    case LN_VectorExpressionKind::Absolute:
    case LN_VectorExpressionKind::Minimum:
    case LN_VectorExpressionKind::Maximum:
    case LN_VectorExpressionKind::Scale:
    case LN_VectorExpressionKind::Normalize:
    case LN_VectorExpressionKind::Resize:
    case LN_VectorExpressionKind::VectorToRotation:
    case LN_VectorExpressionKind::Combine:
      return true;
    default:
      return false;
  }
}

bool ColorExpressionCanCSE(const LN_ColorExpressionKind kind)
{
  switch (kind) {
    case LN_ColorExpressionKind::Constant:
    case LN_ColorExpressionKind::Combine:
      return true;
    default:
      return false;
  }
}

bool TryEvaluateFloatCompare(const LN_FloatCompareOperation operation,
                             const float a,
                             const float b,
                             bool &r_value)
{
  switch (operation) {
    case LN_FloatCompareOperation::Equal:
      r_value = std::fabs(a - b) <= 1.0e-6f;
      return true;
    case LN_FloatCompareOperation::NotEqual:
      r_value = std::fabs(a - b) > 1.0e-6f;
      return true;
    case LN_FloatCompareOperation::GreaterThan:
      r_value = a > b;
      return true;
    case LN_FloatCompareOperation::LessThan:
      r_value = a < b;
      return true;
    case LN_FloatCompareOperation::GreaterEqual:
      r_value = a >= b;
      return true;
    case LN_FloatCompareOperation::LessEqual:
      r_value = a <= b;
      return true;
  }
  return false;
}

LN_BoolExpression FoldBoolExpression(const LN_BoolExpression &expression,
                                     const std::vector<LN_BoolExpression> &bool_expressions,
                                     const std::vector<LN_FloatExpression> &float_expressions)
{
  bool a = false;
  bool b = false;
  float float_a = 0.0f;
  float float_b = 0.0f;
  switch (expression.kind) {
    case LN_BoolExpressionKind::Not:
      if (GetConstantBool(bool_expressions, expression.input0, a)) {
        return MakeConstantBoolExpression(!a);
      }
      break;
    case LN_BoolExpressionKind::And:
      if (GetConstantBool(bool_expressions, expression.input0, a) &&
          GetConstantBool(bool_expressions, expression.input1, b))
      {
        return MakeConstantBoolExpression(a && b);
      }
      break;
    case LN_BoolExpressionKind::Or:
      if (GetConstantBool(bool_expressions, expression.input0, a) &&
          GetConstantBool(bool_expressions, expression.input1, b))
      {
        return MakeConstantBoolExpression(a || b);
      }
      break;
    case LN_BoolExpressionKind::FloatCompare:
      if (GetConstantFloat(float_expressions, expression.input0, float_a) &&
          GetConstantFloat(float_expressions, expression.input1, float_b) &&
          TryEvaluateFloatCompare(expression.float_compare_operation, float_a, float_b, a))
      {
        return MakeConstantBoolExpression(a);
      }
      break;
    default:
      break;
  }
  return expression;
}

LN_FloatExpression FoldFloatExpression(const LN_FloatExpression &expression,
                                       const std::vector<LN_BoolExpression> &bool_expressions,
                                       const std::vector<LN_FloatExpression> &float_expressions,
                                       const std::vector<LN_VectorExpression> &vector_expressions,
                                       const std::vector<LN_ColorExpression> &color_expressions)
{
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  bool condition = false;
  MT_Vector3 vector;
  MT_Vector4 color;
  switch (expression.kind) {
    case LN_FloatExpressionKind::Add:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(a + b);
      }
      break;
    case LN_FloatExpressionKind::Subtract:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(a - b);
      }
      break;
    case LN_FloatExpressionKind::Multiply:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(a * b);
      }
      break;
    case LN_FloatExpressionKind::Divide:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(std::fabs(b) <= 1.0e-20f ? 0.0f : a / b);
      }
      break;
    case LN_FloatExpressionKind::Minimum:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(std::min(a, b));
      }
      break;
    case LN_FloatExpressionKind::Maximum:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b)) {
        return MakeConstantFloatExpression(std::max(a, b));
      }
      break;
    case LN_FloatExpressionKind::Absolute:
      if (GetConstantFloat(float_expressions, expression.input0, a)) {
        return MakeConstantFloatExpression(std::fabs(a));
      }
      break;
    case LN_FloatExpressionKind::Negate:
      if (GetConstantFloat(float_expressions, expression.input0, a)) {
        return MakeConstantFloatExpression(-a);
      }
      break;
    case LN_FloatExpressionKind::Clamp:
      if (GetConstantFloat(float_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.input1, b) &&
          GetConstantFloat(float_expressions, expression.input2, c))
      {
        const float lower = std::min(b, c);
        const float upper = std::max(b, c);
        return MakeConstantFloatExpression(std::min(std::max(a, lower), upper));
      }
      break;
    case LN_FloatExpressionKind::VectorComponent:
      if (GetConstantVector(vector_expressions, expression.input0, vector)) {
        if (expression.component_index == 0) {
          return MakeConstantFloatExpression(vector.x());
        }
        if (expression.component_index == 1) {
          return MakeConstantFloatExpression(vector.y());
        }
        if (expression.component_index == 2) {
          return MakeConstantFloatExpression(vector.z());
        }
      }
      break;
    case LN_FloatExpressionKind::ColorComponent:
      if (GetConstantColor(color_expressions, expression.input0, color)) {
        if (expression.component_index == 0) {
          return MakeConstantFloatExpression(color.x());
        }
        if (expression.component_index == 1) {
          return MakeConstantFloatExpression(color.y());
        }
        if (expression.component_index == 2) {
          return MakeConstantFloatExpression(color.z());
        }
        if (expression.component_index == 3) {
          return MakeConstantFloatExpression(color.w());
        }
      }
      break;
    case LN_FloatExpressionKind::Select:
      if (GetConstantBool(bool_expressions, expression.bool_expr_index, condition)) {
        const uint32_t input_index = condition ? expression.input0 : expression.input1;
        if (GetConstantFloat(float_expressions, input_index, a)) {
          return MakeConstantFloatExpression(a);
        }
      }
      break;
    default:
      break;
  }
  return expression;
}

static int32_t CountStringOccurrences(const std::string &text, const std::string &needle)
{
  if (needle.empty()) {
    return 0;
  }
  int32_t count = 0;
  size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos) {
    count++;
    offset += needle.size();
  }
  return count;
}

LN_IntExpression FoldIntExpression(const LN_IntExpression &expression,
                                   const std::vector<LN_StringExpression> &string_expressions)
{
  std::string text;
  std::string needle;
  if (expression.kind == LN_IntExpressionKind::StringCount &&
      GetConstantString(string_expressions, expression.input0, text) &&
      GetConstantString(string_expressions, expression.input1, needle))
  {
    return MakeConstantIntExpression(CountStringOccurrences(text, needle));
  }
  return expression;
}

LN_StringExpression FoldStringExpression(const LN_StringExpression &expression,
                                         const std::vector<LN_StringExpression> &string_expressions)
{
  std::string a;
  std::string b;
  if (expression.kind == LN_StringExpressionKind::Join &&
      GetConstantString(string_expressions, expression.input0, a) &&
      GetConstantString(string_expressions, expression.input1, b))
  {
    return MakeConstantStringExpression(a + b);
  }
  return expression;
}

LN_VectorExpression FoldVectorExpression(
    const LN_VectorExpression &expression,
    const std::vector<LN_FloatExpression> &float_expressions,
    const std::vector<LN_VectorExpression> &vector_expressions)
{
  MT_Vector3 a;
  MT_Vector3 b;
  float scalar = 0.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  switch (expression.kind) {
    case LN_VectorExpressionKind::Add:
      if (GetConstantVector(vector_expressions, expression.input0, a) &&
          GetConstantVector(vector_expressions, expression.input1, b)) {
        return MakeConstantVectorExpression(a + b);
      }
      break;
    case LN_VectorExpressionKind::Subtract:
      if (GetConstantVector(vector_expressions, expression.input0, a) &&
          GetConstantVector(vector_expressions, expression.input1, b)) {
        return MakeConstantVectorExpression(a - b);
      }
      break;
    case LN_VectorExpressionKind::Scale:
      if (GetConstantVector(vector_expressions, expression.input0, a) &&
          GetConstantFloat(float_expressions, expression.float_expr_index, scalar)) {
        return MakeConstantVectorExpression(a * scalar);
      }
      break;
    case LN_VectorExpressionKind::Normalize:
      if (GetConstantVector(vector_expressions, expression.input0, a)) {
        const float length = a.length();
        return MakeConstantVectorExpression(length <= 1.0e-20f ?
                  MT_Vector3(0.0f, 0.0f, 0.0f) :
                  a * (1.0f / length));
      }
      break;
    case LN_VectorExpressionKind::Combine:
      if (GetConstantFloat(float_expressions, expression.input0, x) &&
          GetConstantFloat(float_expressions, expression.input1, y) &&
          GetConstantFloat(float_expressions, expression.input2, z))
      {
        return MakeConstantVectorExpression(MT_Vector3(x, y, z));
      }
      break;
    default:
      break;
  }
  return expression;
}

LN_ColorExpression FoldColorExpression(const LN_ColorExpression &expression,
                                       const std::vector<LN_FloatExpression> &float_expressions)
{
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
  if (expression.kind == LN_ColorExpressionKind::Combine &&
      GetConstantFloat(float_expressions, expression.input0, r) &&
      GetConstantFloat(float_expressions, expression.input1, g) &&
      GetConstantFloat(float_expressions, expression.input2, b) &&
      GetConstantFloat(float_expressions, expression.input3, a))
  {
    return MakeConstantColorExpression(MT_Vector4(r, g, b, a));
  }
  return expression;
}

LN_MainThreadOnlyReason MainThreadOnlyReasonForOpcode(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::AddObject:
    case LN_OpCode::AssignGeometryNodesModifier:
      return LN_MainThreadOnlyReason::ImmediateCommandHelper;
    case LN_OpCode::RemoveObject:
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
      return LN_MainThreadOnlyReason::ObjectLifecycleCommand;
    case LN_OpCode::SetGlobalProperty:
      return LN_MainThreadOnlyReason::GlobalPropertyPersistence;
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
      return LN_MainThreadOnlyReason::FilePersistence;
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
      return LN_MainThreadOnlyReason::PhysicsConstraint;
    case LN_OpCode::MouseLook:
      return LN_MainThreadOnlyReason::InputDeviceState;
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return LN_MainThreadOnlyReason::SpawnPool;
    case LN_OpCode::SetCollectionVisibility:
      return LN_MainThreadOnlyReason::CollectionVisibility;
    case LN_OpCode::ApplyForceToTarget:
      return LN_MainThreadOnlyReason::QueryExpression;
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
      return LN_MainThreadOnlyReason::OverlayCollection;
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
      return LN_MainThreadOnlyReason::DebugDraw;
    default:
      return LN_MainThreadOnlyReason::None;
  }
}

LN_InstructionPayloadKind PayloadKindForOpcode(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::Translate:
    case LN_OpCode::MoveToward:
    case LN_OpCode::SlowFollow:
    case LN_OpCode::AlignAxisToVector:
    case LN_OpCode::RotateToward:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::ApplyTransformVector:
      return LN_InstructionPayloadKind::Transform;
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::SetCharacterWalkDirection:
    case LN_OpCode::SetCharacterVelocity:
    case LN_OpCode::SetVelocityVector:
      return LN_InstructionPayloadKind::Velocity;
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyForceToTarget:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::SetGravity:
    case LN_OpCode::SetCollisionGroup:
    case LN_OpCode::SetPhysics:
    case LN_OpCode::SetDynamics:
    case LN_OpCode::RebuildCollisionShape:
    case LN_OpCode::SetRigidBodyAttribute:
    case LN_OpCode::CharacterJump:
    case LN_OpCode::SetCharacterGravity:
    case LN_OpCode::SetCharacterJumpSpeed:
    case LN_OpCode::SetCharacterMaxJumps:
    case LN_OpCode::VehicleControl:
    case LN_OpCode::VehicleApplyEngineForce:
    case LN_OpCode::VehicleApplyBraking:
    case LN_OpCode::VehicleApplySteering:
    case LN_OpCode::SetVehicleSuspensionCompression:
    case LN_OpCode::SetVehicleSuspensionStiffness:
    case LN_OpCode::SetVehicleSuspensionDamping:
    case LN_OpCode::SetVehicleWheelFriction:
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::ApplyPhysicsVector:
      return LN_InstructionPayloadKind::Physics;
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::SetTreeProperty:
    case LN_OpCode::SetGlobalProperty:
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::CopyProperty:
      return LN_InstructionPayloadKind::Property;
    case LN_OpCode::AddObject:
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
    case LN_OpCode::RemoveObject:
    case LN_OpCode::SetActiveCamera:
    case LN_OpCode::SetCameraFov:
    case LN_OpCode::SetCameraOrthoScale:
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
    case LN_OpCode::LoadScene:
    case LN_OpCode::SetScene:
    case LN_OpCode::ReplaceMesh:
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return LN_InstructionPayloadKind::Scene;
    case LN_OpCode::StopAllSounds:
    case LN_OpCode::PlaySound:
    case LN_OpCode::PlaySound3D:
    case LN_OpCode::PauseSound:
    case LN_OpCode::ResumeSound:
    case LN_OpCode::StopSound:
      return LN_InstructionPayloadKind::Audio;
    case LN_OpCode::SendEvent:
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
      return LN_InstructionPayloadKind::Event;
    case LN_OpCode::LoadBlendFile:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
      return LN_InstructionPayloadKind::Persistence;
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
      return LN_InstructionPayloadKind::Navigation;
    case LN_OpCode::SetVisibility:
    case LN_OpCode::SetObjectColor:
    case LN_OpCode::MakeLightUnique:
    case LN_OpCode::SetLightColor:
    case LN_OpCode::SetLightPower:
    case LN_OpCode::SetLightShadow:
    case LN_OpCode::SetCursorVisibility:
    case LN_OpCode::SetCursorPosition:
    case LN_OpCode::SetFullscreen:
    case LN_OpCode::SetVSync:
    case LN_OpCode::SetShowFramerate:
    case LN_OpCode::SetShowProfile:
    case LN_OpCode::SetBonePoseLocation:
    case LN_OpCode::SetBonePoseRotation:
    case LN_OpCode::SetBonePoseScale:
    case LN_OpCode::SetBonePoseTransform:
    case LN_OpCode::SetBoneAttribute:
    case LN_OpCode::SetBoneConstraintInfluence:
    case LN_OpCode::SetBoneConstraintTarget:
    case LN_OpCode::SetBoneConstraintAttribute:
    case LN_OpCode::SetMaterialSlot:
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::AssignGeometryNodesModifier:
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
      return LN_InstructionPayloadKind::Render;
    case LN_OpCode::Nop:
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::BranchRoute:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
      return LN_InstructionPayloadKind::None;
    default:
      return LN_InstructionPayloadKind::Generic;
  }
}

bool OpcodeUsesVectorCommandPayload(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::SetGravity:
    case LN_OpCode::SetCharacterGravity:
    case LN_OpCode::SetCharacterWalkDirection:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyTransformVector:
    case LN_OpCode::ApplyPhysicsVector:
      return true;
    default:
      return false;
  }
}

bool InstructionStoresGamePropertyTypedValue(const LN_Instruction &instruction)
{
  return instruction.property_value_type != LN_ValueType::None &&
         (instruction.bool_expr_index != LN_INVALID_INDEX ||
          instruction.int_expr_index != LN_INVALID_INDEX ||
          instruction.float_expr_index != LN_INVALID_INDEX ||
          instruction.string_expr_index != LN_INVALID_INDEX ||
          instruction.vector_expr_index != LN_INVALID_INDEX ||
          instruction.color_expr_index != LN_INVALID_INDEX);
}

LN_VectorCommandPayload VectorCommandPayloadFromInstruction(const LN_Instruction &instruction)
{
  LN_VectorCommandPayload payload;
  payload.object_value_expr_index = instruction.value_expr_index;
  payload.vector_expr_index = instruction.vector_expr_index;
  payload.secondary_vector_expr_index = instruction.secondary_vector_expr_index;
  payload.bool_expr_index = instruction.bool_expr_index;
  payload.vector_value = instruction.vector_value;
  payload.secondary_vector_value = instruction.secondary_vector_value;
  payload.operation_mode = instruction.vector_operation_mode;
  payload.operation_channel = instruction.vector_operation_channel;
  payload.operation_mask = instruction.vector_operation_mask;
  payload.bool_value = instruction.bool_value;
  return payload;
}

LN_GamePropertyCommandPayload GamePropertyCommandPayloadFromInstruction(
    const LN_Instruction &instruction)
{
  LN_GamePropertyCommandPayload payload;
  payload.property_ref_index = instruction.property_ref_index;
  payload.value_type = instruction.property_value_type;
  payload.bool_expr_index = instruction.bool_expr_index;
  payload.int_expr_index = instruction.int_expr_index;
  payload.float_expr_index = instruction.float_expr_index;
  payload.string_expr_index = instruction.string_expr_index;
  payload.vector_expr_index = instruction.vector_expr_index;
  payload.color_expr_index = instruction.color_expr_index;

  if (InstructionStoresGamePropertyTypedValue(instruction)) {
    payload.object_value_expr_index = instruction.value_expr_index;
  }
  else {
    payload.value_expr_index = instruction.value_expr_index;
  }
  return payload;
}

uint32_t CommandResourceAccessForOpcode(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::Translate:
    case LN_OpCode::MoveToward:
    case LN_OpCode::SlowFollow:
    case LN_OpCode::AlignAxisToVector:
    case LN_OpCode::RotateToward:
    case LN_OpCode::CopyProperty:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::ApplyTransformVector:
      return LN_SCHEDULER_RESOURCE_WORLD;
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::SetCollisionGroup:
    case LN_OpCode::SetPhysics:
    case LN_OpCode::SetDynamics:
    case LN_OpCode::RebuildCollisionShape:
    case LN_OpCode::SetRigidBodyAttribute:
    case LN_OpCode::CharacterJump:
    case LN_OpCode::SetCharacterGravity:
    case LN_OpCode::SetCharacterJumpSpeed:
    case LN_OpCode::SetCharacterMaxJumps:
    case LN_OpCode::SetCharacterWalkDirection:
    case LN_OpCode::SetCharacterVelocity:
    case LN_OpCode::VehicleControl:
    case LN_OpCode::VehicleApplyEngineForce:
    case LN_OpCode::VehicleApplyBraking:
    case LN_OpCode::VehicleApplySteering:
    case LN_OpCode::SetVehicleSuspensionCompression:
    case LN_OpCode::SetVehicleSuspensionStiffness:
    case LN_OpCode::SetVehicleSuspensionDamping:
    case LN_OpCode::SetVehicleWheelFriction:
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyPhysicsVector:
      return LN_SCHEDULER_RESOURCE_PHYSICS;
    case LN_OpCode::ApplyForceToTarget:
      return LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_PHYSICS;
    case LN_OpCode::AssignGeometryNodesModifier:
      return LN_SCHEDULER_RESOURCE_RENDER | LN_SCHEDULER_RESOURCE_LOGIC_MANAGER |
             LN_SCHEDULER_RESOURCE_DATABLOCK;
    case LN_OpCode::SetVisibility:
    case LN_OpCode::SetObjectColor:
    case LN_OpCode::MakeLightUnique:
    case LN_OpCode::SetLightColor:
    case LN_OpCode::SetLightPower:
    case LN_OpCode::SetLightShadow:
    case LN_OpCode::SetCameraFov:
    case LN_OpCode::SetCameraOrthoScale:
    case LN_OpCode::SetWindowSize:
    case LN_OpCode::SetFullscreen:
    case LN_OpCode::SetVSync:
    case LN_OpCode::SetShowFramerate:
    case LN_OpCode::SetShowProfile:
    case LN_OpCode::SetCursorVisibility:
    case LN_OpCode::SetCursorPosition:
    case LN_OpCode::SetMaterialSlot:
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::ReplaceMesh:
      return LN_SCHEDULER_RESOURCE_RENDER;
    case LN_OpCode::SetGravity:
    case LN_OpCode::SetActiveCamera:
    case LN_OpCode::LoadScene:
    case LN_OpCode::SetScene:
      return LN_SCHEDULER_RESOURCE_SCENE;
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_SCENE |
             LN_SCHEDULER_RESOURCE_PHYSICS;
    case LN_OpCode::SetTimeScale:
      return LN_SCHEDULER_RESOURCE_GLOBAL_STATE;
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::SetTreeProperty:
      return LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_GLOBAL_STATE;
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
    case LN_OpCode::RemoveObject:
    case LN_OpCode::AddObject:
      return LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_SCENE;
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
    case LN_OpCode::SendEvent:
      return LN_SCHEDULER_RESOURCE_LOGIC_MANAGER;
    case LN_OpCode::PlayAction:
    case LN_OpCode::StopAction:
    case LN_OpCode::SetActionFrame:
    case LN_OpCode::SetBonePoseLocation:
    case LN_OpCode::SetBonePoseRotation:
    case LN_OpCode::SetBonePoseScale:
    case LN_OpCode::SetBonePoseTransform:
    case LN_OpCode::SetBoneAttribute:
    case LN_OpCode::SetBoneConstraintInfluence:
    case LN_OpCode::SetBoneConstraintTarget:
    case LN_OpCode::SetBoneConstraintAttribute:
      return LN_SCHEDULER_RESOURCE_DATABLOCK;
    case LN_OpCode::StopAllSounds:
    case LN_OpCode::PlaySound:
    case LN_OpCode::PlaySound3D:
    case LN_OpCode::PauseSound:
    case LN_OpCode::ResumeSound:
    case LN_OpCode::StopSound:
      return LN_SCHEDULER_RESOURCE_AUDIO;
    case LN_OpCode::LoadBlendFile:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
      return LN_SCHEDULER_RESOURCE_FILE;
    case LN_OpCode::QuitGame:
    case LN_OpCode::RestartGame:
    case LN_OpCode::Print:
    case LN_OpCode::SetGamepadVibration:
      return LN_SCHEDULER_RESOURCE_GLOBAL_STATE;
    case LN_OpCode::GamepadLook:
    case LN_OpCode::MouseLook:
      return LN_SCHEDULER_RESOURCE_WORLD;
    default:
      return LN_SCHEDULER_RESOURCE_NONE;
  }
}

bool OpcodeEmitsCommands(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::Nop:
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::BranchRoute:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
    case LN_OpCode::RunLogicTreeOnce:
      return false;
    case LN_OpCode::AddObject:
    case LN_OpCode::RemoveObject:
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
    case LN_OpCode::Navigate:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::ApplyForceToTarget:
    case LN_OpCode::MouseLook:
      return true;
    default:
      break;
  }
  if (MainThreadOnlyReasonForOpcode(opcode) != LN_MainThreadOnlyReason::None) {
    return false;
  }
  return true;
}

uint32_t SchedulerResourcesForSemanticReads(const uint32_t reads)
{
  uint32_t resources = LN_SCHEDULER_RESOURCE_NONE;
  if ((reads & LN_RUNTIME_SEMANTIC_READ_SNAPSHOT) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_SNAPSHOT;
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_INPUT) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_INPUT;
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_EVENT_BUS) != 0u ||
      (reads & LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE) != 0u)
  {
    resources |= LN_SCHEDULER_RESOURCE_LOGIC_MANAGER;
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_QUERY_CACHE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_QUERY_CACHE | LN_SCHEDULER_RESOURCE_PHYSICS;
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_FILE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_FILE;
  }
  if ((reads & LN_RUNTIME_SEMANTIC_READ_GLOBAL_STATE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_GLOBAL_STATE;
  }
  return resources;
}

uint32_t SchedulerResourcesForSemanticObjectWrite(const LN_RuntimeCommandFamily family)
{
  switch (family) {
    case LN_RuntimeCommandFamily::ObjectPhysics:
    case LN_RuntimeCommandFamily::PhysicsConstraint:
      return LN_SCHEDULER_RESOURCE_PHYSICS;
    case LN_RuntimeCommandFamily::SpawnPool:
      return LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_PHYSICS;
    case LN_RuntimeCommandFamily::Animation:
    case LN_RuntimeCommandFamily::Datablock:
      return LN_SCHEDULER_RESOURCE_DATABLOCK;
    case LN_RuntimeCommandFamily::Camera:
    case LN_RuntimeCommandFamily::Light:
    case LN_RuntimeCommandFamily::Render:
    case LN_RuntimeCommandFamily::Window:
    case LN_RuntimeCommandFamily::DebugDraw:
      return LN_SCHEDULER_RESOURCE_RENDER;
    default:
      return LN_SCHEDULER_RESOURCE_WORLD;
  }
}

uint32_t MinimumSchedulerResourcesForInstructionSemantics(
    const LN_RuntimeInstructionSemantics &semantics,
    const LN_RuntimeCommandFamily family)
{
  uint32_t resources = SchedulerResourcesForSemanticReads(semantics.reads);
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_COMMAND_BUFFER;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_OBJECT_STATE) != 0u) {
    resources |= SchedulerResourcesForSemanticObjectWrite(family);
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_SCENE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_SCENE;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_AUDIO) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_AUDIO;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_FILE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_FILE;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_GLOBAL_STATE) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_GLOBAL_STATE;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_RENDER) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_RENDER;
  }
  if ((semantics.writes & LN_RUNTIME_SEMANTIC_WRITE_EVENT_BUS) != 0u) {
    resources |= LN_SCHEDULER_RESOURCE_LOGIC_MANAGER;
  }
  return resources;
}

uint32_t MinimumSchedulerResourcesForExpressionSemantics(
    const LN_RuntimeExpressionSemantics &semantics)
{
  return SchedulerResourcesForSemanticReads(semantics.reads);
}

LN_MainThreadOnlyReason MainThreadOnlyReasonForBoolExpression(
    const LN_BoolExpressionKind kind)
{
  switch (kind) {
    case LN_BoolExpressionKind::CollisionDetected:
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit:
    case LN_BoolExpressionKind::ObjectsColliding:
      return LN_MainThreadOnlyReason::CollisionOrPhysicsQuery;
    default:
      return LN_MainThreadOnlyReason::None;
  }
}

LN_MainThreadOnlyReason MainThreadOnlyReasonForValueExpression(
    const LN_ValueExpressionKind kind)
{
  switch (kind) {
    case LN_ValueExpressionKind::GetGlobalProperty:
    case LN_ValueExpressionKind::ListGlobalProperties:
      return LN_MainThreadOnlyReason::GlobalPropertyPersistence;
    case LN_ValueExpressionKind::LoadVariable:
    case LN_ValueExpressionKind::LoadVariableDict:
    case LN_ValueExpressionKind::ListSavedVariables:
      return LN_MainThreadOnlyReason::FilePersistence;
    case LN_ValueExpressionKind::CollisionHitObject:
    case LN_ValueExpressionKind::CollisionHitObjects:
    case LN_ValueExpressionKind::CollisionHitPoints:
    case LN_ValueExpressionKind::CollisionHitNormals:
      return LN_MainThreadOnlyReason::CollisionOrPhysicsQuery;
    default:
      return LN_MainThreadOnlyReason::None;
  }
}

const char *SchedulerPurityName(const LN_SchedulerPurity purity)
{
  switch (purity) {
    case LN_SchedulerPurity::Pure:
      return "Pure";
    case LN_SchedulerPurity::ReadsSnapshot:
      return "ReadsSnapshot";
    case LN_SchedulerPurity::ReadsInput:
      return "ReadsInput";
    case LN_SchedulerPurity::ReadsQueryCache:
      return "ReadsQueryCache";
    case LN_SchedulerPurity::WritesCommands:
      return "WritesCommands";
    case LN_SchedulerPurity::MainThreadOnly:
      return "MainThreadOnly";
  }
  return "Unknown";
}

const char *RequiredPhaseName(const LN_SchedulerRequiredPhase phase)
{
  switch (phase) {
    case LN_SchedulerRequiredPhase::None:
      return "None";
    case LN_SchedulerRequiredPhase::OnInit:
      return "OnInit";
    case LN_SchedulerRequiredPhase::OnFixedUpdate:
      return "OnFixedUpdate";
  }
  return "Unknown";
}

const char *EstimatedWorkClassName(const LN_EstimatedWorkClass work_class)
{
  switch (work_class) {
    case LN_EstimatedWorkClass::Trivial:
      return "Trivial";
    case LN_EstimatedWorkClass::Small:
      return "Small";
    case LN_EstimatedWorkClass::Medium:
      return "Medium";
    case LN_EstimatedWorkClass::Heavy:
      return "Heavy";
  }
  return "Unknown";
}

const char *MainThreadOnlyReasonName(const LN_MainThreadOnlyReason reason)
{
  switch (reason) {
    case LN_MainThreadOnlyReason::None:
      return "None";
    case LN_MainThreadOnlyReason::QueryExpression:
      return "QueryExpression";
    case LN_MainThreadOnlyReason::CollisionOrPhysicsQuery:
      return "CollisionOrPhysicsQuery";
    case LN_MainThreadOnlyReason::MouseOverQuery:
      return "MouseOverQuery";
    case LN_MainThreadOnlyReason::ImmediateCommandHelper:
      return "ImmediateCommandHelper";
    case LN_MainThreadOnlyReason::ObjectLifecycleCommand:
      return "ObjectLifecycleCommand";
    case LN_MainThreadOnlyReason::GlobalPropertyPersistence:
      return "GlobalPropertyPersistence";
    case LN_MainThreadOnlyReason::FilePersistence:
      return "FilePersistence";
    case LN_MainThreadOnlyReason::PhysicsConstraint:
      return "PhysicsConstraint";
    case LN_MainThreadOnlyReason::SpawnPool:
      return "SpawnPool";
    case LN_MainThreadOnlyReason::CollectionVisibility:
      return "CollectionVisibility";
    case LN_MainThreadOnlyReason::OverlayCollection:
      return "OverlayCollection";
    case LN_MainThreadOnlyReason::InputDeviceState:
      return "InputDeviceState";
    case LN_MainThreadOnlyReason::DebugDraw:
      return "DebugDraw";
  }
  return "Unknown";
}

void AppendResourceAccessName(std::ostringstream &stream,
                              const uint32_t resource_access,
                              const uint32_t flag,
                              const char *name,
                              bool &first)
{
  if ((resource_access & flag) == 0u) {
    return;
  }
  if (!first) {
    stream << ",";
  }
  stream << name;
  first = false;
}

void AppendResourceAccessNames(std::ostringstream &stream, const uint32_t resource_access)
{
  bool first = true;
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_SNAPSHOT,
                           "Snapshot",
                           first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_INPUT, "Input", first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_QUERY_CACHE,
                           "QueryCache",
                           first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_COMMAND_BUFFER,
                           "CommandBuffer",
                           first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_WORLD, "World", first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_SCENE, "Scene", first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_PHYSICS,
                           "Physics",
                           first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_RENDER, "Render", first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_AUDIO, "Audio", first);
  AppendResourceAccessName(stream, resource_access, LN_SCHEDULER_RESOURCE_FILE, "File", first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_GLOBAL_STATE,
                           "GlobalState",
                           first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_LOGIC_MANAGER,
                           "LogicManager",
                           first);
  AppendResourceAccessName(stream,
                           resource_access,
                           LN_SCHEDULER_RESOURCE_DATABLOCK,
                           "Datablock",
                           first);
  if (first) {
    stream << "None";
  }
}

uint32_t CurrentRuntimeBuildFlags()
{
  uint32_t flags = 0;
#ifdef WITH_GAMEENGINE_LOGICNODES
  flags |= 1u << 0;
#endif
#ifdef WITH_AUDASPACE
  flags |= 1u << 1;
#endif
  return flags;
}

LN_ExecOpKind ExecOpKindForOpcode(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::Nop:
      return LN_ExecOpKind::Nop;
    case LN_OpCode::BranchRoute:
      return LN_ExecOpKind::BranchRoute;
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
      return LN_ExecOpKind::TimeFlowControl;
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyTransformVector:
    case LN_OpCode::ApplyPhysicsVector:
      return LN_ExecOpKind::VectorCommand;
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyForceToTarget:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::Translate:
    case LN_OpCode::MoveToward:
    case LN_OpCode::SlowFollow:
    case LN_OpCode::AlignAxisToVector:
    case LN_OpCode::RotateToward:
    case LN_OpCode::SetCollisionGroup:
    case LN_OpCode::SetPhysics:
    case LN_OpCode::SetDynamics:
    case LN_OpCode::RebuildCollisionShape:
    case LN_OpCode::SetRigidBodyAttribute:
    case LN_OpCode::SetGravity:
    case LN_OpCode::CharacterJump:
    case LN_OpCode::SetCharacterGravity:
    case LN_OpCode::SetCharacterJumpSpeed:
    case LN_OpCode::SetCharacterMaxJumps:
    case LN_OpCode::SetCharacterWalkDirection:
    case LN_OpCode::SetCharacterVelocity:
    case LN_OpCode::VehicleControl:
    case LN_OpCode::VehicleApplyEngineForce:
    case LN_OpCode::VehicleApplyBraking:
    case LN_OpCode::VehicleApplySteering:
    case LN_OpCode::SetVehicleSuspensionCompression:
    case LN_OpCode::SetVehicleSuspensionStiffness:
    case LN_OpCode::SetVehicleSuspensionDamping:
    case LN_OpCode::SetVehicleWheelFriction:
      return LN_ExecOpKind::PhysicsCommand;
    case LN_OpCode::SetVisibility:
      return LN_ExecOpKind::ObjectStateCommand;
    case LN_OpCode::SetObjectColor:
      return LN_ExecOpKind::ObjectColorCommand;
    case LN_OpCode::SetActiveCamera:
    case LN_OpCode::SetCameraFov:
    case LN_OpCode::SetCameraOrthoScale:
      return LN_ExecOpKind::CameraCommand;
    case LN_OpCode::MakeLightUnique:
    case LN_OpCode::SetLightColor:
    case LN_OpCode::SetLightPower:
    case LN_OpCode::SetLightShadow:
      return LN_ExecOpKind::LightCommand;
    case LN_OpCode::SetWindowSize:
    case LN_OpCode::SetFullscreen:
    case LN_OpCode::SetVSync:
    case LN_OpCode::SetShowFramerate:
    case LN_OpCode::SetShowProfile:
    case LN_OpCode::SetCursorVisibility:
    case LN_OpCode::SetCursorPosition:
    case LN_OpCode::SetGamepadVibration:
      return LN_ExecOpKind::WindowCommand;
    case LN_OpCode::GamepadLook:
    case LN_OpCode::MouseLook:
      return LN_ExecOpKind::InputMotionCommand;
    case LN_OpCode::StopAllSounds:
    case LN_OpCode::PlaySound:
    case LN_OpCode::PlaySound3D:
    case LN_OpCode::PauseSound:
    case LN_OpCode::ResumeSound:
    case LN_OpCode::StopSound:
      return LN_ExecOpKind::AudioCommand;
    case LN_OpCode::PlayAction:
    case LN_OpCode::StopAction:
    case LN_OpCode::SetActionFrame:
      return LN_ExecOpKind::ActionCommand;
    case LN_OpCode::SetBonePoseLocation:
    case LN_OpCode::SetBonePoseRotation:
    case LN_OpCode::SetBonePoseScale:
    case LN_OpCode::SetBonePoseTransform:
    case LN_OpCode::SetBoneAttribute:
    case LN_OpCode::SetBoneConstraintInfluence:
    case LN_OpCode::SetBoneConstraintTarget:
    case LN_OpCode::SetBoneConstraintAttribute:
      return LN_ExecOpKind::BoneCommand;
    case LN_OpCode::SetMaterialSlot:
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::AssignGeometryNodesModifier:
      return LN_ExecOpKind::MaterialCommand;
    case LN_OpCode::Print:
    case LN_OpCode::QuitGame:
    case LN_OpCode::RestartGame:
    case LN_OpCode::SetTimeScale:
      return LN_ExecOpKind::GlobalCommand;
    case LN_OpCode::LoadBlendFile:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
      return LN_ExecOpKind::FileCommand;
    case LN_OpCode::LoadScene:
    case LN_OpCode::SetScene:
      return LN_ExecOpKind::SceneCommand;
    case LN_OpCode::AddObject:
    case LN_OpCode::RemoveParent:
    case LN_OpCode::RemoveObject:
      return LN_ExecOpKind::LifecycleCommand;
    case LN_OpCode::SetParent:
    case LN_OpCode::ReplaceMesh:
    case LN_OpCode::CopyProperty:
      return LN_ExecOpKind::ObjectReferenceCommand;
    case LN_OpCode::SetGameProperty:
      return LN_ExecOpKind::GamePropertyCommand;
    case LN_OpCode::SetTreeProperty:
      return LN_ExecOpKind::TreePropertyCommand;
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
      return LN_ExecOpKind::TreeControlCommand;
    case LN_OpCode::SendEvent:
      return LN_ExecOpKind::SendEvent;
    case LN_OpCode::SetGlobalProperty:
      return LN_ExecOpKind::GlobalPropertyCommand;
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
      return LN_ExecOpKind::VariableCommand;
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
      return LN_ExecOpKind::NavigationCommand;
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
      return LN_ExecOpKind::SceneCollectionCommand;
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
      return LN_ExecOpKind::DebugDrawCommand;
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
      return LN_ExecOpKind::PhysicsConstraintCommand;
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return LN_ExecOpKind::SpawnPoolCommand;
    default:
      return LN_ExecOpKind::Nop;
  }
}

bool OpcodeHasDirectExecOp(const LN_Instruction &instruction)
{
  switch (instruction.opcode) {
    case LN_OpCode::Nop:
    case LN_OpCode::BranchRoute:
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyForceToTarget:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::Translate:
    case LN_OpCode::MoveToward:
    case LN_OpCode::SlowFollow:
    case LN_OpCode::AlignAxisToVector:
    case LN_OpCode::RotateToward:
    case LN_OpCode::SetCollisionGroup:
    case LN_OpCode::SetPhysics:
    case LN_OpCode::SetDynamics:
    case LN_OpCode::RebuildCollisionShape:
    case LN_OpCode::SetRigidBodyAttribute:
    case LN_OpCode::SetGravity:
    case LN_OpCode::CharacterJump:
    case LN_OpCode::SetCharacterGravity:
    case LN_OpCode::SetCharacterJumpSpeed:
    case LN_OpCode::SetCharacterMaxJumps:
    case LN_OpCode::SetCharacterWalkDirection:
    case LN_OpCode::SetCharacterVelocity:
    case LN_OpCode::VehicleControl:
    case LN_OpCode::VehicleApplyEngineForce:
    case LN_OpCode::VehicleApplyBraking:
    case LN_OpCode::VehicleApplySteering:
    case LN_OpCode::SetVehicleSuspensionCompression:
    case LN_OpCode::SetVehicleSuspensionStiffness:
    case LN_OpCode::SetVehicleSuspensionDamping:
    case LN_OpCode::SetVehicleWheelFriction:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyTransformVector:
    case LN_OpCode::ApplyPhysicsVector:
    case LN_OpCode::SetVisibility:
    case LN_OpCode::SetObjectColor:
    case LN_OpCode::SetActiveCamera:
    case LN_OpCode::SetCameraFov:
    case LN_OpCode::SetCameraOrthoScale:
    case LN_OpCode::MakeLightUnique:
    case LN_OpCode::SetLightColor:
    case LN_OpCode::SetLightPower:
    case LN_OpCode::SetLightShadow:
    case LN_OpCode::SetWindowSize:
    case LN_OpCode::SetFullscreen:
    case LN_OpCode::SetVSync:
    case LN_OpCode::SetShowFramerate:
    case LN_OpCode::SetShowProfile:
    case LN_OpCode::SetCursorVisibility:
    case LN_OpCode::SetCursorPosition:
    case LN_OpCode::SetGamepadVibration:
    case LN_OpCode::GamepadLook:
    case LN_OpCode::MouseLook:
    case LN_OpCode::StopAllSounds:
    case LN_OpCode::PlaySound:
    case LN_OpCode::PlaySound3D:
    case LN_OpCode::PauseSound:
    case LN_OpCode::ResumeSound:
    case LN_OpCode::StopSound:
    case LN_OpCode::PlayAction:
    case LN_OpCode::StopAction:
    case LN_OpCode::SetActionFrame:
    case LN_OpCode::SetBonePoseLocation:
    case LN_OpCode::SetBonePoseRotation:
    case LN_OpCode::SetBonePoseScale:
    case LN_OpCode::SetBonePoseTransform:
    case LN_OpCode::SetBoneAttribute:
    case LN_OpCode::SetBoneConstraintInfluence:
    case LN_OpCode::SetBoneConstraintTarget:
    case LN_OpCode::SetBoneConstraintAttribute:
    case LN_OpCode::SetMaterialSlot:
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::AssignGeometryNodesModifier:
    case LN_OpCode::Print:
    case LN_OpCode::QuitGame:
    case LN_OpCode::RestartGame:
    case LN_OpCode::SetTimeScale:
    case LN_OpCode::LoadBlendFile:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
    case LN_OpCode::LoadScene:
    case LN_OpCode::SetScene:
    case LN_OpCode::AddObject:
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
    case LN_OpCode::RemoveObject:
    case LN_OpCode::ReplaceMesh:
    case LN_OpCode::CopyProperty:
    case LN_OpCode::SendEvent:
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
    case LN_OpCode::SetGlobalProperty:
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return true;
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::SetTreeProperty:
      return true;
    default:
      return false;
  }
}

bool RegisterRefIsValid(const LN_RegisterExpressionRef &ref)
{
  return ref.expression_index != LN_INVALID_INDEX && ref.register_index != LN_INVALID_INDEX;
}

bool RegisterOpIsSoACandidate(const LN_RegisterExpressionOpKind kind)
{
  switch (kind) {
    case LN_RegisterExpressionOpKind::FloatAdd:
    case LN_RegisterExpressionOpKind::FloatSubtract:
    case LN_RegisterExpressionOpKind::FloatMultiply:
    case LN_RegisterExpressionOpKind::FloatDivide:
    case LN_RegisterExpressionOpKind::FloatMinimum:
    case LN_RegisterExpressionOpKind::FloatMaximum:
    case LN_RegisterExpressionOpKind::VectorAdd:
    case LN_RegisterExpressionOpKind::VectorSubtract:
    case LN_RegisterExpressionOpKind::VectorMultiply:
    case LN_RegisterExpressionOpKind::VectorDivide:
    case LN_RegisterExpressionOpKind::VectorScale:
      return true;
    default:
      return false;
  }
}

bool RegisterOpHasSimdKernel(const LN_RegisterExpressionOpKind kind)
{
  switch (kind) {
    case LN_RegisterExpressionOpKind::FloatAdd:
    case LN_RegisterExpressionOpKind::FloatSubtract:
    case LN_RegisterExpressionOpKind::FloatMultiply:
    case LN_RegisterExpressionOpKind::FloatDivide:
    case LN_RegisterExpressionOpKind::FloatMinimum:
    case LN_RegisterExpressionOpKind::FloatMaximum:
      return true;
    default:
      return false;
  }
}

}  // namespace

LN_Program::LN_Program()
    : m_programVersion(LN_PROGRAM_VERSION),
      m_schemaVersion(LN_PROGRAM_SCHEMA_VERSION),
      m_runtimeBuildFlags(CurrentRuntimeBuildFlags())
{
  m_execBlockPrograms[size_t(LN_Event::OnInit)].event = LN_Event::OnInit;
  m_execBlockPrograms[size_t(LN_Event::OnFixedUpdate)].event = LN_Event::OnFixedUpdate;
  m_execBlockPrograms[size_t(LN_Event::OnInit)].runtime_build_flags = m_runtimeBuildFlags;
  m_execBlockPrograms[size_t(LN_Event::OnFixedUpdate)].runtime_build_flags =
      m_runtimeBuildFlags;
  RebuildRegisterExpressionIR();
}

const std::string &LN_Program::GetString(const LN_StringId string_id) const
{
  static const std::string empty;
  if (string_id.index < m_stringTable.size()) {
    return m_stringTable[string_id.index];
  }
  return empty;
}

LN_StringId LN_Program::InternString(const std::string &value)
{
  for (uint32_t index = 0; index < m_stringTable.size(); index++) {
    if (m_stringTable[index] == value) {
      return LN_StringId{index};
    }
  }

  const uint32_t index = uint32_t(m_stringTable.size());
  m_stringTable.push_back(value);
  return LN_StringId{index};
}

uint32_t LN_Program::AddSpawnPoolState()
{
  return m_spawnPoolStateCount++;
}

uint32_t LN_Program::AddTimeFlowState()
{
  return m_timeFlowStateCount++;
}

uint32_t LN_Program::AddTimerState()
{
  return AddTimeFlowState();
}

bool LN_Program::IsCurrentRuntimeCompatible() const
{
  return m_programVersion == LN_PROGRAM_VERSION && m_schemaVersion == LN_PROGRAM_SCHEMA_VERSION &&
         m_runtimeBuildFlags == CurrentRuntimeBuildFlags();
}

bool LN_Program::MatchesSource(const std::string &source_tree_name,
                               const std::string &source_tree_library_path,
                               const std::string &source_checksum) const
{
  return IsCurrentRuntimeCompatible() && m_sourceTreeName == source_tree_name &&
         m_sourceTreeLibraryPath == source_tree_library_path &&
         m_sourceChecksum == source_checksum;
}

bool LN_Program::CanPreserveRuntimeStateWhenReplacing(const LN_Program &previous) const
{
  return IsCurrentRuntimeCompatible() && previous.IsCurrentRuntimeCompatible() &&
         m_programVersion == previous.m_programVersion &&
         m_schemaVersion == previous.m_schemaVersion &&
         m_runtimeBuildFlags == previous.m_runtimeBuildFlags &&
         m_sourceTreeName == previous.m_sourceTreeName &&
         m_sourceTreeLibraryPath == previous.m_sourceTreeLibraryPath &&
         m_sourceChecksum == previous.m_sourceChecksum;
}

std::shared_ptr<LN_Program> LN_Program::CreateDebugSetWorldPosition(const MT_Vector3 &position)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  program->m_name = "Debug Set World Position";
  program->m_sourceTreeName = "<debug-hardcoded>";
  program->m_sourceChecksum = "debug-hardcoded-v1";

  LN_SourceRef source_ref;
  source_ref.source_node_identifier = 0;
  source_ref.node_idname = "LN_DebugSetWorldPosition";
  source_ref.node_name = "Debug Set World Position";
  source_ref.socket_name = "Position";
  source_ref.source_tree_name = program->m_sourceTreeName;
  program->m_sourceRefs.push_back(source_ref);
  program->m_sourceNodeOrder.push_back(source_ref.source_node_identifier);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.source_ref_index = 0;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_value = position;
  LN_VectorExpression position_expression;
  position_expression.vector_value = position;
  instruction.vector_expr_index = program->AddVectorExpression(position_expression);
  program->AddInstruction(LN_Event::OnInit, instruction);

  program->m_compileReport.SetSourceTreeName(program->m_sourceTreeName);
  return program;
}

uint32_t LN_Program::AddVectorExpression(const LN_VectorExpression &expression)
{
  const LN_VectorExpression stored_expression = FoldVectorExpression(
      expression, m_floatExpressions, m_vectorExpressions);
  if (VectorExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_vectorExpressions, stored_expression, VectorExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_vectorExpressions.size());
  m_vectorExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForVectorExpression(stored_expression);
  AddVectorExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddQueryExpression(const LN_QueryExpression &expression)
{
  const uint32_t index = uint32_t(m_queryExpressions.size());
  LN_QueryExpression stored_expression = expression;
  if (stored_expression.runtime_state_index == LN_INVALID_INDEX) {
    if (stored_expression.cache_key >= 0) {
      for (const LN_QueryExpression &existing : m_queryExpressions) {
        if (existing.cache_key == stored_expression.cache_key) {
          stored_expression.ray_query_detail_flags |= existing.ray_query_detail_flags;
          if (existing.runtime_state_index != LN_INVALID_INDEX) {
            stored_expression.runtime_state_index = existing.runtime_state_index;
            break;
          }
        }
      }
    }
    if (stored_expression.runtime_state_index == LN_INVALID_INDEX) {
      stored_expression.runtime_state_index = m_queryRuntimeStateCount++;
    }
  }
  else if (stored_expression.runtime_state_index >= m_queryRuntimeStateCount) {
    m_queryRuntimeStateCount = stored_expression.runtime_state_index + 1;
  }

  m_queryExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForQueryExpression(stored_expression);
  AddQueryExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

void LN_Program::AddRayQueryDetailRequirement(const int32_t cacheKey,
                                              const uint8_t detailFlags)
{
  if (cacheKey < 0 || detailFlags == LN_RAY_QUERY_DETAIL_NONE) {
    return;
  }
  for (LN_QueryExpression &expression : m_queryExpressions) {
    if (expression.cache_key == cacheKey &&
        (expression.kind == LN_QueryExpressionKind::Raycast ||
         expression.kind == LN_QueryExpressionKind::RaycastAll ||
         expression.kind == LN_QueryExpressionKind::ShapeCast ||
         expression.kind == LN_QueryExpressionKind::ShapeCastAll))
    {
      expression.ray_query_detail_flags |= detailFlags;
    }
  }
  m_dependencySummary.ray_query_detail_requirements |= detailFlags;
}

uint32_t LN_Program::AddColorExpression(const LN_ColorExpression &expression)
{
  const LN_ColorExpression stored_expression = FoldColorExpression(expression, m_floatExpressions);
  if (ColorExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_colorExpressions, stored_expression, ColorExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_colorExpressions.size());
  m_colorExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForColorExpression(stored_expression);
  AddColorExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddValueExpression(const LN_ValueExpression &expression)
{
  if (ValueExpressionCanCSE(expression)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_valueExpressions, expression, ValueExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_valueExpressions.size());
  m_valueExpressions.push_back(expression);
  UpdateSchedulerSummaryForValueExpression(expression);
  AddValueExpressionDependencies(expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddTweenCurveTable(
    const std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT> &samples)
{
  const uint32_t index = uint32_t(m_tweenCurveTables.size());
  m_tweenCurveTables.push_back(samples);
  return index;
}

uint32_t LN_Program::AddGamePropertyRef(const LN_GamePropertyRef &property_ref)
{
  for (uint32_t index = 0; index < m_gamePropertyRefs.size(); index++) {
    const LN_GamePropertyRef &existing = m_gamePropertyRefs[index];
    if (existing.name == property_ref.name && existing.value_type == property_ref.value_type) {
      return index;
    }
  }

  const uint32_t index = uint32_t(m_gamePropertyRefs.size());
  LN_GamePropertyRef stored_ref = property_ref;
  stored_ref.name_id = InternString(stored_ref.name);
  m_gamePropertyRefs.push_back(stored_ref);
  return index;
}

uint32_t LN_Program::AddTreePropertyRef(const LN_TreePropertyRef &property_ref)
{
  for (uint32_t index = 0; index < m_treePropertyRefs.size(); index++) {
    const LN_TreePropertyRef &existing = m_treePropertyRefs[index];
    if (existing.name == property_ref.name && existing.value_type == property_ref.value_type) {
      return index;
    }
  }

  const uint32_t index = uint32_t(m_treePropertyRefs.size());
  LN_TreePropertyRef stored_ref = property_ref;
  stored_ref.name_id = InternString(stored_ref.name);
  m_treePropertyRefs.push_back(stored_ref);
  return index;
}

uint32_t LN_Program::AddGroupCallFrame(const LN_GroupCallFrame &call_frame)
{
  for (uint32_t index = 0; index < m_groupCallFrames.size(); index++) {
    const LN_GroupCallFrame &existing = m_groupCallFrames[index];
    if (existing.group_name == call_frame.group_name &&
        existing.source_tree_name == call_frame.source_tree_name &&
        existing.interface_key == call_frame.interface_key)
    {
      return index;
    }
  }

  const uint32_t index = uint32_t(m_groupCallFrames.size());
  m_groupCallFrames.push_back(call_frame);
  return index;
}

uint32_t LN_Program::AddLoopFrame(const LN_LoopFrame &loop_frame)
{
  const uint32_t index = uint32_t(m_loopFrames.size());
  m_loopFrames.push_back(loop_frame);
  return index;
}

void LN_Program::UpdateLoopFrame(const uint32_t loop_frame_index, const LN_LoopFrame &loop_frame)
{
  if (loop_frame_index < m_loopFrames.size()) {
    m_loopFrames[loop_frame_index] = loop_frame;
  }
}

uint32_t LN_Program::AddBoolExpression(const LN_BoolExpression &expression)
{
  const LN_BoolExpression stored_expression = FoldBoolExpression(
      expression, m_boolExpressions, m_floatExpressions);
  if (BoolExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_boolExpressions, stored_expression, BoolExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_boolExpressions.size());
  m_boolExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForBoolExpression(stored_expression);
  AddBoolExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddFloatExpression(const LN_FloatExpression &expression)
{
  const LN_FloatExpression stored_expression = FoldFloatExpression(
      expression, m_boolExpressions, m_floatExpressions, m_vectorExpressions, m_colorExpressions);
  if (FloatExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_floatExpressions, stored_expression, FloatExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_floatExpressions.size());
  m_floatExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForFloatExpression(stored_expression);
  AddFloatExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddIntExpression(const LN_IntExpression &expression)
{
  const LN_IntExpression stored_expression = FoldIntExpression(expression, m_stringExpressions);
  if (IntExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_intExpressions, stored_expression, IntExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_intExpressions.size());
  m_intExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForIntExpression(stored_expression);
  AddIntExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddStringExpression(const LN_StringExpression &expression)
{
  LN_StringExpression stored_expression = FoldStringExpression(expression, m_stringExpressions);
  if (stored_expression.kind == LN_StringExpressionKind::Constant) {
    stored_expression.string_id = InternString(stored_expression.string_value);
  }
  if (StringExpressionCanCSE(stored_expression.kind)) {
    const uint32_t existing_index = FindEquivalentExpression(
        m_stringExpressions, stored_expression, StringExpressionsEqual);
    if (existing_index != LN_INVALID_INDEX) {
      return existing_index;
    }
  }
  const uint32_t index = uint32_t(m_stringExpressions.size());
  m_stringExpressions.push_back(stored_expression);
  UpdateSchedulerSummaryForStringExpression(stored_expression);
  AddStringExpressionDependencies(stored_expression);
  RebuildRegisterExpressionIR();
  return index;
}

uint32_t LN_Program::AddSourceRef(const LN_SourceRef &source_ref)
{
  const uint32_t index = uint32_t(m_sourceRefs.size());
  m_sourceRefs.push_back(source_ref);
  return index;
}

void LN_Program::AddConstant(const LN_Constant &constant)
{
  m_constants.push_back(constant);
}

void LN_Program::AddCompileIssue(const LN_CompileSeverity severity,
                                 const std::string &message,
                                 const uint32_t source_ref_index)
{
  m_compileReport.AddIssue(severity, message, source_ref_index);
}

uint32_t LN_Program::AddVectorCommandPayload(const LN_VectorCommandPayload &payload)
{
  const uint32_t index = uint32_t(m_vectorCommandPayloads.size());
  m_vectorCommandPayloads.push_back(payload);
  return index;
}

uint32_t LN_Program::AddGamePropertyCommandPayload(
    const LN_GamePropertyCommandPayload &payload)
{
  const uint32_t index = uint32_t(m_gamePropertyCommandPayloads.size());
  m_gamePropertyCommandPayloads.push_back(payload);
  return index;
}

uint32_t LN_Program::AddRigidBodyConstraintCommandPayload(
    const LN_RigidBodyConstraintCommandPayload &payload)
{
  const uint32_t index = uint32_t(m_rigidBodyConstraintCommandPayloads.size());
  m_rigidBodyConstraintCommandPayloads.push_back(payload);
  return index;
}

uint32_t LN_Program::AddInstruction(const LN_Event event, const LN_Instruction &instruction)
{
  std::vector<LN_InstructionHeader> &headers = (event == LN_Event::OnInit) ? m_onInit :
                                                                           m_onFixedUpdate;
  std::vector<LN_Instruction> &payloads = (event == LN_Event::OnInit) ? m_onInitPayloads :
                                                                       m_onFixedUpdatePayloads;
  LN_Instruction payload = instruction;
  payload.payload_kind = PayloadKindForOpcode(payload.opcode);
  if (OpcodeUsesVectorCommandPayload(payload.opcode)) {
    payload.command_payload_index = AddVectorCommandPayload(
        VectorCommandPayloadFromInstruction(payload));
  }
  else if (payload.opcode == LN_OpCode::SetGameProperty) {
    payload.command_payload_index = AddGamePropertyCommandPayload(
        GamePropertyCommandPayloadFromInstruction(payload));
  }

  const uint32_t index = uint32_t(headers.size());
  const uint32_t payload_index = uint32_t(payloads.size());
  payloads.push_back(payload);

  LN_InstructionHeader header;
  header.opcode = payload.opcode;
  header.source_ref_index = payload.source_ref_index;
  header.payload_index = payload_index;
  header.flags = payload.flags;
  headers.push_back(header);

  m_debugMap.Add(event, index, header.source_ref_index);
  UpdateSchedulerSummaryForInstruction(event, payload);
  AddInstructionDependencies(event, payload);
  RebuildExecBlockIR(event);
  return index;
}

const std::vector<LN_InstructionHeader> &LN_Program::GetInstructionHeaders(
    const LN_Event event) const
{
  switch (event) {
    case LN_Event::OnInit:
      return m_onInit;
    case LN_Event::OnFixedUpdate:
      return m_onFixedUpdate;
  }

  return m_onFixedUpdate;
}

const LN_Instruction &LN_Program::GetInstructionPayload(const LN_Event event,
                                                        const uint32_t payload_index) const
{
  const std::vector<LN_Instruction> &payloads = GetInstructions(event);
  if (payload_index < payloads.size()) {
    return payloads[payload_index];
  }

  static const LN_Instruction empty_instruction;
  return empty_instruction;
}

const LN_Instruction &LN_Program::GetInstructionPayload(
    const LN_Event event, const LN_InstructionHeader &instruction) const
{
  return GetInstructionPayload(event, instruction.payload_index);
}

const std::vector<LN_Instruction> &LN_Program::GetInstructions(LN_Event event) const
{
  switch (event) {
    case LN_Event::OnInit:
      return m_onInitPayloads;
    case LN_Event::OnFixedUpdate:
      return m_onFixedUpdatePayloads;
  }

  return m_onFixedUpdatePayloads;
}

const LN_ExecBlockProgram &LN_Program::GetExecBlockProgram(const LN_Event event) const
{
  return m_execBlockPrograms[size_t(event)];
}

void LN_Program::RebuildExecBlockIR(const LN_Event event)
{
  LN_ExecBlockProgram &ir = m_execBlockPrograms[size_t(event)];
  const std::vector<LN_InstructionHeader> &headers = GetInstructionHeaders(event);

  ir = LN_ExecBlockProgram();
  ir.event = event;
  ir.program_version = m_programVersion;
  ir.schema_version = m_schemaVersion;
  ir.runtime_build_flags = m_runtimeBuildFlags;
  ir.cache_generation = LN_EXEC_BLOCK_IR_CACHE_GENERATION;
  ir.feature_mask = LN_EXEC_BLOCK_IR_FEATURE_MASK;
  ir.blocks.reserve(headers.size());
  ir.ops.reserve(headers.size());
  ir.source_refs.reserve(headers.size());

  uint32_t instruction_index = 0;
  while (instruction_index < headers.size()) {
    const LN_Instruction &instruction = GetInstructionPayload(event, headers[instruction_index]);
    const bool direct = OpcodeHasDirectExecOp(instruction);
    const uint32_t loop_frame = instruction.loop_frame_index;
    const uint32_t first_instruction = instruction_index;
    if (!direct) {
      ir.validation_errors.push_back("unsupported opcode in mandatory exec-block IR");
      ir.valid = false;
      return;
    }

    LN_ExecBlock block;
    block.first_instruction = first_instruction;
    block.first_op = uint32_t(ir.ops.size());
    block.loop_frame_index = loop_frame;
    block.source_ref_index = instruction.source_ref_index;

    while (instruction_index < headers.size()) {
      const LN_Instruction &block_instruction = GetInstructionPayload(event,
                                                                      headers[instruction_index]);
      if (!OpcodeHasDirectExecOp(block_instruction)) {
        break;
      }
      if (loop_frame != block_instruction.loop_frame_index) {
        break;
      }

      LN_ExecOp op;
      op.kind = ExecOpKindForOpcode(block_instruction.opcode);
      op.opcode = block_instruction.opcode;
      op.instruction_index = instruction_index;
      op.source_ref_index = block_instruction.source_ref_index;
      op.guard_bool_expr_index = block_instruction.bool_guard_expr_index;
      op.payload_index = headers[instruction_index].payload_index;
      if (const LN_RuntimeInstructionSemantics *semantics =
              LN_GetRuntimeInstructionSemantics(block_instruction.opcode))
      {
        op.fallback_requirements = semantics->fallback_requirements;
      }
      ir.ops.push_back(op);
      ir.source_refs.push_back(block_instruction.source_ref_index);
      instruction_index++;
    }
    block.instruction_count = instruction_index - first_instruction;
    block.op_count = uint32_t(ir.ops.size()) - block.first_op;
    ir.direct_instruction_count += block.instruction_count;

    ir.blocks.push_back(block);
  }

  ir.validation_errors.clear();
  ir.valid = ValidateExecBlockProgram(ir, headers, ir.validation_errors);
}

bool LN_Program::ValidateExecBlockProgram(const LN_ExecBlockProgram &ir,
                                          const std::vector<LN_InstructionHeader> &headers,
                                          std::vector<std::string> &r_errors) const
{
  const char *event_name = (ir.event == LN_Event::OnInit) ? "OnInit" : "OnFixedUpdate";
  auto add_error = [&](const std::string &message) {
    r_errors.push_back(std::string(event_name) + " exec-block IR: " + message);
  };

  if (ir.program_version != m_programVersion || ir.schema_version != m_schemaVersion ||
      ir.runtime_build_flags != m_runtimeBuildFlags ||
      ir.cache_generation != LN_EXEC_BLOCK_IR_CACHE_GENERATION ||
      ir.feature_mask != LN_EXEC_BLOCK_IR_FEATURE_MASK)
  {
    add_error("compatibility key does not match program/runtime");
  }

  uint32_t direct_instruction_count = 0;
  uint32_t fallback_instruction_count = 0;
  uint32_t fallback_block_count = 0;
  for (const LN_ExecBlock &block : ir.blocks) {
    if (block.first_instruction + block.instruction_count > headers.size()) {
      add_error("block instruction range is out of bounds");
    }
    if (block.first_op + block.op_count > ir.ops.size()) {
      add_error("block op range is out of bounds");
    }
    direct_instruction_count += block.instruction_count;
    if (block.op_count != block.instruction_count) {
      add_error("direct block op count does not match instruction count");
    }
    for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
         op_index++)
    {
      const LN_ExecOp &op = ir.ops[op_index];
      if (op.instruction_index >= headers.size()) {
        add_error("direct op instruction index is out of bounds");
        continue;
      }
      if (headers[op.instruction_index].payload_index != op.payload_index) {
        add_error("direct op payload index does not match instruction header");
      }
      const LN_Instruction &instruction = GetInstructionPayload(ir.event,
                                                                headers[op.instruction_index]);
      if (!OpcodeHasDirectExecOp(instruction)) {
        add_error("direct op references unsupported instruction");
      }
    }
  }

  if (direct_instruction_count != ir.direct_instruction_count) {
    add_error("direct instruction count drift");
  }
  if (fallback_instruction_count != ir.fallback_instruction_count) {
    add_error("fallback instruction count drift");
  }
  if (fallback_block_count != ir.fallback_block_count) {
    add_error("fallback block count drift");
  }

  return r_errors.empty();
}

bool LN_Program::ValidateExecBlockIR(std::vector<std::string> *r_errors) const
{
  std::vector<std::string> errors;
  const bool valid_on_init = ValidateExecBlockProgram(
      GetExecBlockProgram(LN_Event::OnInit), m_onInit, errors);
  const bool valid_on_fixed_update = ValidateExecBlockProgram(
      GetExecBlockProgram(LN_Event::OnFixedUpdate), m_onFixedUpdate, errors);
  if (r_errors != nullptr) {
    *r_errors = errors;
  }
  return valid_on_init && valid_on_fixed_update;
}

void LN_Program::RebuildRegisterExpressionIR()
{
  LN_RegisterExpressionProgram ir;
  ir.program_version = m_programVersion;
  ir.schema_version = m_schemaVersion;
  ir.runtime_build_flags = m_runtimeBuildFlags;
  ir.cache_generation = LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION;
  ir.feature_mask = LN_REGISTER_EXPRESSION_IR_FEATURE_MASK;
  ir.bool_expression_registers.assign(m_boolExpressions.size(), LN_INVALID_INDEX);
  ir.int_expression_registers.assign(m_intExpressions.size(), LN_INVALID_INDEX);
  ir.float_expression_registers.assign(m_floatExpressions.size(), LN_INVALID_INDEX);
  ir.vector_expression_registers.assign(m_vectorExpressions.size(), LN_INVALID_INDEX);
  ir.color_expression_registers.assign(m_colorExpressions.size(), LN_INVALID_INDEX);
  ir.string_expression_registers.assign(m_stringExpressions.size(), LN_INVALID_INDEX);
  ir.string_expression_register_kinds.assign(m_stringExpressions.size(), LN_RegisterValueKind::String);
  ir.value_expression_registers.assign(m_valueExpressions.size(), LN_INVALID_INDEX);
  ir.object_register_count = 0;
  ir.string_register_count = 0;
  ir.string_id_register_count = 0;
  ir.generic_value_register_count = 0;

  auto make_ref = [](const LN_RegisterValueKind kind,
                     const uint32_t expression_index,
                     const std::vector<uint32_t> &registers) {
    LN_RegisterExpressionRef ref;
    ref.value_kind = kind;
    ref.expression_index = expression_index;
    if (expression_index < registers.size()) {
      ref.register_index = registers[expression_index];
    }
    return ref;
  };

  auto make_string_ref = [&](const uint32_t expression_index) {
    LN_RegisterExpressionRef ref;
    ref.value_kind = LN_RegisterValueKind::String;
    ref.expression_index = expression_index;
    if (expression_index < ir.string_expression_registers.size()) {
      ref.register_index = ir.string_expression_registers[expression_index];
      if (expression_index < ir.string_expression_register_kinds.size()) {
        ref.value_kind = ir.string_expression_register_kinds[expression_index];
      }
    }
    return ref;
  };

  auto append_op = [&](LN_RegisterExpressionOp op) {
    switch (op.output_kind) {
      case LN_RegisterValueKind::Bool:
        op.output_register = ir.bool_register_count++;
        ir.bool_expression_registers[op.expression_index] = op.output_register;
        break;
      case LN_RegisterValueKind::Int:
        op.output_register = ir.int_register_count++;
        ir.int_expression_registers[op.expression_index] = op.output_register;
        break;
      case LN_RegisterValueKind::Float:
        op.output_register = ir.float_register_count++;
        ir.float_expression_registers[op.expression_index] = op.output_register;
        break;
      case LN_RegisterValueKind::Vector:
        op.output_register = ir.vector_register_count++;
        ir.vector_expression_registers[op.expression_index] = op.output_register;
        break;
      case LN_RegisterValueKind::Color:
        op.output_register = ir.color_register_count++;
        ir.color_expression_registers[op.expression_index] = op.output_register;
        break;
      case LN_RegisterValueKind::String:
        op.output_register = ir.string_register_count++;
        ir.string_expression_registers[op.expression_index] = op.output_register;
        ir.string_expression_register_kinds[op.expression_index] = LN_RegisterValueKind::String;
        break;
      case LN_RegisterValueKind::StringId:
        op.output_register = ir.string_id_register_count++;
        ir.string_expression_registers[op.expression_index] = op.output_register;
        ir.string_expression_register_kinds[op.expression_index] = LN_RegisterValueKind::StringId;
        break;
      case LN_RegisterValueKind::Object:
        break;
      case LN_RegisterValueKind::GenericValue:
        op.output_register = ir.generic_value_register_count++;
        ir.value_expression_registers[op.expression_index] = op.output_register;
        break;
    }
    ir.ops.push_back(op);
    ir.scalar_op_count++;
  };

  for (uint32_t index = 0; index < m_stringExpressions.size(); index++) {
    const LN_StringExpression &expression = m_stringExpressions[index];
    LN_RegisterExpressionOp op;
    op.expression_index = index;
    bool supported = true;
    switch (expression.kind) {
      case LN_StringExpressionKind::Constant:
        op.kind = LN_RegisterExpressionOpKind::StringIdConstant;
        op.output_kind = LN_RegisterValueKind::StringId;
        break;
      case LN_StringExpressionKind::SnapshotGameProperty:
        supported = expression.property_ref_index != LN_INVALID_INDEX;
        op.kind = LN_RegisterExpressionOpKind::StringSnapshotRead;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::RuntimeTreeProperty:
        supported = expression.property_ref_index != LN_INVALID_INDEX;
        op.kind = LN_RegisterExpressionOpKind::StringRuntimePropertyRead;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::Join:
        op.input0 = make_string_ref(expression.input0);
        op.input1 = make_string_ref(expression.input1);
        supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
        op.kind = LN_RegisterExpressionOpKind::StringJoin;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::Replace:
        op.input0 = make_string_ref(expression.input0);
        op.input1 = make_string_ref(expression.input1);
        op.input2 = make_string_ref(expression.input2);
        supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                    RegisterRefIsValid(op.input2);
        op.kind = LN_RegisterExpressionOpKind::StringReplace;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::ToUppercase:
      case LN_StringExpressionKind::ToLowercase:
        op.input0 = make_string_ref(expression.input0);
        supported = RegisterRefIsValid(op.input0);
        op.kind = expression.kind == LN_StringExpressionKind::ToUppercase ?
                      LN_RegisterExpressionOpKind::StringToUppercase :
                      LN_RegisterExpressionOpKind::StringToLowercase;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::ZeroFill:
        op.input0 = make_string_ref(expression.input0);
        op.input1 = make_ref(LN_RegisterValueKind::Int,
                             expression.int_expr_index,
                             ir.int_expression_registers);
        supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
        op.kind = LN_RegisterExpressionOpKind::StringZeroFill;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      case LN_StringExpressionKind::Format: {
        op.input0 = make_string_ref(expression.input0);
        op.input1 = make_string_ref(expression.input1);
        op.input2 = make_string_ref(expression.input2);
        op.input3 = make_string_ref(expression.input3);
        LN_RegisterExpressionRef input4 = make_string_ref(expression.input4);
        supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                    RegisterRefIsValid(op.input2) && RegisterRefIsValid(op.input3) &&
                    RegisterRefIsValid(input4);
        if (supported) {
          op.variable_ref_start = uint32_t(ir.variable_refs.size());
          op.variable_ref_count = 1;
          ir.variable_refs.push_back(input4);
        }
        op.kind = LN_RegisterExpressionOpKind::StringFormat;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      }
      case LN_StringExpressionKind::FromGenericValue:
        op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                             expression.value_expr_index,
                             ir.value_expression_registers);
        supported = RegisterRefIsValid(op.input0);
        op.kind = LN_RegisterExpressionOpKind::StringFromValue;
        op.output_kind = LN_RegisterValueKind::String;
        break;
      default:
        continue;
    }
    if (!supported) {
      continue;
    }
    append_op(op);
  }

  auto value_constant_is_register_safe = [](const LN_Value &value) {
    if (!value.exists) {
      return true;
    }
    switch (value.type) {
      case LN_ValueType::None:
      case LN_ValueType::Bool:
      case LN_ValueType::Int:
      case LN_ValueType::Float:
      case LN_ValueType::Vector:
      case LN_ValueType::Vector4:
      case LN_ValueType::Color:
      case LN_ValueType::Rotation:
      case LN_ValueType::Matrix:
        return true;
      default:
        return false;
    }
  };

  bool changed = true;
  while (changed) {
    changed = false;

    for (uint32_t index = 0; index < m_intExpressions.size(); index++) {
      if (ir.int_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_IntExpression &expression = m_intExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::Int;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_IntExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::IntConstant;
          break;
        case LN_IntExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::IntRuntimePropertyRead;
          break;
        case LN_IntExpressionKind::SnapshotGameProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::IntSnapshotRead;
          break;
        case LN_IntExpressionKind::SnapshotCollisionGroup:
        case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
        case LN_IntExpressionKind::SnapshotCharacterJumpCount:
          supported = expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::IntSnapshotRead;
          break;
        case LN_IntExpressionKind::MouseWheelDelta:
        case LN_IntExpressionKind::WindowResolutionWidth:
        case LN_IntExpressionKind::WindowResolutionHeight:
        case LN_IntExpressionKind::WindowVSyncMode:
          op.kind = LN_RegisterExpressionOpKind::IntInputRead;
          break;
        case LN_IntExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::IntFromValue;
          break;
        case LN_IntExpressionKind::DictLength:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::IntDictLength;
          break;
        case LN_IntExpressionKind::ListLength:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::IntListLength;
          break;
        case LN_IntExpressionKind::StringCount:
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_string_ref(expression.input1);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::IntStringCount;
          break;
        case LN_IntExpressionKind::Random:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.input1,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::IntRandom;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_stringExpressions.size(); index++) {
      if (ir.string_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_StringExpression &expression = m_stringExpressions[index];
      LN_RegisterExpressionOp op;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_StringExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::StringIdConstant;
          op.output_kind = LN_RegisterValueKind::StringId;
          break;
        case LN_StringExpressionKind::SnapshotGameProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::StringSnapshotRead;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::StringRuntimePropertyRead;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::Join:
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_string_ref(expression.input1);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::StringJoin;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::Replace:
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_string_ref(expression.input1);
          op.input2 = make_string_ref(expression.input2);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input2);
          op.kind = LN_RegisterExpressionOpKind::StringReplace;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::ToUppercase:
        case LN_StringExpressionKind::ToLowercase:
          op.input0 = make_string_ref(expression.input0);
          supported = RegisterRefIsValid(op.input0);
          op.kind = expression.kind == LN_StringExpressionKind::ToUppercase ?
                        LN_RegisterExpressionOpKind::StringToUppercase :
                        LN_RegisterExpressionOpKind::StringToLowercase;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::ZeroFill:
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.int_expr_index,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::StringZeroFill;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        case LN_StringExpressionKind::Format: {
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_string_ref(expression.input1);
          op.input2 = make_string_ref(expression.input2);
          op.input3 = make_string_ref(expression.input3);
          LN_RegisterExpressionRef input4 = make_string_ref(expression.input4);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input2) && RegisterRefIsValid(op.input3) &&
                      RegisterRefIsValid(input4);
          if (supported) {
            op.variable_ref_start = uint32_t(ir.variable_refs.size());
            op.variable_ref_count = 1;
            ir.variable_refs.push_back(input4);
          }
          op.kind = LN_RegisterExpressionOpKind::StringFormat;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        }
        case LN_StringExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.value_expr_index,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::StringFromValue;
          op.output_kind = LN_RegisterValueKind::String;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_floatExpressions.size(); index++) {
      if (ir.float_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_FloatExpression &expression = m_floatExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::Float;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_FloatExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::FloatConstant;
          break;
        case LN_FloatExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::FloatRuntimePropertyRead;
          break;
        case LN_FloatExpressionKind::SnapshotGameProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::FloatSnapshotRead;
          break;
        case LN_FloatExpressionKind::SnapshotTimeScale:
        case LN_FloatExpressionKind::SnapshotLightPower:
        case LN_FloatExpressionKind::SnapshotElapsedTime:
        case LN_FloatExpressionKind::SnapshotFrameDelta:
        case LN_FloatExpressionKind::SnapshotFPS:
        case LN_FloatExpressionKind::SnapshotDeltaFactor:
          supported = expression.kind != LN_FloatExpressionKind::SnapshotLightPower ||
                      expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::FloatSnapshotRead;
          break;
        case LN_FloatExpressionKind::GamepadButtonStrength:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::FloatInputRead;
          break;
        case LN_FloatExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::FloatFromValue;
          break;
        case LN_FloatExpressionKind::Add:
        case LN_FloatExpressionKind::Subtract:
        case LN_FloatExpressionKind::Multiply:
        case LN_FloatExpressionKind::Divide:
        case LN_FloatExpressionKind::Power:
        case LN_FloatExpressionKind::Minimum:
        case LN_FloatExpressionKind::Maximum:
        case LN_FloatExpressionKind::Modulo:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = expression.kind == LN_FloatExpressionKind::Add ?
                        LN_RegisterExpressionOpKind::FloatAdd :
                    expression.kind == LN_FloatExpressionKind::Subtract ?
                        LN_RegisterExpressionOpKind::FloatSubtract :
                    expression.kind == LN_FloatExpressionKind::Multiply ?
                        LN_RegisterExpressionOpKind::FloatMultiply :
                    expression.kind == LN_FloatExpressionKind::Divide ?
                        LN_RegisterExpressionOpKind::FloatDivide :
                    expression.kind == LN_FloatExpressionKind::Power ?
                        LN_RegisterExpressionOpKind::FloatPower :
                    expression.kind == LN_FloatExpressionKind::Minimum ?
                        LN_RegisterExpressionOpKind::FloatMinimum :
                    expression.kind == LN_FloatExpressionKind::Maximum ?
                        LN_RegisterExpressionOpKind::FloatMaximum :
                    expression.kind == LN_FloatExpressionKind::Modulo ?
                        LN_RegisterExpressionOpKind::FloatModulo :
                        LN_RegisterExpressionOpKind::FloatMaximum;
          break;
        case LN_FloatExpressionKind::Absolute:
        case LN_FloatExpressionKind::Sign:
        case LN_FloatExpressionKind::Round:
        case LN_FloatExpressionKind::Floor:
        case LN_FloatExpressionKind::Ceil:
        case LN_FloatExpressionKind::Truncate:
        case LN_FloatExpressionKind::Fraction:
        case LN_FloatExpressionKind::Sine:
        case LN_FloatExpressionKind::Cosine:
        case LN_FloatExpressionKind::Radians:
        case LN_FloatExpressionKind::Degrees:
        case LN_FloatExpressionKind::Negate:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = expression.kind == LN_FloatExpressionKind::Absolute ?
                        LN_RegisterExpressionOpKind::FloatAbsolute :
                    expression.kind == LN_FloatExpressionKind::Sign ?
                        LN_RegisterExpressionOpKind::FloatSign :
                    expression.kind == LN_FloatExpressionKind::Round ?
                        LN_RegisterExpressionOpKind::FloatRound :
                    expression.kind == LN_FloatExpressionKind::Floor ?
                        LN_RegisterExpressionOpKind::FloatFloor :
                    expression.kind == LN_FloatExpressionKind::Ceil ?
                        LN_RegisterExpressionOpKind::FloatCeil :
                    expression.kind == LN_FloatExpressionKind::Truncate ?
                        LN_RegisterExpressionOpKind::FloatTruncate :
                    expression.kind == LN_FloatExpressionKind::Fraction ?
                        LN_RegisterExpressionOpKind::FloatFraction :
                    expression.kind == LN_FloatExpressionKind::Sine ?
                        LN_RegisterExpressionOpKind::FloatSine :
                    expression.kind == LN_FloatExpressionKind::Cosine ?
                        LN_RegisterExpressionOpKind::FloatCosine :
                    expression.kind == LN_FloatExpressionKind::Radians ?
                        LN_RegisterExpressionOpKind::FloatRadians :
                    expression.kind == LN_FloatExpressionKind::Degrees ?
                        LN_RegisterExpressionOpKind::FloatDegrees :
                        LN_RegisterExpressionOpKind::FloatNegate;
          break;
        case LN_FloatExpressionKind::Clamp:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Float,
                               expression.input2,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input2);
          op.kind = LN_RegisterExpressionOpKind::FloatClamp;
          break;
        case LN_FloatExpressionKind::Threshold:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Bool,
                               expression.bool_expr_index,
                               ir.bool_expression_registers);
          op.threshold_operation = expression.threshold_operation;
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      (expression.bool_expr_index == LN_INVALID_INDEX ||
                       RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::FloatThreshold;
          break;
        case LN_FloatExpressionKind::RangedThreshold:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Float,
                               expression.input2,
                               ir.float_expression_registers);
          op.range_operation = expression.range_operation;
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input2);
          op.kind = LN_RegisterExpressionOpKind::FloatRangedThreshold;
          break;
        case LN_FloatExpressionKind::Select:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Bool,
                               expression.bool_expr_index,
                               ir.bool_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      (expression.bool_expr_index == LN_INVALID_INDEX ||
                       RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::FloatSelect;
          break;
        case LN_FloatExpressionKind::Formula:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      StringExpressionIndexIsConstant(m_stringExpressions,
                                                      expression.string_expr_index);
          op.kind = LN_RegisterExpressionOpKind::FloatFormula;
          break;
        case LN_FloatExpressionKind::VectorComponent:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          op.component_index = expression.component_index;
          supported = RegisterRefIsValid(op.input0) && expression.component_index < 3;
          op.kind = LN_RegisterExpressionOpKind::FloatVectorComponent;
          break;
        case LN_FloatExpressionKind::ColorComponent:
          op.input0 = make_ref(LN_RegisterValueKind::Color,
                               expression.input0,
                               ir.color_expression_registers);
          op.component_index = expression.component_index;
          supported = RegisterRefIsValid(op.input0) && expression.component_index < 4;
          op.kind = LN_RegisterExpressionOpKind::FloatColorComponent;
          break;
        case LN_FloatExpressionKind::Random:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::FloatRandom;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_vectorExpressions.size(); index++) {
      if (ir.vector_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_VectorExpression &expression = m_vectorExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::Vector;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_VectorExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::VectorConstant;
          break;
        case LN_VectorExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::VectorRuntimePropertyRead;
          break;
        case LN_VectorExpressionKind::SnapshotWorldPosition:
        case LN_VectorExpressionKind::SnapshotLocalPosition:
        case LN_VectorExpressionKind::SnapshotWorldOrientation:
        case LN_VectorExpressionKind::SnapshotLocalOrientation:
        case LN_VectorExpressionKind::SnapshotWorldScale:
        case LN_VectorExpressionKind::SnapshotLocalScale:
        case LN_VectorExpressionKind::SnapshotLinearVelocity:
        case LN_VectorExpressionKind::SnapshotLocalLinearVelocity:
        case LN_VectorExpressionKind::SnapshotAngularVelocity:
        case LN_VectorExpressionKind::SnapshotLocalAngularVelocity:
        case LN_VectorExpressionKind::SnapshotCharacterGravity:
        case LN_VectorExpressionKind::SnapshotCharacterWalkDirection:
        case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection:
          supported = expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::VectorSnapshotRead;
          break;
        case LN_VectorExpressionKind::AxisVector:
          supported = expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::VectorSnapshotRead;
          break;
        case LN_VectorExpressionKind::SnapshotGravity:
          op.kind = LN_RegisterExpressionOpKind::VectorSnapshotRead;
          break;
        case LN_VectorExpressionKind::CursorPosition:
        case LN_VectorExpressionKind::CursorMovement:
        case LN_VectorExpressionKind::WindowResolution:
          op.kind = LN_RegisterExpressionOpKind::VectorInputRead;
          break;
        case LN_VectorExpressionKind::GamepadStick:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input3 = make_ref(LN_RegisterValueKind::Float,
                               expression.float_expr_index,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input3);
          op.kind = LN_RegisterExpressionOpKind::VectorInputRead;
          break;
        case LN_VectorExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::VectorFromValue;
          break;
        case LN_VectorExpressionKind::Add:
        case LN_VectorExpressionKind::Subtract:
        case LN_VectorExpressionKind::Multiply:
        case LN_VectorExpressionKind::Divide:
        case LN_VectorExpressionKind::Minimum:
        case LN_VectorExpressionKind::Maximum:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input1,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = expression.kind == LN_VectorExpressionKind::Add ?
                        LN_RegisterExpressionOpKind::VectorAdd :
                    expression.kind == LN_VectorExpressionKind::Subtract ?
                        LN_RegisterExpressionOpKind::VectorSubtract :
                    expression.kind == LN_VectorExpressionKind::Multiply ?
                        LN_RegisterExpressionOpKind::VectorMultiply :
                    expression.kind == LN_VectorExpressionKind::Divide ?
                        LN_RegisterExpressionOpKind::VectorDivide :
                    expression.kind == LN_VectorExpressionKind::Minimum ?
                        LN_RegisterExpressionOpKind::VectorMinimum :
                        LN_RegisterExpressionOpKind::VectorMaximum;
          break;
        case LN_VectorExpressionKind::Absolute:
        case LN_VectorExpressionKind::Normalize:
        case LN_VectorExpressionKind::Resize:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = expression.kind == LN_VectorExpressionKind::Absolute ?
                        LN_RegisterExpressionOpKind::VectorAbsolute :
                    expression.kind == LN_VectorExpressionKind::Normalize ?
                        LN_RegisterExpressionOpKind::VectorNormalize :
                        LN_RegisterExpressionOpKind::VectorResize;
          break;
        case LN_VectorExpressionKind::RotateAroundAxis: {
          const int32_t rotate_mode = int32_t(expression.property_ref_index) / 4;
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input1,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          if (rotate_mode == 3) {
            op.input2 = make_ref(LN_RegisterValueKind::Vector,
                                 expression.input2,
                                 ir.vector_expression_registers);
            supported = supported && RegisterRefIsValid(op.input2);
          }
          else {
            op.input3 = make_ref(LN_RegisterValueKind::Float,
                                 expression.float_expr_index,
                                 ir.float_expression_registers);
            supported = supported && RegisterRefIsValid(op.input3);
            if (rotate_mode == 2) {
              op.input2 = make_ref(LN_RegisterValueKind::Vector,
                                   expression.input2,
                                   ir.vector_expression_registers);
              supported = supported && RegisterRefIsValid(op.input2);
            }
          }
          op.kind = LN_RegisterExpressionOpKind::VectorRotateAroundAxis;
          break;
        }
        case LN_VectorExpressionKind::VectorToRotation:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input1,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::VectorToRotation;
          break;
        case LN_VectorExpressionKind::Scale:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.float_expr_index,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::VectorScale;
          break;
        case LN_VectorExpressionKind::Combine:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Float,
                               expression.input2,
                               ir.float_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1)) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::VectorCombine;
          break;
        case LN_VectorExpressionKind::Random:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::VectorRandom;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_colorExpressions.size(); index++) {
      if (ir.color_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_ColorExpression &expression = m_colorExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::Color;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_ColorExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::ColorConstant;
          break;
        case LN_ColorExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::ColorRuntimePropertyRead;
          break;
        case LN_ColorExpressionKind::SnapshotObjectColor:
        case LN_ColorExpressionKind::SnapshotLightColor:
          supported = expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::ColorSnapshotRead;
          break;
        case LN_ColorExpressionKind::Combine:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Float,
                               expression.input2,
                               ir.float_expression_registers);
          op.input3 = make_ref(LN_RegisterValueKind::Float,
                               expression.input3,
                               ir.float_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1)) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2)) &&
                      (expression.input3 == LN_INVALID_INDEX || RegisterRefIsValid(op.input3));
          op.kind = LN_RegisterExpressionOpKind::ColorCombine;
          break;
        case LN_ColorExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ColorFromValue;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_boolExpressions.size(); index++) {
      if (ir.bool_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_BoolExpression &expression = m_boolExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::Bool;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_BoolExpressionKind::Constant:
          op.kind = LN_RegisterExpressionOpKind::BoolConstant;
          break;
        case LN_BoolExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::BoolRuntimePropertyRead;
          break;
        case LN_BoolExpressionKind::SnapshotGameProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::BoolSnapshotRead;
          break;
        case LN_BoolExpressionKind::SnapshotGamePropertyExists:
          op.kind = LN_RegisterExpressionOpKind::BoolSnapshotRead;
          break;
        case LN_BoolExpressionKind::SnapshotVisibility:
        case LN_BoolExpressionKind::SnapshotCharacterOnGround:
          supported = expression.input0 == LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::BoolSnapshotRead;
          break;
        case LN_BoolExpressionKind::InputStatus:
        case LN_BoolExpressionKind::KeyboardActive:
        case LN_BoolExpressionKind::WindowFullscreen:
        case LN_BoolExpressionKind::MouseMoved:
        case LN_BoolExpressionKind::MouseWheelMoved:
        case LN_BoolExpressionKind::KeyLoggerPressed:
          supported = expression.kind != LN_BoolExpressionKind::MouseMoved || !expression.bool_value;
          op.kind = LN_RegisterExpressionOpKind::BoolInputRead;
          break;
        case LN_BoolExpressionKind::GamepadActive:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::BoolInputRead;
          break;
        case LN_BoolExpressionKind::GamepadButton:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.int_expr_index,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0) &&
                      (expression.int_expr_index == LN_INVALID_INDEX ||
                       RegisterRefIsValid(op.input1));
          op.kind = LN_RegisterExpressionOpKind::BoolInputRead;
          break;
        case LN_BoolExpressionKind::Not:
          op.input0 = make_ref(LN_RegisterValueKind::Bool,
                               expression.input0,
                               ir.bool_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::BoolNot;
          break;
        case LN_BoolExpressionKind::And:
        case LN_BoolExpressionKind::Or:
          op.input0 = make_ref(LN_RegisterValueKind::Bool,
                               expression.input0,
                               ir.bool_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Bool,
                               expression.input1,
                               ir.bool_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = expression.kind == LN_BoolExpressionKind::And ?
                        LN_RegisterExpressionOpKind::BoolAnd :
                        LN_RegisterExpressionOpKind::BoolOr;
          break;
        case LN_BoolExpressionKind::FloatCompare:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Float,
                               expression.input1,
                               ir.float_expression_registers);
          op.float_compare_operation = expression.float_compare_operation;
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::BoolFloatCompare;
          break;
        case LN_BoolExpressionKind::StringContains:
        case LN_BoolExpressionKind::StringStartsWith:
        case LN_BoolExpressionKind::StringEndsWith:
          op.input0 = make_string_ref(expression.input0);
          op.input1 = make_string_ref(expression.input1);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::BoolStringPredicate;
          break;
        case LN_BoolExpressionKind::ValueIsNone:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::BoolValueIsNone;
          break;
        case LN_BoolExpressionKind::FromGenericValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::BoolFromValue;
          break;
        case LN_BoolExpressionKind::ValueCompare:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          op.float_compare_operation = expression.float_compare_operation;
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::BoolValueCompare;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }

    for (uint32_t index = 0; index < m_valueExpressions.size(); index++) {
      if (ir.value_expression_registers[index] != LN_INVALID_INDEX) {
        continue;
      }
      const LN_ValueExpression &expression = m_valueExpressions[index];
      LN_RegisterExpressionOp op;
      op.output_kind = LN_RegisterValueKind::GenericValue;
      op.expression_index = index;
      bool supported = true;
      switch (expression.kind) {
        case LN_ValueExpressionKind::Constant:
          supported = value_constant_is_register_safe(expression.value);
          op.kind = LN_RegisterExpressionOpKind::ValueConstant;
          break;
        case LN_ValueExpressionKind::SnapshotGameProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::ValueSnapshotRead;
          break;
        case LN_ValueExpressionKind::RuntimeTreeProperty:
          supported = expression.property_ref_index != LN_INVALID_INDEX;
          op.kind = LN_RegisterExpressionOpKind::ValueRuntimePropertyRead;
          break;
        case LN_ValueExpressionKind::EmptyDict:
          op.kind = LN_RegisterExpressionOpKind::ValueEmptyDict;
          break;
        case LN_ValueExpressionKind::EmptyList:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          supported = expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueEmptyList;
          break;
        case LN_ValueExpressionKind::Select:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::Bool,
                               expression.bool_expr_index,
                               ir.bool_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                      RegisterRefIsValid(op.input2);
          op.kind = LN_RegisterExpressionOpKind::ValueSelect;
          break;
        case LN_ValueExpressionKind::FromBool:
          op.input0 = make_ref(LN_RegisterValueKind::Bool,
                               expression.input0,
                               ir.bool_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromBool;
          break;
        case LN_ValueExpressionKind::FromInt:
          op.input0 = make_ref(LN_RegisterValueKind::Int,
                               expression.input0,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromInt;
          break;
        case LN_ValueExpressionKind::FromFloat:
          op.input0 = make_ref(LN_RegisterValueKind::Float,
                               expression.input0,
                               ir.float_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromFloat;
          break;
        case LN_ValueExpressionKind::FromString:
          op.input0 = make_string_ref(expression.input0);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromString;
          break;
        case LN_ValueExpressionKind::FromVector:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromVector;
          break;
        case LN_ValueExpressionKind::FromColor:
          op.input0 = make_ref(LN_RegisterValueKind::Color,
                               expression.input0,
                               ir.color_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromColor;
          break;
        case LN_ValueExpressionKind::FromRotation:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueFromRotation;
          break;
        case LN_ValueExpressionKind::CombineVector4:
          supported = expression.input_indices.size() >= 4;
          if (supported) {
            op.input0 = make_ref(LN_RegisterValueKind::Float,
                                 expression.input_indices[0],
                                 ir.float_expression_registers);
            op.input1 = make_ref(LN_RegisterValueKind::Float,
                                 expression.input_indices[1],
                                 ir.float_expression_registers);
            op.input2 = make_ref(LN_RegisterValueKind::Float,
                                 expression.input_indices[2],
                                 ir.float_expression_registers);
            op.input3 = make_ref(LN_RegisterValueKind::Float,
                                 expression.input_indices[3],
                                 ir.float_expression_registers);
            supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1) &&
                        RegisterRefIsValid(op.input2) && RegisterRefIsValid(op.input3);
          }
          op.kind = LN_RegisterExpressionOpKind::ValueCombineVector4;
          break;
        case LN_ValueExpressionKind::ResizeVectorValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueResizeVector;
          break;
        case LN_ValueExpressionKind::EulerToMatrix:
          op.input0 = make_ref(LN_RegisterValueKind::Vector,
                               expression.input0,
                               ir.vector_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueEulerToMatrix;
          break;
        case LN_ValueExpressionKind::MatrixToEuler:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueMatrixToEuler;
          break;
        case LN_ValueExpressionKind::MakeList:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input2,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1)) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::ValueMakeList;
          break;
        case LN_ValueExpressionKind::ListFromItems: {
          std::vector<LN_RegisterExpressionRef> refs;
          refs.reserve(expression.input_indices.size());
          for (const uint32_t input_index : expression.input_indices) {
            if (input_index == LN_INVALID_INDEX) {
              continue;
            }
            LN_RegisterExpressionRef ref = make_ref(LN_RegisterValueKind::GenericValue,
                                                    input_index,
                                                    ir.value_expression_registers);
            if (!RegisterRefIsValid(ref)) {
              supported = false;
              break;
            }
            refs.push_back(ref);
          }
          if (supported) {
            op.variable_ref_start = uint32_t(ir.variable_refs.size());
            op.variable_ref_count = uint32_t(refs.size());
            ir.variable_refs.insert(ir.variable_refs.end(), refs.begin(), refs.end());
          }
          op.kind = LN_RegisterExpressionOpKind::ValueListFromItems;
          break;
        }
        case LN_ValueExpressionKind::ListDuplicate:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueListDuplicate;
          break;
        case LN_ValueExpressionKind::ListExtend:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          op.kind = LN_RegisterExpressionOpKind::ValueListExtend;
          break;
        case LN_ValueExpressionKind::ListAppend:
        case LN_ValueExpressionKind::ListRemoveValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          op.kind = expression.kind == LN_ValueExpressionKind::ListAppend ?
                        LN_RegisterExpressionOpKind::ValueListAppend :
                        LN_RegisterExpressionOpKind::ValueListRemoveValue;
          break;
        case LN_ValueExpressionKind::ListRemoveIndex:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.input1,
                               ir.int_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          op.kind = LN_RegisterExpressionOpKind::ValueListRemoveIndex;
          break;
        case LN_ValueExpressionKind::ListSetIndex:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.input1,
                               ir.int_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input2,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1)) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::ValueListSetIndex;
          break;
        case LN_ValueExpressionKind::ListElement:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::Int,
                               expression.input1,
                               ir.int_expression_registers);
          supported = RegisterRefIsValid(op.input0) && RegisterRefIsValid(op.input1);
          op.kind = LN_RegisterExpressionOpKind::ValueListElement;
          break;
        case LN_ValueExpressionKind::ListRandomItem:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueListRandomItem;
          break;
        case LN_ValueExpressionKind::ValueSwitchList: {
          std::vector<LN_RegisterExpressionRef> refs;
          refs.reserve(expression.input_indices.size());
          const size_t pair_count = expression.input_indices.size() / 2;
          for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
            const uint32_t condition_index = expression.input_indices[pair_index * 2];
            const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
            LN_RegisterExpressionRef condition_ref = make_ref(LN_RegisterValueKind::Bool,
                                                              condition_index,
                                                              ir.bool_expression_registers);
            LN_RegisterExpressionRef value_ref = make_ref(LN_RegisterValueKind::GenericValue,
                                                          value_index,
                                                          ir.value_expression_registers);
            if ((condition_index != LN_INVALID_INDEX && !RegisterRefIsValid(condition_ref)) ||
                (value_index != LN_INVALID_INDEX && !RegisterRefIsValid(value_ref)))
            {
              supported = false;
              break;
            }
            refs.push_back(condition_ref);
            refs.push_back(value_ref);
          }
          if (supported) {
            op.variable_ref_start = uint32_t(ir.variable_refs.size());
            op.variable_ref_count = uint32_t(refs.size());
            ir.variable_refs.insert(ir.variable_refs.end(), refs.begin(), refs.end());
          }
          op.kind = LN_RegisterExpressionOpKind::ValueSwitchList;
          break;
        }
        case LN_ValueExpressionKind::ValueSwitchListCompare: {
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          std::vector<LN_RegisterExpressionRef> refs;
          refs.reserve(expression.input_indices.size());
          const size_t pair_count = expression.input_indices.size() / 2;
          for (size_t pair_index = 0; supported && pair_index < pair_count; pair_index++) {
            const uint32_t case_index = expression.input_indices[pair_index * 2];
            const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
            LN_RegisterExpressionRef case_ref = make_ref(LN_RegisterValueKind::GenericValue,
                                                         case_index,
                                                         ir.value_expression_registers);
            LN_RegisterExpressionRef value_ref = make_ref(LN_RegisterValueKind::GenericValue,
                                                          value_index,
                                                          ir.value_expression_registers);
            if ((case_index != LN_INVALID_INDEX && !RegisterRefIsValid(case_ref)) ||
                (value_index != LN_INVALID_INDEX && !RegisterRefIsValid(value_ref)))
            {
              supported = false;
              break;
            }
            refs.push_back(case_ref);
            refs.push_back(value_ref);
          }
          if (supported) {
            op.variable_ref_start = uint32_t(ir.variable_refs.size());
            op.variable_ref_count = uint32_t(refs.size());
            ir.variable_refs.insert(ir.variable_refs.end(), refs.begin(), refs.end());
          }
          op.kind = LN_RegisterExpressionOpKind::ValueSwitchListCompare;
          break;
        }
        case LN_ValueExpressionKind::MakeDict:
          op.input0 = make_ref(LN_RegisterValueKind::StringId,
                               expression.input0,
                               ir.string_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX ||
                       (RegisterRefIsValid(op.input0) &&
                        StringExpressionIndexIsConstant(m_stringExpressions, expression.input0))) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          op.kind = LN_RegisterExpressionOpKind::ValueMakeDict;
          break;
        case LN_ValueExpressionKind::DictGetKey:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::StringId,
                               expression.input1,
                               ir.string_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input2,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX ||
                       (RegisterRefIsValid(op.input1) &&
                        StringExpressionIndexIsConstant(m_stringExpressions, expression.input1))) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::ValueDictGetKey;
          break;
        case LN_ValueExpressionKind::DictSetKey:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::StringId,
                               expression.input1,
                               ir.string_expression_registers);
          op.input2 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input2,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX ||
                       (RegisterRefIsValid(op.input1) &&
                        StringExpressionIndexIsConstant(m_stringExpressions, expression.input1))) &&
                      (expression.input2 == LN_INVALID_INDEX || RegisterRefIsValid(op.input2));
          op.kind = LN_RegisterExpressionOpKind::ValueDictSetKey;
          break;
        case LN_ValueExpressionKind::DictRemoveKey:
        case LN_ValueExpressionKind::DictRemoveKeyValue:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::StringId,
                               expression.input1,
                               ir.string_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX ||
                       (RegisterRefIsValid(op.input1) &&
                        StringExpressionIndexIsConstant(m_stringExpressions, expression.input1)));
          op.kind = expression.kind == LN_ValueExpressionKind::DictRemoveKey ?
                        LN_RegisterExpressionOpKind::ValueDictRemoveKey :
                        LN_RegisterExpressionOpKind::ValueDictRemoveKeyValue;
          break;
        case LN_ValueExpressionKind::DictMerge:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          op.input1 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input1,
                               ir.value_expression_registers);
          supported = (expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0)) &&
                      (expression.input1 == LN_INVALID_INDEX || RegisterRefIsValid(op.input1));
          op.kind = LN_RegisterExpressionOpKind::ValueDictMerge;
          break;
        case LN_ValueExpressionKind::DictGetKeys:
          op.input0 = make_ref(LN_RegisterValueKind::GenericValue,
                               expression.input0,
                               ir.value_expression_registers);
          supported = expression.input0 == LN_INVALID_INDEX || RegisterRefIsValid(op.input0);
          op.kind = LN_RegisterExpressionOpKind::ValueDictGetKeys;
          break;
        default:
          supported = false;
          break;
      }
      if (!supported) {
        continue;
      }
      append_op(op);
      changed = true;
    }
  }

  ir.fallback_expression_count =
      uint32_t(m_boolExpressions.size() + m_intExpressions.size() + m_floatExpressions.size() +
               m_vectorExpressions.size() + m_colorExpressions.size() +
               m_stringExpressions.size() + m_valueExpressions.size() -
               ir.bool_register_count - ir.int_register_count - ir.float_register_count -
               ir.vector_register_count - ir.color_register_count - ir.string_register_count -
               ir.string_id_register_count - ir.generic_value_register_count);

  for (uint32_t op_index = 0; op_index < ir.ops.size(); op_index++) {
    const LN_RegisterExpressionOp &op = ir.ops[op_index];
    LN_RegisterExpressionLifetime lifetime;
    lifetime.value_kind = op.output_kind;
    lifetime.expression_index = op.expression_index;
    lifetime.register_index = op.output_register;
    lifetime.first_op_index = op_index;
    lifetime.last_use_op_index = op_index;
    ir.lifetimes.push_back(lifetime);
  }

  auto touch_input = [&](const LN_RegisterExpressionRef &ref, const uint32_t op_index) {
    if (!RegisterRefIsValid(ref)) {
      return;
    }
    for (LN_RegisterExpressionLifetime &lifetime : ir.lifetimes) {
      if (lifetime.value_kind == ref.value_kind && lifetime.register_index == ref.register_index) {
        lifetime.last_use_op_index = std::max(lifetime.last_use_op_index, op_index);
        return;
      }
    }
  };
  for (uint32_t op_index = 0; op_index < ir.ops.size(); op_index++) {
    touch_input(ir.ops[op_index].input0, op_index);
    touch_input(ir.ops[op_index].input1, op_index);
    touch_input(ir.ops[op_index].input2, op_index);
    touch_input(ir.ops[op_index].input3, op_index);
    const uint32_t variable_ref_end = ir.ops[op_index].variable_ref_start +
                                      ir.ops[op_index].variable_ref_count;
    for (uint32_t ref_index = ir.ops[op_index].variable_ref_start;
         ref_index < variable_ref_end && ref_index < ir.variable_refs.size();
         ref_index++)
    {
      touch_input(ir.variable_refs[ref_index], op_index);
    }
  }

  uint32_t batch_start = 0;
  while (batch_start < ir.ops.size()) {
    const LN_RegisterExpressionOpKind kind = ir.ops[batch_start].kind;
    uint32_t batch_end = batch_start + 1;
    while (batch_end < ir.ops.size() && ir.ops[batch_end].kind == kind) {
      batch_end++;
    }
    const uint32_t op_count = batch_end - batch_start;
    if (RegisterOpIsSoACandidate(kind) && op_count >= 2) {
      LN_RegisterExpressionSoABatch batch;
      batch.kind = kind;
      batch.first_op_index = batch_start;
      batch.op_count = op_count;
      batch.scalar_fallback_available = true;
      batch.simd_candidate = RegisterOpHasSimdKernel(kind) && op_count >= 4;
      ir.soa_batches.push_back(batch);
      if (batch.simd_candidate) {
        ir.simd_candidate_batch_count++;
        ir.simd_candidate_lane_count += op_count;
      }
    }
    batch_start = batch_end;
  }

  std::vector<std::string> errors;
  ir.valid = ValidateRegisterExpressionProgram(ir, errors);
  m_registerExpressionProgram = std::move(ir);
}

bool LN_Program::ValidateRegisterExpressionProgram(const LN_RegisterExpressionProgram &ir,
                                                   std::vector<std::string> &r_errors) const
{
  auto add_error = [&](const std::string &message) {
    r_errors.push_back(std::string("register expression IR: ") + message);
  };

  if (ir.program_version != m_programVersion || ir.schema_version != m_schemaVersion ||
      ir.runtime_build_flags != m_runtimeBuildFlags ||
      ir.cache_generation != LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION ||
      ir.feature_mask != LN_REGISTER_EXPRESSION_IR_FEATURE_MASK)
  {
    add_error("compatibility key does not match program/runtime");
  }
  if (!ir.scalar_fallback_available) {
    add_error("scalar fallback is required for every register/SIMD path");
  }
  if (ir.bool_expression_registers.size() != m_boolExpressions.size() ||
      ir.int_expression_registers.size() != m_intExpressions.size() ||
      ir.float_expression_registers.size() != m_floatExpressions.size() ||
      ir.vector_expression_registers.size() != m_vectorExpressions.size() ||
      ir.color_expression_registers.size() != m_colorExpressions.size() ||
      ir.string_expression_registers.size() != m_stringExpressions.size() ||
      ir.string_expression_register_kinds.size() != m_stringExpressions.size() ||
      ir.value_expression_registers.size() != m_valueExpressions.size())
  {
    add_error("expression-to-register maps do not match expression tables");
  }

  auto validate_ref = [&](const LN_RegisterExpressionRef &ref) {
    if (!RegisterRefIsValid(ref)) {
      return;
    }
    switch (ref.value_kind) {
      case LN_RegisterValueKind::Bool:
        if (ref.register_index >= ir.bool_register_count ||
            ref.expression_index >= m_boolExpressions.size())
        {
          add_error("bool input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::Int:
        if (ref.register_index >= ir.int_register_count ||
            ref.expression_index >= m_intExpressions.size())
        {
          add_error("int input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::Float:
        if (ref.register_index >= ir.float_register_count ||
            ref.expression_index >= m_floatExpressions.size())
        {
          add_error("float input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::Vector:
        if (ref.register_index >= ir.vector_register_count ||
            ref.expression_index >= m_vectorExpressions.size())
        {
          add_error("vector input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::Color:
        if (ref.register_index >= ir.color_register_count ||
            ref.expression_index >= m_colorExpressions.size())
        {
          add_error("color input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::StringId:
        if (ref.register_index >= ir.string_id_register_count ||
            ref.expression_index >= m_stringExpressions.size())
        {
          add_error("string-id input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::String:
        if (ref.register_index >= ir.string_register_count ||
            ref.expression_index >= m_stringExpressions.size())
        {
          add_error("string input ref is out of bounds");
        }
        break;
      case LN_RegisterValueKind::Object:
        break;
      case LN_RegisterValueKind::GenericValue:
        if (ref.register_index >= ir.generic_value_register_count ||
            ref.expression_index >= m_valueExpressions.size())
        {
          add_error("generic-value input ref is out of bounds");
        }
        break;
    }
  };

  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.output_register == LN_INVALID_INDEX) {
      add_error("op output register is invalid");
    }
    validate_ref(op.input0);
    validate_ref(op.input1);
    validate_ref(op.input2);
    validate_ref(op.input3);
    if (op.variable_ref_start + op.variable_ref_count > ir.variable_refs.size()) {
      add_error("op variable input ref range is out of bounds");
    }
    else {
      const uint32_t variable_ref_end = op.variable_ref_start + op.variable_ref_count;
      for (uint32_t ref_index = op.variable_ref_start; ref_index < variable_ref_end; ref_index++) {
        validate_ref(ir.variable_refs[ref_index]);
      }
    }
  }

  for (const LN_RegisterExpressionLifetime &lifetime : ir.lifetimes) {
    if (lifetime.first_op_index >= ir.ops.size() || lifetime.last_use_op_index >= ir.ops.size() ||
        lifetime.first_op_index > lifetime.last_use_op_index)
    {
      add_error("register lifetime range is invalid");
    }
  }

  for (const LN_RegisterExpressionSoABatch &batch : ir.soa_batches) {
    if (batch.first_op_index + batch.op_count > ir.ops.size()) {
      add_error("SoA batch range is out of bounds");
    }
    if (!batch.scalar_fallback_available) {
      add_error("SoA/SIMD batch is missing scalar fallback");
    }
  }

  return r_errors.empty();
}

bool LN_Program::ValidateRegisterExpressionIR(std::vector<std::string> *r_errors) const
{
  std::vector<std::string> errors;
  const bool valid = ValidateRegisterExpressionProgram(m_registerExpressionProgram, errors);
  if (r_errors != nullptr) {
    *r_errors = errors;
  }
  return valid;
}

void LN_Program::ValidateInstructionPayloadCollection(
    const LN_Event event,
    const std::vector<LN_InstructionHeader> &headers,
    const std::vector<LN_Instruction> &payloads,
    std::vector<std::string> &r_errors) const
{
  const char *event_name = (event == LN_Event::OnInit) ? "OnInit" : "OnFixedUpdate";
  auto add_error = [&](const size_t instruction_index, const std::string &message) {
    r_errors.push_back(std::string(event_name) + " instruction " +
                       std::to_string(instruction_index) + ": " + message);
  };
  auto check_index = [&](const uint32_t index,
                         const size_t size,
                         const size_t instruction_index,
                         const char *name) {
    if (index != LN_INVALID_INDEX && index >= size) {
      add_error(instruction_index,
                std::string(name) + " index " + std::to_string(index) +
                    " is outside table size " + std::to_string(size));
    }
  };

  if (headers.size() != payloads.size()) {
    r_errors.push_back(std::string(event_name) + " header/payload count mismatch");
  }

  for (size_t index = 0; index < headers.size(); index++) {
    const LN_InstructionHeader &header = headers[index];
    if (header.payload_index >= payloads.size()) {
      add_error(index, "payload index is outside payload table");
      continue;
    }

    const LN_Instruction &payload = payloads[header.payload_index];
    const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(
        payload.opcode);
    if (semantics == nullptr) {
      add_error(index, "opcode is missing runtime semantics entry");
    }
    else {
      const LN_RuntimeCommandFamily command_family = LN_GetRuntimeCommandFamily(payload.opcode);
      const LN_RuntimeSideEffectDelivery delivery = LN_GetRuntimeSideEffectDelivery(
          payload.opcode);
      if (command_family == LN_RuntimeCommandFamily::Unknown) {
        add_error(index, "opcode is missing runtime command family entry");
      }
      if (delivery == LN_RuntimeSideEffectDelivery::Unknown) {
        add_error(index, "opcode is missing runtime side-effect delivery entry");
      }
      if (delivery == LN_RuntimeSideEffectDelivery::None &&
          semantics->writes != LN_RUNTIME_SEMANTIC_WRITE_NONE)
      {
        add_error(index, "side-effect-free opcode declares runtime writes");
      }
      if (delivery == LN_RuntimeSideEffectDelivery::RuntimeTreeState &&
          (semantics->writes & LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE) == 0u)
      {
        add_error(index, "runtime-state opcode does not declare runtime-state writes");
      }
      if ((delivery == LN_RuntimeSideEffectDelivery::DeferredCommandBuffer ||
           delivery == LN_RuntimeSideEffectDelivery::ImmediateAndDeferred) &&
          (semantics->writes & LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM) == 0u)
      {
        add_error(index, "deferred command opcode does not declare command-stream writes");
      }
      if (delivery == LN_RuntimeSideEffectDelivery::ImmediateMainThread &&
          (semantics->writes & LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM) != 0u)
      {
        add_error(index, "immediate main-thread opcode incorrectly declares command-stream writes");
      }
      if ((semantics->fallback_requirements & LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP) != 0u &&
          (semantics->reads & LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE) == 0u)
      {
        add_error(index, "dynamic lookup fallback does not declare program-table reads");
      }
    }
    if (payload.opcode != header.opcode) {
      add_error(index, "payload opcode does not match instruction header");
    }
    if (payload.source_ref_index != header.source_ref_index) {
      add_error(index, "payload source ref does not match instruction header");
    }
    if (payload.flags != header.flags) {
      add_error(index, "payload flags do not match instruction header");
    }
    if (payload.payload_kind != PayloadKindForOpcode(payload.opcode)) {
      add_error(index, "payload kind does not match opcode family");
    }
    if ((payload.opcode == LN_OpCode::ArmTimer || payload.opcode == LN_OpCode::ArmDelay ||
         payload.opcode == LN_OpCode::UpdatePulsify ||
         payload.opcode == LN_OpCode::UpdateBarrier ||
         payload.opcode == LN_OpCode::TryCooldown ||
         payload.opcode == LN_OpCode::ResetCooldown) &&
        (payload.int_value < 0 || uint32_t(payload.int_value) >= m_timeFlowStateCount))
    {
      add_error(index, "time-flow state index is outside dense state table");
    }
    if (payload.opcode == LN_OpCode::TryOnce || payload.opcode == LN_OpCode::ResetOnce) {
      if (payload.int_value < 0 || uint32_t(payload.int_value) >= m_boolExpressions.size()) {
        add_error(index, "Once bool expression index is outside expression table");
      }
      else if (m_boolExpressions[uint32_t(payload.int_value)].kind !=
               LN_BoolExpressionKind::Once)
      {
        add_error(index, "Once state opcode does not reference a Once expression");
      }
    }
    if (!m_sourceRefs.empty() && header.source_ref_index >= m_sourceRefs.size()) {
      add_error(index, "source ref index is outside source ref table");
    }

    const bool input_motion_look_opcode = payload.opcode == LN_OpCode::GamepadLook ||
                                          payload.opcode == LN_OpCode::MouseLook;
    const bool send_event_opcode = payload.opcode == LN_OpCode::SendEvent;

    check_index(payload.bool_guard_expr_index, m_boolExpressions.size(), index, "bool guard");
    check_index(payload.bool_expr_index, m_boolExpressions.size(), index, "bool");
    check_index(payload.secondary_bool_expr_index,
                m_boolExpressions.size(),
                index,
                "secondary bool");
    check_index(payload.tertiary_bool_expr_index,
                m_boolExpressions.size(),
                index,
                "tertiary bool");
    check_index(payload.quaternary_bool_expr_index,
                m_boolExpressions.size(),
                index,
                "quaternary bool");
    if (input_motion_look_opcode) {
      check_index(payload.int_expr_index, m_vectorExpressions.size(), index, "look cap y vector");
    }
    else if (send_event_opcode) {
      check_index(payload.int_expr_index, m_valueExpressions.size(), index, "event target value");
    }
    else {
      check_index(payload.int_expr_index, m_intExpressions.size(), index, "int");
    }
    check_index(payload.secondary_int_expr_index,
                m_intExpressions.size(),
                index,
                "secondary int");
    check_index(payload.tertiary_int_expr_index,
                m_intExpressions.size(),
                index,
                "tertiary int");
    check_index(payload.float_expr_index, m_floatExpressions.size(), index, "float");
    check_index(payload.secondary_float_expr_index,
                m_floatExpressions.size(),
                index,
                "secondary float");
    check_index(payload.tertiary_float_expr_index,
                m_floatExpressions.size(),
                index,
                "tertiary float");
    check_index(payload.quaternary_float_expr_index,
                m_floatExpressions.size(),
                index,
                "quaternary float");
    if (input_motion_look_opcode) {
      check_index(payload.string_expr_index, m_floatExpressions.size(), index, "look smoothing float");
    }
    else {
      check_index(payload.string_expr_index, m_stringExpressions.size(), index, "string");
    }
    check_index(payload.secondary_string_expr_index,
                m_stringExpressions.size(),
                index,
                "secondary string");
    check_index(payload.tertiary_string_expr_index,
                m_stringExpressions.size(),
                index,
                "tertiary string");
    check_index(payload.vector_expr_index, m_vectorExpressions.size(), index, "vector");
    check_index(payload.secondary_vector_expr_index,
                m_vectorExpressions.size(),
                index,
                "secondary vector");
    if (input_motion_look_opcode) {
      check_index(payload.color_expr_index, m_vectorExpressions.size(), index, "look cap x vector");
    }
    else {
      check_index(payload.color_expr_index, m_colorExpressions.size(), index, "color");
    }
    check_index(payload.value_expr_index, m_valueExpressions.size(), index, "value");
    check_index(payload.secondary_value_expr_index,
                m_valueExpressions.size(),
                index,
                "secondary value");
    check_index(payload.tertiary_value_expr_index,
                m_valueExpressions.size(),
                index,
                "tertiary value");
    check_index(payload.quaternary_value_expr_index,
                m_valueExpressions.size(),
                index,
                "quaternary value");
    check_index(payload.loop_frame_index, m_loopFrames.size(), index, "loop frame");

    if (OpcodeUsesVectorCommandPayload(payload.opcode)) {
      if (payload.command_payload_index >= m_vectorCommandPayloads.size()) {
        add_error(index, "vector command payload index is outside payload table");
      }
      else {
        const LN_VectorCommandPayload &vector_payload =
            m_vectorCommandPayloads[payload.command_payload_index];
        check_index(vector_payload.object_value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "vector command object value");
        check_index(vector_payload.vector_expr_index,
                    m_vectorExpressions.size(),
                    index,
                    "vector command vector");
        check_index(vector_payload.secondary_vector_expr_index,
                    m_vectorExpressions.size(),
                    index,
                    "vector command secondary vector");
        check_index(vector_payload.bool_expr_index,
                    m_boolExpressions.size(),
                    index,
                    "vector command bool");
        if (vector_payload.operation_mask == 0 ||
            (vector_payload.operation_mask & ~LN_VECTOR_OPERATION_MASK_ALL) != 0)
        {
          add_error(index, "vector command mask contains unsupported channels");
        }
      }
    }
    else if (payload.opcode == LN_OpCode::SetGameProperty) {
      if (payload.command_payload_index >= m_gamePropertyCommandPayloads.size()) {
        add_error(index, "game property command payload index is outside payload table");
      }
      else {
        const LN_GamePropertyCommandPayload &property_payload =
            m_gamePropertyCommandPayloads[payload.command_payload_index];
        if (property_payload.property_ref_index == LN_INVALID_INDEX) {
          add_error(index, "game property command payload is missing a property ref");
        }
        check_index(property_payload.object_value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "game property command object value");
        check_index(property_payload.property_ref_index,
                    m_gamePropertyRefs.size(),
                    index,
                    "game property command ref");
        check_index(property_payload.value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "game property command value");
        check_index(property_payload.bool_expr_index,
                    m_boolExpressions.size(),
                    index,
                    "game property command bool");
        check_index(property_payload.int_expr_index,
                    m_intExpressions.size(),
                    index,
                    "game property command int");
        check_index(property_payload.float_expr_index,
                    m_floatExpressions.size(),
                    index,
                    "game property command float");
        check_index(property_payload.string_expr_index,
                    m_stringExpressions.size(),
                    index,
                    "game property command string");
        check_index(property_payload.vector_expr_index,
                    m_vectorExpressions.size(),
                    index,
                    "game property command vector");
        check_index(property_payload.color_expr_index,
                    m_colorExpressions.size(),
                    index,
                    "game property command color");
        if (property_payload.value_expr_index == LN_INVALID_INDEX &&
            property_payload.value_type == LN_ValueType::None)
        {
          add_error(index, "set game property command payload is missing a value type");
        }
      }
    }
    else if (payload.opcode == LN_OpCode::AddPhysicsConstraint) {
      if (payload.command_payload_index >= m_rigidBodyConstraintCommandPayloads.size()) {
        add_error(index, "rigid body constraint command payload index is outside payload table");
      }
      else {
        const LN_RigidBodyConstraintCommandPayload &constraint_payload =
            m_rigidBodyConstraintCommandPayloads[payload.command_payload_index];
        check_index(constraint_payload.constraint_object_value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "rigid body constraint object frame value");
        check_index(constraint_payload.object_value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "rigid body constraint object value");
        check_index(constraint_payload.target_value_expr_index,
                    m_valueExpressions.size(),
                    index,
                    "rigid body constraint target value");
        check_index(constraint_payload.name_string_expr_index,
                    m_stringExpressions.size(),
                    index,
                    "rigid body constraint name string");
        check_index(constraint_payload.velocity_solver_iterations_int_expr_index,
                    m_intExpressions.size(),
                    index,
                    "rigid body constraint velocity solver iterations int");
        check_index(constraint_payload.position_solver_iterations_int_expr_index,
                    m_intExpressions.size(),
                    index,
                    "rigid body constraint position solver iterations int");
        for (const uint32_t bool_expr_index : constraint_payload.bool_expr_indices) {
          check_index(
              bool_expr_index, m_boolExpressions.size(), index, "rigid body constraint bool");
        }
        for (const uint32_t vector_expr_index : constraint_payload.vector_expr_indices) {
          check_index(vector_expr_index,
                      m_vectorExpressions.size(),
                      index,
                      "rigid body constraint vector");
        }
        for (const uint32_t float_expr_index : constraint_payload.float_expr_indices) {
          check_index(float_expr_index,
                      m_floatExpressions.size(),
                      index,
                      "rigid body constraint float");
        }
        const bool valid_constraint_type =
            constraint_payload.type == PHY_RigidBodyConstraintType::Point ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Hinge ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Slider ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Generic ||
            constraint_payload.type == PHY_RigidBodyConstraintType::GenericSpring ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Fixed ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Piston ||
            constraint_payload.type == PHY_RigidBodyConstraintType::Motor;
        if (!valid_constraint_type) {
          add_error(index, "rigid body constraint type is unsupported");
        }
        if (uint8_t(constraint_payload.spring_type) >
            uint8_t(PHY_RigidBodyConstraintSpringType::Spring2))
        {
          add_error(index, "rigid body constraint spring type is unsupported");
        }
      }
    }

    switch (payload.opcode) {
      case LN_OpCode::SetTransformVector:
        if (payload.vector_operation_mode > uint8_t(LN_VectorOperationMode::LocalFromBool)) {
          add_error(index, "transform vector operation has an invalid mode");
        }
        if (payload.vector_operation_channel > uint8_t(LN_VectorOperationChannel::Scale)) {
          add_error(index, "transform vector operation has an invalid channel");
        }
        break;
      case LN_OpCode::SetVelocityVector:
        if (payload.vector_operation_mode > uint8_t(LN_VectorOperationMode::LocalFromBool)) {
          add_error(index, "velocity vector operation has an invalid mode");
        }
        if (payload.vector_operation_channel <
            uint8_t(LN_VectorOperationChannel::LinearVelocity) ||
          payload.vector_operation_channel >
            uint8_t(LN_VectorOperationChannel::AngularVelocity))
        {
          add_error(index, "velocity vector operation has an invalid channel");
        }
        break;
      case LN_OpCode::ApplyTransformVector:
        if (payload.vector_operation_mode > uint8_t(LN_VectorOperationMode::LocalFromBool)) {
          add_error(index, "apply transform vector operation has an invalid mode");
        }
        if (payload.vector_operation_channel < uint8_t(LN_VectorOperationChannel::Movement) ||
          payload.vector_operation_channel > uint8_t(LN_VectorOperationChannel::Rotation))
        {
          add_error(index, "apply transform vector operation has an invalid channel");
        }
        break;
      case LN_OpCode::ApplyPhysicsVector:
        if (payload.vector_operation_mode > uint8_t(LN_VectorOperationMode::LocalFromBool)) {
          add_error(index, "apply physics vector operation has an invalid mode");
        }
        if (payload.vector_operation_channel < uint8_t(LN_VectorOperationChannel::Force) ||
            payload.vector_operation_channel > uint8_t(LN_VectorOperationChannel::Torque))
        {
          add_error(index, "apply physics vector operation has an invalid channel");
        }
        break;
      case LN_OpCode::SetBonePoseLocation:
        if (payload.int_value < int(LN_BonePoseLocationSpace::ArmatureOffset) ||
            payload.int_value > int(LN_BonePoseLocationSpace::PoseChannel))
        {
          add_error(index, "set bone pose location has an invalid space");
        }
        if (payload.secondary_int_value < 0 || payload.secondary_int_value > 1) {
          add_error(index, "set bone pose location has an invalid center flag");
        }
        break;
      case LN_OpCode::SetBonePoseRotation:
        if (payload.int_value < int(LN_BonePoseRotationSpace::PoseChannel) ||
            payload.int_value > int(LN_BonePoseRotationSpace::World))
        {
          add_error(index, "set bone pose rotation has an invalid space");
        }
        if (payload.secondary_int_value < 0 || payload.secondary_int_value > 1) {
          add_error(index, "set bone pose rotation has an invalid center flag");
        }
        break;
      case LN_OpCode::SetBonePoseTransform:
        if (payload.int_value < int(LN_BonePoseLocationSpace::ArmatureOffset) ||
            payload.int_value > int(LN_BonePoseLocationSpace::PoseChannel))
        {
          add_error(index, "set bone pose transform has an invalid space");
        }
        if (payload.secondary_int_value < 0 || payload.secondary_int_value > 1) {
          add_error(index, "set bone pose transform has an invalid center flag");
        }
        break;
      case LN_OpCode::SetGameProperty:
        if (payload.property_ref_index == LN_INVALID_INDEX &&
            payload.command_payload_index == LN_INVALID_INDEX)
        {
          add_error(index, "set game property payload is missing a property ref");
        }
        check_index(payload.property_ref_index,
                    m_gamePropertyRefs.size(),
                    index,
                    "game property ref");
        if (payload.value_expr_index == LN_INVALID_INDEX &&
            payload.property_value_type == LN_ValueType::None)
        {
          add_error(index, "set game property payload is missing a value type");
        }
        break;
      case LN_OpCode::SetTreeProperty:
        if (payload.property_ref_index == LN_INVALID_INDEX) {
          add_error(index, "set tree property payload is missing a property ref");
        }
        check_index(payload.property_ref_index,
                    m_treePropertyRefs.size(),
                    index,
                    "tree property ref");
        break;
      case LN_OpCode::AddObject:
        check_index(payload.property_ref_index,
                    m_treePropertyRefs.size(),
                    index,
                    "tree property ref");
        break;
      case LN_OpCode::GamepadLook:
      case LN_OpCode::MouseLook:
        check_index(payload.property_ref_index,
                    m_valueExpressions.size(),
                    index,
                    "value property ref");
        break;
      case LN_OpCode::SpawnPoolCreate:
      case LN_OpCode::SpawnPoolSpawn:
        if (payload.int_value < 0 || uint32_t(payload.int_value) >= m_spawnPoolStateCount) {
          add_error(index, "spawn pool state id is outside dense state table");
        }
        break;
      default:
        break;
    }
    if (payload.vector_operation_mask == 0 ||
        (payload.vector_operation_mask & ~LN_VECTOR_OPERATION_MASK_ALL) != 0)
    {
      add_error(index, "vector operation mask contains unsupported channels");
    }
  }
}

bool LN_Program::ValidateInstructionPayloads(std::vector<std::string> *r_errors) const
{
  std::vector<std::string> errors;
  ValidateInstructionPayloadCollection(
      LN_Event::OnInit, m_onInit, m_onInitPayloads, errors);
  ValidateInstructionPayloadCollection(
      LN_Event::OnFixedUpdate, m_onFixedUpdate, m_onFixedUpdatePayloads, errors);

  if (!m_onFixedUpdate.empty()) {
    if (m_schedulerSummary.required_phase != LN_SchedulerRequiredPhase::OnFixedUpdate) {
      errors.push_back("scheduler phase restriction does not cover OnFixedUpdate instructions");
    }
  }
  else if (!m_onInit.empty()) {
    if (m_schedulerSummary.required_phase != LN_SchedulerRequiredPhase::OnInit) {
      errors.push_back("scheduler phase restriction does not cover OnInit instructions");
    }
  }

  auto validate_scheduler_restrictions = [&](const char *event_name,
                                             const std::vector<LN_Instruction> &payloads) {
    for (size_t index = 0; index < payloads.size(); index++) {
      const LN_Instruction &payload = payloads[index];
      const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(
          payload.opcode);
      if (semantics != nullptr) {
        const LN_RuntimeCommandFamily command_family = LN_GetRuntimeCommandFamily(payload.opcode);
        const uint32_t minimum_resources = MinimumSchedulerResourcesForInstructionSemantics(
            *semantics, command_family);
        if ((minimum_resources & ~m_schedulerSummary.resource_access) != 0u) {
          errors.push_back(std::string(event_name) + " instruction " + std::to_string(index) +
                           ": scheduler summary is missing semantic resource access");
        }
      }
      const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForOpcode(payload.opcode);
      if (reason != LN_MainThreadOnlyReason::None &&
          (m_schedulerSummary.worker_lane_eligible ||
           m_schedulerSummary.main_thread_only_reason == LN_MainThreadOnlyReason::None))
      {
        errors.push_back(std::string(event_name) + " instruction " + std::to_string(index) +
                         ": scheduler restriction allows a main-thread-only opcode");
      }
      if (OpcodeEmitsCommands(payload.opcode) && !m_schedulerSummary.emits_commands) {
        errors.push_back(std::string(event_name) + " instruction " + std::to_string(index) +
                         ": scheduler restriction lost command-emitting status");
      }
    }
  };
  validate_scheduler_restrictions("OnInit", m_onInitPayloads);
  validate_scheduler_restrictions("OnFixedUpdate", m_onFixedUpdatePayloads);

  LN_ProgramDependencySummary expected_dependencies;
  auto add_expected_instruction_dependencies = [&](const std::vector<LN_Instruction> &payloads) {
    for (const LN_Instruction &payload : payloads) {
      const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(
          payload.opcode);
      if (semantics != nullptr) {
        AddSemanticReadDependencies(expected_dependencies, semantics->reads);
      }
      AddInstructionDependencyDetails(expected_dependencies, payload, m_stringExpressions);
    }
  };
  add_expected_instruction_dependencies(m_onInitPayloads);
  add_expected_instruction_dependencies(m_onFixedUpdatePayloads);
  for (const LN_BoolExpression &expression : m_boolExpressions) {
    AddBoolExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_FloatExpression &expression : m_floatExpressions) {
    AddFloatExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_IntExpression &expression : m_intExpressions) {
    AddIntExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_StringExpression &expression : m_stringExpressions) {
    AddStringExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_VectorExpression &expression : m_vectorExpressions) {
    AddVectorExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_QueryExpression &expression : m_queryExpressions) {
    AddQueryExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_ColorExpression &expression : m_colorExpressions) {
    AddColorExpressionDependencyDetails(expected_dependencies, expression);
  }
  for (const LN_ValueExpression &expression : m_valueExpressions) {
    AddValueExpressionDependencyDetails(expected_dependencies, expression);
  }

  auto validate_dependency_mask = [&](const char *name,
                                      const uint32_t actual,
                                      const uint32_t expected) {
    if ((expected & ~actual) != 0u) {
      errors.push_back(std::string("dependency summary is missing ") + name + " bits");
    }
  };
  validate_dependency_mask(
      "snapshot channel", m_dependencySummary.snapshot_channels, expected_dependencies.snapshot_channels);
  validate_dependency_mask(
      "input channel", m_dependencySummary.input_channels, expected_dependencies.input_channels);
  validate_dependency_mask(
      "event channel", m_dependencySummary.event_channels, expected_dependencies.event_channels);
  validate_dependency_mask(
      "query channel", m_dependencySummary.query_channels, expected_dependencies.query_channels);
  validate_dependency_mask(
      "command class", m_dependencySummary.command_classes, expected_dependencies.command_classes);
  validate_dependency_mask(
      "access class", m_dependencySummary.access_classes, expected_dependencies.access_classes);
  validate_dependency_mask(
      "dynamic flag", m_dependencySummary.dynamic_flags, expected_dependencies.dynamic_flags);
  validate_dependency_mask("state preservation",
                           m_dependencySummary.state_preservation,
                           expected_dependencies.state_preservation);
  validate_dependency_mask("main-thread reason",
                           m_dependencySummary.main_thread_reason_mask,
                           expected_dependencies.main_thread_reason_mask);
  validate_dependency_mask("ray-query detail",
                           m_dependencySummary.ray_query_detail_requirements,
                           expected_dependencies.ray_query_detail_requirements);
  if (m_dependencySummary.worker_lane_eligible != expected_dependencies.worker_lane_eligible) {
    errors.push_back("dependency summary worker-lane eligibility does not match runtime semantics");
  }
  if (m_schedulerSummary.worker_lane_eligible && !m_dependencySummary.worker_lane_eligible) {
    errors.push_back("scheduler summary allows a dependency main-thread-only program");
  }

  auto validate_expression_semantics = [&](const char *table_name,
                                           const size_t index,
                                           const LN_RuntimeExpressionSemantics &semantics) {
    if (!semantics.known) {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": kind is missing runtime semantics entry");
      return;
    }
    const uint32_t minimum_resources = MinimumSchedulerResourcesForExpressionSemantics(semantics);
    if ((minimum_resources & ~m_schedulerSummary.resource_access) != 0u) {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": scheduler summary is missing semantic resource access");
    }
    if (semantics.threading == LN_RuntimeSemanticThreading::MainThreadRecord &&
        m_schedulerSummary.worker_lane_eligible)
    {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": scheduler summary allows main-thread-only expression");
    }
    if ((semantics.fallback_requirements & LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION) != 0u &&
        (semantics.reads & LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE) == 0u)
    {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": generic conversion fallback does not declare program-table reads");
    }
    if ((semantics.fallback_requirements & LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP) != 0u &&
        (semantics.reads & LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE) == 0u)
    {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": dynamic lookup fallback does not declare program-table reads");
    }
    if ((semantics.fallback_requirements & LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ) != 0u &&
        semantics.threading != LN_RuntimeSemanticThreading::MainThreadRecord)
    {
      errors.push_back(std::string(table_name) + " expression " + std::to_string(index) +
                       ": main-thread live-read fallback is not main-thread-only");
    }
  };

  for (size_t index = 0; index < m_boolExpressions.size(); index++) {
    validate_expression_semantics("bool",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_boolExpressions[index].kind));
  }
  for (size_t index = 0; index < m_floatExpressions.size(); index++) {
    validate_expression_semantics("float",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_floatExpressions[index].kind));
  }
  for (size_t index = 0; index < m_intExpressions.size(); index++) {
    validate_expression_semantics("int",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_intExpressions[index].kind));
  }
  for (size_t index = 0; index < m_stringExpressions.size(); index++) {
    validate_expression_semantics("string",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_stringExpressions[index].kind));
  }
  for (size_t index = 0; index < m_vectorExpressions.size(); index++) {
    validate_expression_semantics("vector",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_vectorExpressions[index].kind));
  }
  for (size_t index = 0; index < m_colorExpressions.size(); index++) {
    validate_expression_semantics("color",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_colorExpressions[index].kind));
  }
  for (size_t index = 0; index < m_valueExpressions.size(); index++) {
    validate_expression_semantics("value",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_valueExpressions[index].kind));
  }
  for (size_t index = 0; index < m_queryExpressions.size(); index++) {
    validate_expression_semantics("query",
                                  index,
                                  LN_GetRuntimeExpressionSemantics(
                                      m_queryExpressions[index].kind));
  }

  for (size_t index = 0; index < m_queryExpressions.size(); index++) {
    const LN_QueryExpression &expression = m_queryExpressions[index];
    if (expression.runtime_state_index >= m_queryRuntimeStateCount) {
      errors.push_back("query expression " + std::to_string(index) +
                       ": runtime state id is outside dense state table");
    }
  }

  for (size_t index = 0; index < m_boolExpressions.size(); index++) {
    const LN_BoolExpression &expression = m_boolExpressions[index];
    if ((expression.kind == LN_BoolExpressionKind::TimerElapsed ||
         expression.kind == LN_BoolExpressionKind::DelayDone ||
         expression.kind == LN_BoolExpressionKind::PulsifyPulse ||
         expression.kind == LN_BoolExpressionKind::BarrierPassed ||
         expression.kind == LN_BoolExpressionKind::CooldownAccepted ||
         expression.kind == LN_BoolExpressionKind::CooldownBlocked ||
         expression.kind == LN_BoolExpressionKind::CooldownCompleted ||
         expression.kind == LN_BoolExpressionKind::CooldownReady) &&
        (expression.int_value < 0 || uint32_t(expression.int_value) >= m_timeFlowStateCount))
    {
      errors.push_back("bool expression " + std::to_string(index) +
                       ": time-flow state id is outside dense state table");
    }
    if (expression.kind == LN_BoolExpressionKind::BooleanEdge &&
        (expression.input0 == LN_INVALID_INDEX || expression.input0 >= m_boolExpressions.size()))
    {
      errors.push_back("bool expression " + std::to_string(index) +
                       ": Boolean Edge condition expression is invalid");
    }
    if (expression.kind == LN_BoolExpressionKind::BooleanEdgeFalling) {
      if (expression.input0 == LN_INVALID_INDEX || expression.input0 >= m_boolExpressions.size()) {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": Boolean Edge master expression is invalid");
      }
      else if (m_boolExpressions[expression.input0].kind !=
               LN_BoolExpressionKind::BooleanEdge)
      {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": falling edge does not reference a Boolean Edge expression");
      }
    }
    if (expression.kind == LN_BoolExpressionKind::InputStatus) {
      if (expression.int_value <= 0) {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": input code is not compiled");
      }
      if (expression.secondary_int_value < 1 || expression.secondary_int_value > 3) {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": input status is outside supported event status range");
      }
    }
    if (expression.kind == LN_BoolExpressionKind::RigidBodyConstraintFound &&
        expression.rigid_body_constraint_match_mode > LN_RigidBodyConstraintMatchMode::All)
    {
      errors.push_back("bool expression " + std::to_string(index) +
                       ": rigid body constraint match mode is invalid");
    }
    if (expression.kind == LN_BoolExpressionKind::RigidBodyConstraintFound) {
      if (expression.input0 != LN_INVALID_INDEX && expression.input0 >= m_valueExpressions.size()) {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": rigid body constraint object expression is invalid");
      }
      if (expression.rigid_body_constraint_match_mode !=
              LN_RigidBodyConstraintMatchMode::All &&
          expression.property_ref_index >= m_stringExpressions.size())
      {
        errors.push_back("bool expression " + std::to_string(index) +
                         ": rigid body constraint name expression is invalid");
      }
    }
  }
  for (size_t index = 0; index < m_floatExpressions.size(); index++) {
    const LN_FloatExpression &expression = m_floatExpressions[index];
    if ((expression.kind == LN_FloatExpressionKind::CooldownRemaining ||
         expression.kind == LN_FloatExpressionKind::CooldownProgress) &&
        (expression.int_value < 0 || uint32_t(expression.int_value) >= m_timeFlowStateCount))
    {
      errors.push_back("float expression " + std::to_string(index) +
                       ": time-flow state id is outside dense state table");
    }
  }
  for (size_t index = 0; index < m_stringExpressions.size(); index++) {
    const LN_StringExpression &expression = m_stringExpressions[index];
    if (expression.kind == LN_StringExpressionKind::RigidBodyConstraintName &&
        expression.rigid_body_constraint_match_mode > LN_RigidBodyConstraintMatchMode::All)
    {
      errors.push_back("string expression " + std::to_string(index) +
                       ": rigid body constraint match mode is invalid");
    }
    if (expression.kind == LN_StringExpressionKind::RigidBodyConstraintName) {
      if (expression.value_expr_index != LN_INVALID_INDEX &&
          expression.value_expr_index >= m_valueExpressions.size())
      {
        errors.push_back("string expression " + std::to_string(index) +
                         ": rigid body constraint object expression is invalid");
      }
      if (expression.rigid_body_constraint_match_mode !=
              LN_RigidBodyConstraintMatchMode::All &&
          expression.input0 >= m_stringExpressions.size())
      {
        errors.push_back("string expression " + std::to_string(index) +
                         ": rigid body constraint name expression is invalid");
      }
    }
  }
  for (size_t index = 0; index < m_valueExpressions.size(); index++) {
    const LN_ValueExpression &expression = m_valueExpressions[index];
    if (expression.kind == LN_ValueExpressionKind::RigidBodyConstraintNames &&
        expression.rigid_body_constraint_match_mode > LN_RigidBodyConstraintMatchMode::All)
    {
      errors.push_back("value expression " + std::to_string(index) +
                       ": rigid body constraint match mode is invalid");
    }
    if (expression.kind == LN_ValueExpressionKind::RigidBodyConstraintNames) {
      if (expression.input0 != LN_INVALID_INDEX && expression.input0 >= m_valueExpressions.size()) {
        errors.push_back("value expression " + std::to_string(index) +
                         ": rigid body constraint object expression is invalid");
      }
      if (expression.rigid_body_constraint_match_mode !=
              LN_RigidBodyConstraintMatchMode::All &&
          expression.string_expr_index >= m_stringExpressions.size())
      {
        errors.push_back("value expression " + std::to_string(index) +
                         ": rigid body constraint name expression is invalid");
      }
    }
  }

  ValidateExecBlockProgram(GetExecBlockProgram(LN_Event::OnInit), m_onInit, errors);
  ValidateExecBlockProgram(
      GetExecBlockProgram(LN_Event::OnFixedUpdate), m_onFixedUpdate, errors);
  ValidateRegisterExpressionProgram(m_registerExpressionProgram, errors);

  if (r_errors != nullptr) {
    *r_errors = errors;
  }
  return errors.empty();
}

bool LN_Program::IsParallelEligible() const
{
  return m_schedulerSummary.worker_lane_eligible && m_dependencySummary.worker_lane_eligible;
}

LN_MainThreadOnlyReason LN_Program::GetMainThreadOnlyReason() const
{
  return m_schedulerSummary.main_thread_only_reason;
}

std::string LN_Program::DescribeSchedulerSummary() const
{
  std::ostringstream stream;
  stream << "purity=" << SchedulerPurityName(m_schedulerSummary.purity);
  stream << " phase=" << RequiredPhaseName(m_schedulerSummary.required_phase);
  stream << " emits_commands=" << (m_schedulerSummary.emits_commands ? "true" : "false");
  stream << " resources=";
  AppendResourceAccessNames(stream, m_schedulerSummary.resource_access);
  stream << " uses_jolt_queries_or_contacts="
         << (m_schedulerSummary.uses_jolt_queries_or_contacts ? "true" : "false");
  stream << " reads_snapshot_only="
         << (m_schedulerSummary.reads_snapshot_only ? "true" : "false");
  stream << " estimated_work=" << EstimatedWorkClassName(m_schedulerSummary.estimated_work_class);
  stream << " worker_lane_eligible="
         << (m_schedulerSummary.worker_lane_eligible ? "true" : "false");
  stream << " main_thread_only_reason="
         << MainThreadOnlyReasonName(m_schedulerSummary.main_thread_only_reason);
  return stream.str();
}

void LN_Program::AddInstructionDependencies(const LN_Event /*event*/,
                                            const LN_Instruction &instruction)
{
  const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(
      instruction.opcode);
  if (semantics != nullptr) {
    AddSemanticReadDependencies(m_dependencySummary, semantics->reads);
  }
  AddInstructionDependencyDetails(m_dependencySummary, instruction, m_stringExpressions);
}

void LN_Program::AddBoolExpressionDependencies(const LN_BoolExpression &expression)
{
  AddBoolExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddFloatExpressionDependencies(const LN_FloatExpression &expression)
{
  AddFloatExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddIntExpressionDependencies(const LN_IntExpression &expression)
{
  AddIntExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddStringExpressionDependencies(const LN_StringExpression &expression)
{
  AddStringExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddVectorExpressionDependencies(const LN_VectorExpression &expression)
{
  AddVectorExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddQueryExpressionDependencies(const LN_QueryExpression &expression)
{
  AddQueryExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddColorExpressionDependencies(const LN_ColorExpression &expression)
{
  AddColorExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::AddValueExpressionDependencies(const LN_ValueExpression &expression)
{
  AddValueExpressionDependencyDetails(m_dependencySummary, expression);
}

void LN_Program::UpdateSchedulerSummaryForInstruction(const LN_Event event,
                                                      const LN_Instruction &instruction)
{
  if (event == LN_Event::OnFixedUpdate) {
    m_schedulerSummary.required_phase = LN_SchedulerRequiredPhase::OnFixedUpdate;
  }
  else if (m_schedulerSummary.required_phase == LN_SchedulerRequiredPhase::None) {
    m_schedulerSummary.required_phase = LN_SchedulerRequiredPhase::OnInit;
  }

  const uint32_t resource_access = CommandResourceAccessForOpcode(instruction.opcode);
  if (OpcodeEmitsCommands(instruction.opcode)) {
    const LN_EstimatedWorkClass work_class =
        (instruction.opcode == LN_OpCode::Navigate) ? LN_EstimatedWorkClass::Heavy :
                                                       LN_EstimatedWorkClass::Small;
    MarkCommandWrite(m_schedulerSummary, resource_access, work_class);
  }
  else {
    AddResourceAccess(m_schedulerSummary, resource_access);
  }

  if (instruction.opcode == LN_OpCode::GamepadLook || instruction.opcode == LN_OpCode::MouseLook) {
    MarkInputRead(m_schedulerSummary);
  }

  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForOpcode(instruction.opcode);
  if (reason != LN_MainThreadOnlyReason::None) {
    switch (reason) {
      case LN_MainThreadOnlyReason::GlobalPropertyPersistence:
        AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_GLOBAL_STATE);
        break;
      case LN_MainThreadOnlyReason::FilePersistence:
        AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_FILE);
        break;
      case LN_MainThreadOnlyReason::PhysicsConstraint:
        AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_PHYSICS);
        break;
      case LN_MainThreadOnlyReason::InputDeviceState:
        AddResourceAccess(m_schedulerSummary,
                          LN_SCHEDULER_RESOURCE_INPUT | LN_SCHEDULER_RESOURCE_RENDER);
        break;
      case LN_MainThreadOnlyReason::SpawnPool:
        AddResourceAccess(m_schedulerSummary,
                          LN_SCHEDULER_RESOURCE_WORLD | LN_SCHEDULER_RESOURCE_PHYSICS);
        PromoteWorkClass(m_schedulerSummary, LN_EstimatedWorkClass::Heavy);
        break;
      case LN_MainThreadOnlyReason::CollectionVisibility:
      case LN_MainThreadOnlyReason::OverlayCollection:
        AddResourceAccess(m_schedulerSummary,
                          LN_SCHEDULER_RESOURCE_SCENE | LN_SCHEDULER_RESOURCE_RENDER);
        break;
      case LN_MainThreadOnlyReason::DebugDraw:
        AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_RENDER);
        PromoteWorkClass(m_schedulerSummary, LN_EstimatedWorkClass::Medium);
        break;
      default:
        break;
    }
    MarkMainThreadOnly(m_schedulerSummary, reason);
  }
}

void LN_Program::UpdateSchedulerSummaryForBoolExpression(const LN_BoolExpression &expression)
{
  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForBoolExpression(expression.kind);
  if (reason != LN_MainThreadOnlyReason::None) {
    MarkQueryCacheRead(m_schedulerSummary, reason);
    if (expression.kind == LN_BoolExpressionKind::CollisionEnter ||
        expression.kind == LN_BoolExpressionKind::CollisionStay ||
        expression.kind == LN_BoolExpressionKind::CollisionExit)
    {
      MarkRuntimeTreeStateRead(m_schedulerSummary);
    }
    return;
  }

  switch (expression.kind) {
    case LN_BoolExpressionKind::SnapshotGameProperty:
    case LN_BoolExpressionKind::SnapshotVisibility:
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
    case LN_BoolExpressionKind::SnapshotCharacterOnGround:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_BoolExpressionKind::RuntimeTreeProperty:
    case LN_BoolExpressionKind::LogicTreeRunning:
    case LN_BoolExpressionKind::LogicTreeStopped:
    case LN_BoolExpressionKind::ActionDone:
    case LN_BoolExpressionKind::AnimationPlaying:
    case LN_BoolExpressionKind::LoopActive:
    case LN_BoolExpressionKind::StoreValueDone:
    case LN_BoolExpressionKind::InstructionExecuted:
    case LN_BoolExpressionKind::InstructionReached:
    case LN_BoolExpressionKind::Once:
    case LN_BoolExpressionKind::OnNextTick:
    case LN_BoolExpressionKind::Timer:
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo:
    case LN_BoolExpressionKind::TweenValue:
    case LN_BoolExpressionKind::TweenReached:
    case LN_BoolExpressionKind::SpawnPoolSpawnedPulse:
    case LN_BoolExpressionKind::SpawnPoolHitPulse:
    case LN_BoolExpressionKind::TimerElapsed:
    case LN_BoolExpressionKind::DelayDone:
    case LN_BoolExpressionKind::PulsifyPulse:
    case LN_BoolExpressionKind::BarrierPassed:
    case LN_BoolExpressionKind::BooleanEdge:
    case LN_BoolExpressionKind::BooleanEdgeFalling:
    case LN_BoolExpressionKind::CooldownAccepted:
    case LN_BoolExpressionKind::CooldownBlocked:
    case LN_BoolExpressionKind::CooldownCompleted:
    case LN_BoolExpressionKind::CooldownReady:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_BoolExpressionKind::InputStatus:
    case LN_BoolExpressionKind::KeyboardActive:
    case LN_BoolExpressionKind::MouseMoved:
    case LN_BoolExpressionKind::MouseWheelMoved:
    case LN_BoolExpressionKind::GamepadActive:
    case LN_BoolExpressionKind::GamepadButton:
    case LN_BoolExpressionKind::KeyLoggerPressed:
    case LN_BoolExpressionKind::WindowFullscreen:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_BoolExpressionKind::EventReceived:
      MarkEventBusRead(m_schedulerSummary);
      break;
    case LN_BoolExpressionKind::PhysicsQueryDone:
    case LN_BoolExpressionKind::PhysicsQueryHit:
    case LN_BoolExpressionKind::PhysicsQueryBlocked:
    case LN_BoolExpressionKind::PhysicsQueryHasUV:
    case LN_BoolExpressionKind::PhysicsQueryStartedOverlapping:
    case LN_BoolExpressionKind::MouseOverEnter:
    case LN_BoolExpressionKind::MouseOverOver:
    case LN_BoolExpressionKind::MouseOverExit:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
    case LN_BoolExpressionKind::MaterialSlotFound:
    case LN_BoolExpressionKind::MaterialNodeValueFound:
    case LN_BoolExpressionKind::EditorNodeValueFound:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_DATABLOCK);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_BoolExpressionKind::RigidBodyAttribute:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_BoolExpressionKind::RigidBodyConstraintFound:
      AddResourceAccess(m_schedulerSummary,
                        LN_SCHEDULER_RESOURCE_SCENE | LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForFloatExpression(const LN_FloatExpression &expression)
{
  switch (expression.kind) {
    case LN_FloatExpressionKind::SnapshotGameProperty:
    case LN_FloatExpressionKind::SnapshotTimeScale:
    case LN_FloatExpressionKind::SnapshotLightPower:
    case LN_FloatExpressionKind::SnapshotElapsedTime:
    case LN_FloatExpressionKind::SnapshotFrameDelta:
    case LN_FloatExpressionKind::SnapshotFPS:
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_FloatExpressionKind::RuntimeTreeProperty:
    case LN_FloatExpressionKind::StoreValue:
    case LN_FloatExpressionKind::TweenFactor:
    case LN_FloatExpressionKind::TweenFloatResult:
    case LN_FloatExpressionKind::CooldownRemaining:
    case LN_FloatExpressionKind::CooldownProgress:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_FloatExpressionKind::GamepadButtonStrength:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_FloatExpressionKind::ObjectDistance:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_SCENE);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_FloatExpressionKind::PhysicsQueryDistance:
    case LN_FloatExpressionKind::PhysicsQueryFraction:
    case LN_FloatExpressionKind::PhysicsQueryPenetrationDepth:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
    case LN_FloatExpressionKind::RigidBodyAttribute:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForIntExpression(const LN_IntExpression &expression)
{
  switch (expression.kind) {
    case LN_IntExpressionKind::SnapshotGameProperty:
    case LN_IntExpressionKind::SnapshotCollisionGroup:
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
    case LN_IntExpressionKind::SnapshotCharacterJumpCount:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_IntExpressionKind::RuntimeTreeProperty:
    case LN_IntExpressionKind::LoopIndex:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_IntExpressionKind::MouseWheelDelta:
    case LN_IntExpressionKind::WindowResolutionWidth:
    case LN_IntExpressionKind::WindowResolutionHeight:
    case LN_IntExpressionKind::WindowVSyncMode:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_IntExpressionKind::MaterialSlotCount:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_DATABLOCK);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_IntExpressionKind::CollisionContactCount:
      MarkQueryCacheRead(m_schedulerSummary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_IntExpressionKind::PhysicsQueryFaceIndex:
    case LN_IntExpressionKind::PhysicsQueryHitCount:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForStringExpression(const LN_StringExpression &expression)
{
  switch (expression.kind) {
    case LN_StringExpressionKind::SnapshotGameProperty:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_StringExpressionKind::RuntimeTreeProperty:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_StringExpressionKind::KeyLoggerCharacter:
    case LN_StringExpressionKind::KeyLoggerKeycode:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_StringExpressionKind::MaterialName:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_DATABLOCK);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_StringExpressionKind::RigidBodyConstraintName:
      AddResourceAccess(m_schedulerSummary,
                        LN_SCHEDULER_RESOURCE_SCENE | LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForVectorExpression(const LN_VectorExpression &expression)
{
  switch (expression.kind) {
    case LN_VectorExpressionKind::SnapshotWorldPosition:
    case LN_VectorExpressionKind::SnapshotLocalPosition:
    case LN_VectorExpressionKind::SnapshotWorldOrientation:
    case LN_VectorExpressionKind::SnapshotLocalOrientation:
    case LN_VectorExpressionKind::SnapshotWorldScale:
    case LN_VectorExpressionKind::SnapshotLocalScale:
    case LN_VectorExpressionKind::SnapshotLinearVelocity:
    case LN_VectorExpressionKind::SnapshotLocalLinearVelocity:
    case LN_VectorExpressionKind::SnapshotAngularVelocity:
    case LN_VectorExpressionKind::SnapshotLocalAngularVelocity:
    case LN_VectorExpressionKind::SnapshotGravity:
    case LN_VectorExpressionKind::WorldToScreen:
    case LN_VectorExpressionKind::BoneHeadWorld:
    case LN_VectorExpressionKind::BoneTailWorld:
    case LN_VectorExpressionKind::BoneCenterWorld:
    case LN_VectorExpressionKind::BoneHeadPoseWorld:
    case LN_VectorExpressionKind::BoneTailPoseWorld:
    case LN_VectorExpressionKind::BoneCenterPoseWorld:
    case LN_VectorExpressionKind::BonePoseLocation:
    case LN_VectorExpressionKind::BonePoseScale:
    case LN_VectorExpressionKind::ScreenToWorld:
    case LN_VectorExpressionKind::SnapshotCharacterGravity:
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection:
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::WindowResolution:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::CursorPosition:
    case LN_VectorExpressionKind::CursorMovement:
    case LN_VectorExpressionKind::GamepadStick:
      MarkInputRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::PhysicsQueryPoint:
    case LN_VectorExpressionKind::PhysicsQueryNormal:
    case LN_VectorExpressionKind::PhysicsQueryCastPosition:
    case LN_VectorExpressionKind::PhysicsQueryDirection:
    case LN_VectorExpressionKind::PhysicsQueryEndPoint:
    case LN_VectorExpressionKind::PhysicsQueryUV:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::CollisionHitPoint:
    case LN_VectorExpressionKind::CollisionHitNormal:
      MarkQueryCacheRead(m_schedulerSummary,
                         LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_VectorExpressionKind::GroupCenterPosition:
      AddResourceAccess(m_schedulerSummary,
                        LN_SCHEDULER_RESOURCE_SCENE | LN_SCHEDULER_RESOURCE_DATABLOCK);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_VectorExpressionKind::InstructionNextPoint:
    case LN_VectorExpressionKind::TweenVectorResult:
    case LN_VectorExpressionKind::TweenRotationResult:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_VectorExpressionKind::RigidBodyAttribute:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForQueryExpression(const LN_QueryExpression &expression)
{
  switch (expression.kind) {
    case LN_QueryExpressionKind::Raycast:
    case LN_QueryExpressionKind::RaycastAll:
    case LN_QueryExpressionKind::ShapeCast:
    case LN_QueryExpressionKind::ShapeCastAll:
      MarkQueryCacheRead(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForColorExpression(const LN_ColorExpression &expression)
{
  switch (expression.kind) {
    case LN_ColorExpressionKind::SnapshotObjectColor:
    case LN_ColorExpressionKind::SnapshotLightColor:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    default:
      break;
  }
}

void LN_Program::UpdateSchedulerSummaryForValueExpression(const LN_ValueExpression &expression)
{
  const LN_MainThreadOnlyReason reason = MainThreadOnlyReasonForValueExpression(expression.kind);
  if (reason == LN_MainThreadOnlyReason::GlobalPropertyPersistence) {
    AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_GLOBAL_STATE);
    MarkMainThreadOnly(m_schedulerSummary, reason);
    return;
  }
  if (reason == LN_MainThreadOnlyReason::FilePersistence) {
    AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_FILE);
    MarkMainThreadOnly(m_schedulerSummary, reason);
    return;
  }
  if (reason == LN_MainThreadOnlyReason::CollisionOrPhysicsQuery) {
    MarkQueryCacheRead(m_schedulerSummary, reason);
    return;
  }

  switch (expression.kind) {
    case LN_ValueExpressionKind::SnapshotGameProperty:
    case LN_ValueExpressionKind::OwnerObject:
    case LN_ValueExpressionKind::ObjectParent:
    case LN_ValueExpressionKind::ObjectChild:
    case LN_ValueExpressionKind::ObjectChildByName:
    case LN_ValueExpressionKind::ObjectAttribute:
    case LN_ValueExpressionKind::BoneAttribute:
    case LN_ValueExpressionKind::BonePoseRotation:
    case LN_ValueExpressionKind::ObjectGameProperty:
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_ValueExpressionKind::RuntimeTreeProperty:
    case LN_ValueExpressionKind::LoopCurrentValue:
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew:
    case LN_ValueExpressionKind::ListAppend:
    case LN_ValueExpressionKind::ListRemoveIndex:
    case LN_ValueExpressionKind::ListRemoveValue:
    case LN_ValueExpressionKind::ListSetIndex:
    case LN_ValueExpressionKind::DictSetKey:
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue:
      MarkRuntimeTreeStateRead(m_schedulerSummary);
      break;
    case LN_ValueExpressionKind::EventContent:
    case LN_ValueExpressionKind::EventMessenger:
      MarkEventBusRead(m_schedulerSummary);
      break;
    case LN_ValueExpressionKind::PhysicsQueryObject:
    case LN_ValueExpressionKind::PhysicsQueryObjects:
    case LN_ValueExpressionKind::PhysicsQueryPoints:
    case LN_ValueExpressionKind::PhysicsQueryNormals:
    case LN_ValueExpressionKind::PhysicsQueryDistances:
    case LN_ValueExpressionKind::PhysicsQueryCastPositions:
    case LN_ValueExpressionKind::PhysicsQueryFractions:
    case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
    case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
    case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
    case LN_ValueExpressionKind::PhysicsQueryHasUVs:
    case LN_ValueExpressionKind::PhysicsQueryUVs:
    case LN_ValueExpressionKind::ProjectileParabola:
      MarkSnapshotQueryCacheRead(m_schedulerSummary);
      break;
    case LN_ValueExpressionKind::CollisionHitObject:
    case LN_ValueExpressionKind::CollisionHitObjects:
    case LN_ValueExpressionKind::CollisionHitPoints:
    case LN_ValueExpressionKind::CollisionHitNormals:
    case LN_ValueExpressionKind::SpawnPoolHitObject:
      MarkQueryCacheRead(m_schedulerSummary,
                         LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
      break;
    case LN_ValueExpressionKind::CurrentScene:
    case LN_ValueExpressionKind::CollectionObjects:
    case LN_ValueExpressionKind::CollectionObjectNames:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_SCENE);
      MarkSnapshotRead(m_schedulerSummary);
      break;
    case LN_ValueExpressionKind::MaterialSlot:
    case LN_ValueExpressionKind::MaterialNodeValue:
    case LN_ValueExpressionKind::EditorNodeValue:
      AddResourceAccess(m_schedulerSummary, LN_SCHEDULER_RESOURCE_DATABLOCK);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    case LN_ValueExpressionKind::RigidBodyConstraintNames:
      AddResourceAccess(m_schedulerSummary,
                        LN_SCHEDULER_RESOURCE_SCENE | LN_SCHEDULER_RESOURCE_PHYSICS);
      m_schedulerSummary.reads_snapshot_only = false;
      MarkMainThreadOnly(m_schedulerSummary, LN_MainThreadOnlyReason::QueryExpression);
      break;
    default:
      break;
  }
}
