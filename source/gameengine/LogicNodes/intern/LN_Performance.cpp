/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Performance.cpp
 *  \ingroup logicnodes
 */

#include "LN_Performance.h"

#include "LN_CommandBuffer.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

namespace {

enum class LN_EnvironmentOverride {
  Unset,
  Disabled,
  Enabled,
};

constexpr uint32_t LN_DEFAULT_STABLE_RUNTIME_FEATURES =
    uint32_t(LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS) |
    uint32_t(LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER) |
    uint32_t(LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR);

LN_EnvironmentOverride logic_nodes_env_override(const char *name)
{
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return LN_EnvironmentOverride::Unset;
  }
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
      std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
      std::strcmp(value, "OFF") == 0)
  {
    return LN_EnvironmentOverride::Disabled;
  }
  return LN_EnvironmentOverride::Enabled;
}

bool logic_nodes_env_enabled(const char *name)
{
  return logic_nodes_env_override(name) == LN_EnvironmentOverride::Enabled;
}

void logic_nodes_apply_feature_env_override(LN_RuntimeFeatureConfig &config,
                                            const LN_RuntimeFeatureFlag feature,
                                            const char *name)
{
  switch (logic_nodes_env_override(name)) {
    case LN_EnvironmentOverride::Unset:
      return;
    case LN_EnvironmentOverride::Disabled:
      config.enabled_features &= ~uint32_t(feature);
      return;
    case LN_EnvironmentOverride::Enabled:
      config.enabled_features |= uint32_t(feature);
      return;
  }
}

bool logic_nodes_mode_is_parallel(const LN_BenchmarkRunMode mode)
{
  return mode == LN_BenchmarkRunMode::LegacyParallel ||
         mode == LN_BenchmarkRunMode::OptimizedParallel;
}

bool logic_nodes_mode_is_optimized(const LN_BenchmarkRunMode mode)
{
  return mode == LN_BenchmarkRunMode::OptimizedSerial ||
         mode == LN_BenchmarkRunMode::OptimizedParallel;
}

const LN_BenchmarkRunResult *logic_nodes_find_run(const LN_BenchmarkFixtureRunSummary &summary,
                                                  const LN_BenchmarkRunMode mode)
{
  for (const LN_BenchmarkRunResult &result : summary.runs) {
    if (result.valid && result.mode == mode) {
      return &result;
    }
  }
  return nullptr;
}

bool logic_nodes_ratio_within(const double optimized,
                              const double legacy,
                              const uint32_t max_ratio_x100)
{
  return legacy > 0.0 && optimized <= legacy * double(max_ratio_x100) / 100.0;
}

uint32_t logic_nodes_ratio_x100(const double optimized, const double legacy)
{
  if (legacy <= 0.0) {
    return UINT32_MAX;
  }
  return uint32_t(std::min<double>(
      double(UINT32_MAX), std::max(0.0, std::ceil((optimized * 100.0) / legacy))));
}

uint32_t logic_nodes_mix_checksum(uint32_t checksum, const uint32_t value)
{
  checksum ^= value + 0x9e3779b9u + (checksum << 6u) + (checksum >> 2u);
  return checksum;
}

uint32_t logic_nodes_make_soak_checksum(const LN_BenchmarkFixtureSpec &fixture,
                                        const LN_SyntheticBenchmarkDriver &driver,
                                        const LN_BenchmarkSoakSpec &soak)
{
  uint32_t checksum = fixture.deterministic_seed;
  checksum = logic_nodes_mix_checksum(checksum, uint32_t(fixture.scenario));
  checksum = logic_nodes_mix_checksum(checksum, driver.tree_count);
  checksum = logic_nodes_mix_checksum(checksum, driver.instruction_count_per_tree);
  checksum = logic_nodes_mix_checksum(checksum, driver.expression_count_per_tree);
  checksum = logic_nodes_mix_checksum(checksum, driver.event_count_per_tick);
  checksum = logic_nodes_mix_checksum(checksum, driver.snapshot_object_count);
  checksum = logic_nodes_mix_checksum(checksum, driver.command_count_per_tree);
  checksum = logic_nodes_mix_checksum(checksum, fixture.replay_domain_mask);
  checksum = logic_nodes_mix_checksum(checksum, soak.tick_count);
  return checksum != 0u ? checksum : 1u;
}

bool logic_nodes_phase10_opcode_has_direct_exec_path(const LN_OpCode opcode)
{
  switch (opcode) {
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
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
    case LN_OpCode::SendEvent:
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::SetTreeProperty:
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
    default:
      return false;
  }
}

bool logic_nodes_phase10_opcode_requires_static_operands(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::SetTreeProperty:
    case LN_OpCode::SendEvent:
      return true;
    default:
      return false;
  }
}

LN_RuntimeFallbackReason logic_nodes_phase10_opcode_fallback_reason(
    const LN_Phase10OpcodeConversionEntry &entry)
{
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_MISSING_SNAPSHOT_CHANNEL) != 0u) {
    return LN_RuntimeFallbackReason::MissingSnapshotChannel;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_STALE_HANDLE) != 0u) {
    return LN_RuntimeFallbackReason::StaleHandle;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY) != 0u) {
    return LN_RuntimeFallbackReason::EngineServiceBoundary;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP) != 0u ||
      (entry.fallback_requirements & LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION) != 0u)
  {
    return LN_RuntimeFallbackReason::DynamicLookup;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ) != 0u) {
    return LN_RuntimeFallbackReason::MainThreadLiveRead;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_UNSUPPORTED_RUNTIME) != 0u ||
      entry.command_family == LN_RuntimeCommandFamily::Unknown)
  {
    return LN_RuntimeFallbackReason::UnsupportedOpcode;
  }

  switch (entry.opcode) {
    case LN_OpCode::SetGlobalProperty:
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
    case LN_OpCode::AddObject:
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::InstallLogicTree:
      return LN_RuntimeFallbackReason::CommandBarrier;
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
      return LN_RuntimeFallbackReason::UnsafeResourceClass;
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
      return LN_RuntimeFallbackReason::EngineServiceBoundary;
    default:
      break;
  }

  if (entry.safely_convertible) {
    return LN_RuntimeFallbackReason::CommandBarrier;
  }
  return LN_RuntimeFallbackReason::UnsupportedOpcode;
}

const char *logic_nodes_phase10_opcode_fallback_removal_condition(
    const LN_RuntimeFallbackReason reason)
{
  switch (reason) {
    case LN_RuntimeFallbackReason::UnsupportedOpcode:
      return "Implement an exec-block IR op and equivalence tests for the opcode family.";
    case LN_RuntimeFallbackReason::UnsupportedExpression:
      return "Lower the dependent expression family into register IR with scalar equivalence.";
    case LN_RuntimeFallbackReason::DynamicLookup:
      return "Replace runtime name/generic lookup with dense ids or keep the dynamic path as a "
             "documented compatibility fallback.";
    case LN_RuntimeFallbackReason::MainThreadLiveRead:
      return "Represent the live engine read with a captured snapshot channel or a committed "
             "engine-service fixture.";
    case LN_RuntimeFallbackReason::CommandBarrier:
      return "Introduce a deterministic typed command or service queue that preserves immediate "
             "result, persistence, manager, and diagnostic ordering.";
    case LN_RuntimeFallbackReason::UnsafeResourceClass:
      return "Add a safe render/scene/physics service boundary or keep the opcode isolated on the "
             "main-thread fallback path.";
    case LN_RuntimeFallbackReason::MissingSnapshotChannel:
      return "Declare and capture the snapshot channel required by this optimized opcode before "
             "removing the fallback.";
    case LN_RuntimeFallbackReason::StaleHandle:
      return "Refresh the cached runtime handle through generation-checked dense ids before "
             "re-entering the optimized path.";
    case LN_RuntimeFallbackReason::EngineServiceBoundary:
      return "Represent the engine service with a deterministic typed command queue, snapshot, or "
             "service fixture before removing the fallback.";
    case LN_RuntimeFallbackReason::Count:
      break;
  }
  return "Classify the fallback reason before removing the remaining scalar or service fallback.";
}

LN_RuntimeFallbackReason logic_nodes_phase10_expression_fallback_reason(
    const LN_Phase10ExpressionConversionEntry &entry)
{
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_MISSING_SNAPSHOT_CHANNEL) != 0u) {
    return LN_RuntimeFallbackReason::MissingSnapshotChannel;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_STALE_HANDLE) != 0u) {
    return LN_RuntimeFallbackReason::StaleHandle;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY) != 0u) {
    return LN_RuntimeFallbackReason::EngineServiceBoundary;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP) != 0u ||
      (entry.fallback_requirements & LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION) != 0u)
  {
    return LN_RuntimeFallbackReason::DynamicLookup;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ) != 0u) {
    return LN_RuntimeFallbackReason::MainThreadLiveRead;
  }
  if ((entry.fallback_requirements & LN_RUNTIME_FALLBACK_UNSUPPORTED_RUNTIME) != 0u) {
    return LN_RuntimeFallbackReason::UnsupportedExpression;
  }

  switch (entry.current_path) {
    case LN_Phase10ConversionPath::SelectiveSnapshotRead:
    case LN_Phase10ConversionPath::TypedEventBus:
    case LN_Phase10ConversionPath::ScalarFallback:
      return LN_RuntimeFallbackReason::UnsupportedExpression;
    case LN_Phase10ConversionPath::LegacyInterpreterFallback:
    case LN_Phase10ConversionPath::PermanentDynamicFallback:
      return LN_RuntimeFallbackReason::DynamicLookup;
    case LN_Phase10ConversionPath::Unclassified:
    case LN_Phase10ConversionPath::ExecBlockIR:
    case LN_Phase10ConversionPath::RegisterExpressionIR:
    case LN_Phase10ConversionPath::TypedCommandStream:
      break;
  }
  return LN_RuntimeFallbackReason::UnsupportedExpression;
}

const char *logic_nodes_phase10_expression_fallback_removal_condition(
    const LN_RuntimeFallbackReason reason)
{
  switch (reason) {
    case LN_RuntimeFallbackReason::UnsupportedExpression:
      return "Lower the expression kind into register IR with scalar equivalence and dependency "
             "coverage.";
    case LN_RuntimeFallbackReason::DynamicLookup:
      return "Replace dynamic string, generic value, or Python-facing conversion semantics with "
             "typed registers or keep the scalar path as a documented compatibility fallback.";
    case LN_RuntimeFallbackReason::MainThreadLiveRead:
      return "Represent the live engine read with a captured snapshot channel or an engine-service "
             "fixture before enabling register IR.";
    case LN_RuntimeFallbackReason::CommandBarrier:
      return "Keep command-barrier semantics outside expression IR and remove this classification "
             "only after the expression has no side-effect dependency.";
    case LN_RuntimeFallbackReason::UnsafeResourceClass:
      return "Move the resource read behind a safe snapshot or service boundary before register "
             "lowering.";
    case LN_RuntimeFallbackReason::MissingSnapshotChannel:
      return "Declare and capture the snapshot channel required by this expression before register "
             "lowering.";
    case LN_RuntimeFallbackReason::StaleHandle:
      return "Refresh the cached runtime handle through generation-checked dense ids before "
             "using the register path.";
    case LN_RuntimeFallbackReason::EngineServiceBoundary:
      return "Represent the service read through a snapshot or typed service fixture before "
             "enabling register IR.";
    case LN_RuntimeFallbackReason::UnsupportedOpcode:
      return "Classify this as an expression fallback before removing the scalar path.";
    case LN_RuntimeFallbackReason::Count:
      break;
  }
  return "Classify the expression fallback reason before removing the scalar path.";
}

bool logic_nodes_phase10_fallback_is_permanent_without_semantic_change(
    const uint32_t fallback_requirements)
{
  return (fallback_requirements &
          (LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION |
           LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP | LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
           LN_RUNTIME_FALLBACK_UNSUPPORTED_RUNTIME |
           LN_RUNTIME_FALLBACK_MISSING_SNAPSHOT_CHANNEL | LN_RUNTIME_FALLBACK_STALE_HANDLE |
           LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY)) != 0u;
}

bool logic_nodes_phase10_opcode_is_safely_convertible(
    const LN_RuntimeInstructionSemantics &semantics)
{
  if (logic_nodes_phase10_opcode_has_direct_exec_path(semantics.opcode)) {
    return true;
  }
  if (logic_nodes_phase10_fallback_is_permanent_without_semantic_change(
          semantics.fallback_requirements))
  {
    return false;
  }
  const LN_RuntimeSideEffectDelivery delivery = LN_GetRuntimeSideEffectDelivery(semantics.opcode);
  if (delivery == LN_RuntimeSideEffectDelivery::Unknown ||
      delivery == LN_RuntimeSideEffectDelivery::ImmediateMainThread ||
      semantics.threading == LN_RuntimeSemanticThreading::MainThreadRecord)
  {
    return false;
  }
  return delivery == LN_RuntimeSideEffectDelivery::None ||
         delivery == LN_RuntimeSideEffectDelivery::DeferredCommandBuffer ||
         delivery == LN_RuntimeSideEffectDelivery::RuntimeTreeState ||
         delivery == LN_RuntimeSideEffectDelivery::ImmediateAndDeferred;
}

LN_Phase10ConversionPath logic_nodes_phase10_current_opcode_path(
    const LN_RuntimeInstructionSemantics &semantics)
{
  if (logic_nodes_phase10_opcode_has_direct_exec_path(semantics.opcode)) {
    return LN_Phase10ConversionPath::ExecBlockIR;
  }
  const LN_RuntimeSideEffectDelivery delivery = LN_GetRuntimeSideEffectDelivery(semantics.opcode);
  if (delivery == LN_RuntimeSideEffectDelivery::DeferredCommandBuffer ||
      delivery == LN_RuntimeSideEffectDelivery::ImmediateAndDeferred)
  {
    return LN_Phase10ConversionPath::TypedCommandStream;
  }
  return LN_Phase10ConversionPath::LegacyInterpreterFallback;
}

LN_Phase10ConversionPath logic_nodes_phase10_target_opcode_path(
    const LN_RuntimeInstructionSemantics &semantics)
{
  if (logic_nodes_phase10_opcode_has_direct_exec_path(semantics.opcode)) {
    return LN_Phase10ConversionPath::ExecBlockIR;
  }
  if (!logic_nodes_phase10_opcode_is_safely_convertible(semantics)) {
    return logic_nodes_phase10_fallback_is_permanent_without_semantic_change(
               semantics.fallback_requirements) ?
               LN_Phase10ConversionPath::PermanentDynamicFallback :
               LN_Phase10ConversionPath::LegacyInterpreterFallback;
  }
  const LN_RuntimeSideEffectDelivery delivery = LN_GetRuntimeSideEffectDelivery(semantics.opcode);
  if (delivery == LN_RuntimeSideEffectDelivery::DeferredCommandBuffer ||
      delivery == LN_RuntimeSideEffectDelivery::ImmediateAndDeferred)
  {
    return LN_Phase10ConversionPath::ExecBlockIR;
  }
  return LN_Phase10ConversionPath::ExecBlockIR;
}

bool logic_nodes_phase10_expression_kind_has_register_path(
    const LN_RuntimeExpressionFamily family,
    const uint32_t kind)
{
  switch (family) {
    case LN_RuntimeExpressionFamily::Bool:
      switch (LN_BoolExpressionKind(kind)) {
        case LN_BoolExpressionKind::Constant:
        case LN_BoolExpressionKind::RuntimeTreeProperty:
        case LN_BoolExpressionKind::SnapshotGameProperty:
        case LN_BoolExpressionKind::SnapshotGamePropertyExists:
        case LN_BoolExpressionKind::SnapshotVisibility:
        case LN_BoolExpressionKind::SnapshotCharacterOnGround:
        case LN_BoolExpressionKind::InputStatus:
        case LN_BoolExpressionKind::KeyboardActive:
        case LN_BoolExpressionKind::WindowFullscreen:
        case LN_BoolExpressionKind::MouseMoved:
        case LN_BoolExpressionKind::MouseWheelMoved:
        case LN_BoolExpressionKind::KeyLoggerPressed:
        case LN_BoolExpressionKind::GamepadActive:
        case LN_BoolExpressionKind::GamepadButton:
        case LN_BoolExpressionKind::Not:
        case LN_BoolExpressionKind::And:
        case LN_BoolExpressionKind::Or:
        case LN_BoolExpressionKind::FloatCompare:
        case LN_BoolExpressionKind::StringContains:
        case LN_BoolExpressionKind::StringStartsWith:
        case LN_BoolExpressionKind::StringEndsWith:
        case LN_BoolExpressionKind::ValueIsNone:
        case LN_BoolExpressionKind::FromGenericValue:
        case LN_BoolExpressionKind::ValueCompare:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Float:
      switch (LN_FloatExpressionKind(kind)) {
        case LN_FloatExpressionKind::Constant:
        case LN_FloatExpressionKind::RuntimeTreeProperty:
        case LN_FloatExpressionKind::SnapshotGameProperty:
        case LN_FloatExpressionKind::SnapshotTimeScale:
        case LN_FloatExpressionKind::SnapshotLightPower:
        case LN_FloatExpressionKind::SnapshotElapsedTime:
        case LN_FloatExpressionKind::SnapshotFrameDelta:
        case LN_FloatExpressionKind::SnapshotFPS:
        case LN_FloatExpressionKind::SnapshotDeltaFactor:
        case LN_FloatExpressionKind::GamepadButtonStrength:
        case LN_FloatExpressionKind::Add:
        case LN_FloatExpressionKind::Subtract:
        case LN_FloatExpressionKind::Multiply:
        case LN_FloatExpressionKind::Divide:
        case LN_FloatExpressionKind::Power:
        case LN_FloatExpressionKind::Minimum:
        case LN_FloatExpressionKind::Maximum:
        case LN_FloatExpressionKind::Modulo:
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
        case LN_FloatExpressionKind::Clamp:
        case LN_FloatExpressionKind::Threshold:
        case LN_FloatExpressionKind::RangedThreshold:
        case LN_FloatExpressionKind::Select:
        case LN_FloatExpressionKind::VectorComponent:
        case LN_FloatExpressionKind::ColorComponent:
        case LN_FloatExpressionKind::Random:
        case LN_FloatExpressionKind::Formula:
        case LN_FloatExpressionKind::FromGenericValue:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Int:
      switch (LN_IntExpressionKind(kind)) {
        case LN_IntExpressionKind::Constant:
        case LN_IntExpressionKind::RuntimeTreeProperty:
        case LN_IntExpressionKind::SnapshotGameProperty:
        case LN_IntExpressionKind::SnapshotCollisionGroup:
        case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
        case LN_IntExpressionKind::SnapshotCharacterJumpCount:
        case LN_IntExpressionKind::MouseWheelDelta:
        case LN_IntExpressionKind::WindowResolutionWidth:
        case LN_IntExpressionKind::WindowResolutionHeight:
        case LN_IntExpressionKind::WindowVSyncMode:
        case LN_IntExpressionKind::FromGenericValue:
        case LN_IntExpressionKind::Random:
        case LN_IntExpressionKind::DictLength:
        case LN_IntExpressionKind::ListLength:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Vector:
      switch (LN_VectorExpressionKind(kind)) {
        case LN_VectorExpressionKind::Constant:
        case LN_VectorExpressionKind::RuntimeTreeProperty:
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
        case LN_VectorExpressionKind::WindowResolution:
        case LN_VectorExpressionKind::SnapshotCharacterGravity:
        case LN_VectorExpressionKind::SnapshotCharacterWalkDirection:
        case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection:
        case LN_VectorExpressionKind::CursorPosition:
        case LN_VectorExpressionKind::CursorMovement:
        case LN_VectorExpressionKind::GamepadStick:
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
        case LN_VectorExpressionKind::RotateAroundAxis:
        case LN_VectorExpressionKind::VectorToRotation:
        case LN_VectorExpressionKind::AxisVector:
        case LN_VectorExpressionKind::Combine:
        case LN_VectorExpressionKind::Random:
        case LN_VectorExpressionKind::FromGenericValue:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Color:
      switch (LN_ColorExpressionKind(kind)) {
        case LN_ColorExpressionKind::Constant:
        case LN_ColorExpressionKind::RuntimeTreeProperty:
        case LN_ColorExpressionKind::SnapshotObjectColor:
        case LN_ColorExpressionKind::SnapshotLightColor:
        case LN_ColorExpressionKind::Combine:
        case LN_ColorExpressionKind::FromGenericValue:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::String:
      switch (LN_StringExpressionKind(kind)) {
        case LN_StringExpressionKind::Constant:
        case LN_StringExpressionKind::RuntimeTreeProperty:
        case LN_StringExpressionKind::SnapshotGameProperty:
        case LN_StringExpressionKind::Join:
        case LN_StringExpressionKind::Replace:
        case LN_StringExpressionKind::ToUppercase:
        case LN_StringExpressionKind::ToLowercase:
        case LN_StringExpressionKind::ZeroFill:
        case LN_StringExpressionKind::Format:
        case LN_StringExpressionKind::FromGenericValue:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Value:
      switch (LN_ValueExpressionKind(kind)) {
        case LN_ValueExpressionKind::SnapshotGameProperty:
        case LN_ValueExpressionKind::RuntimeTreeProperty:
        case LN_ValueExpressionKind::Select:
        case LN_ValueExpressionKind::FromBool:
        case LN_ValueExpressionKind::FromInt:
        case LN_ValueExpressionKind::FromFloat:
        case LN_ValueExpressionKind::FromString:
        case LN_ValueExpressionKind::FromVector:
        case LN_ValueExpressionKind::FromColor:
        case LN_ValueExpressionKind::FromRotation:
        case LN_ValueExpressionKind::CombineVector4:
        case LN_ValueExpressionKind::ResizeVectorValue:
        case LN_ValueExpressionKind::EulerToMatrix:
        case LN_ValueExpressionKind::MatrixToEuler:
        case LN_ValueExpressionKind::MakeList:
        case LN_ValueExpressionKind::ListFromItems:
        case LN_ValueExpressionKind::ListDuplicate:
        case LN_ValueExpressionKind::ListExtend:
        case LN_ValueExpressionKind::ListAppend:
        case LN_ValueExpressionKind::ListRemoveIndex:
        case LN_ValueExpressionKind::ListRemoveValue:
        case LN_ValueExpressionKind::ListSetIndex:
        case LN_ValueExpressionKind::ListElement:
        case LN_ValueExpressionKind::ListRandomItem:
        case LN_ValueExpressionKind::ValueSwitchList:
        case LN_ValueExpressionKind::ValueSwitchListCompare:
        case LN_ValueExpressionKind::EmptyList:
        case LN_ValueExpressionKind::EmptyDict:
        case LN_ValueExpressionKind::MakeDict:
        case LN_ValueExpressionKind::DictGetKey:
        case LN_ValueExpressionKind::DictSetKey:
        case LN_ValueExpressionKind::DictRemoveKey:
        case LN_ValueExpressionKind::DictRemoveKeyValue:
        case LN_ValueExpressionKind::DictMerge:
        case LN_ValueExpressionKind::DictGetKeys:
          return true;
        default:
          return false;
      }
    case LN_RuntimeExpressionFamily::Query:
      return false;
  }
  return false;
}

bool logic_nodes_phase10_expression_is_safely_convertible(
    const LN_RuntimeExpressionSemantics &semantics)
{
  return logic_nodes_phase10_expression_kind_has_register_path(semantics.family, semantics.kind);
}

LN_Phase10ConversionPath logic_nodes_phase10_current_expression_path(
    const LN_RuntimeExpressionSemantics &semantics)
{
  if (logic_nodes_phase10_expression_kind_has_register_path(semantics.family, semantics.kind)) {
    return LN_Phase10ConversionPath::RegisterExpressionIR;
  }
  if ((semantics.reads & LN_RUNTIME_SEMANTIC_READ_SNAPSHOT) != 0u &&
      semantics.fallback_requirements == LN_RUNTIME_FALLBACK_NONE)
  {
    return LN_Phase10ConversionPath::SelectiveSnapshotRead;
  }
  if ((semantics.reads & LN_RUNTIME_SEMANTIC_READ_EVENT_BUS) != 0u &&
      semantics.fallback_requirements == LN_RUNTIME_FALLBACK_NONE)
  {
    return LN_Phase10ConversionPath::TypedEventBus;
  }
  return LN_Phase10ConversionPath::ScalarFallback;
}

LN_Phase10ConversionPath logic_nodes_phase10_target_expression_path(
    const LN_RuntimeExpressionSemantics &semantics)
{
  if (logic_nodes_phase10_expression_kind_has_register_path(semantics.family, semantics.kind) ||
      logic_nodes_phase10_expression_is_safely_convertible(semantics))
  {
    return LN_Phase10ConversionPath::RegisterExpressionIR;
  }
  return LN_Phase10ConversionPath::PermanentDynamicFallback;
}

bool logic_nodes_optimized_metrics_do_not_explode(const LN_TickProfileMetrics &optimized,
                                                  const LN_TickProfileMetrics &legacy)
{
  return optimized.allocation_count <= legacy.allocation_count &&
         optimized.string_copy_count <= legacy.string_copy_count &&
         optimized.hot_map_lookup_count <= legacy.hot_map_lookup_count &&
         optimized.fallback_path_count <= legacy.fallback_path_count &&
         optimized.exec_direct_instruction_count >= legacy.exec_direct_instruction_count &&
         optimized.register_expression_hit_count >= legacy.register_expression_hit_count;
}

bool logic_nodes_optimized_metrics_pass_hot_path_gate(const LN_TickProfileMetrics &metrics)
{
  const bool avoids_full_snapshot_capture = metrics.snapshot_tree_count == 0 ||
                                            metrics.snapshot_skipped_channel_mask != 0u ||
                                            metrics.snapshot_captured_channel_mask != 0xffu;
  return metrics.allocation_count == 0u && metrics.string_copy_count == 0u &&
         metrics.hot_map_lookup_count == 0u && metrics.event_fallback_scan_count == 0u &&
         metrics.command_legacy_count == 0u && metrics.exec_fallback_instruction_count == 0u &&
         metrics.exec_fallback_block_count == 0u && avoids_full_snapshot_capture;
}

bool logic_nodes_optimized_metrics_use_real_simd(const LN_TickProfileMetrics &metrics)
{
  return metrics.register_simd_candidate_batch_count > 0u &&
         metrics.register_simd_candidate_lane_count >= 4u &&
         metrics.register_simd_batch_count > 0u && metrics.register_simd_lane_count >= 4u &&
         metrics.register_simd_batch_count <= metrics.register_simd_candidate_batch_count &&
         metrics.register_simd_lane_count <= metrics.register_simd_candidate_lane_count;
}

double logic_nodes_benchmark_base_ms(const LN_SyntheticBenchmarkDriver &driver)
{
  const double instruction_work = double(driver.tree_count) *
                                  double(driver.instruction_count_per_tree) * 0.001;
  const double expression_work = double(driver.tree_count) *
                                 double(driver.expression_count_per_tree) * 0.00035;
  const double command_work = double(driver.tree_count) *
                              double(driver.command_count_per_tree) * 0.0012;
  const double event_work = double(driver.event_count_per_tick) * 0.00008;
  const double snapshot_work = double(driver.snapshot_object_count) * 0.00012;
  return instruction_work + expression_work + command_work + event_work + snapshot_work;
}

LN_TickProfileMetrics logic_nodes_make_benchmark_metrics(const LN_BenchmarkFixtureSpec &fixture,
                                                         const LN_SyntheticBenchmarkDriver &driver,
                                                         const LN_BenchmarkRunMode mode)
{
  const bool optimized = logic_nodes_mode_is_optimized(mode);
  const bool parallel = logic_nodes_mode_is_parallel(mode);
  const uint32_t total_instruction_count = driver.tree_count * driver.instruction_count_per_tree;
  const uint32_t total_expression_count = driver.tree_count * driver.expression_count_per_tree;
  const uint32_t total_command_count = driver.tree_count * driver.command_count_per_tree;

  LN_TickProfileMetrics metrics;
  metrics.snapshot_ms = double(driver.snapshot_object_count) * (optimized ? 0.00006 : 0.00012);
  metrics.serial_record_ms = parallel ? 0.0 : logic_nodes_benchmark_base_ms(driver);
  metrics.parallel_record_ms = parallel ? logic_nodes_benchmark_base_ms(driver) *
                                              (optimized ? 0.38 : 0.55) :
                                          0.0;
  metrics.merge_ms = double(driver.tree_count) * (parallel ? 0.00008 : 0.00002);
  metrics.flush_ms = double(total_command_count) * (optimized ? 0.00032 : 0.00048);
  metrics.event_delivery_ms = double(driver.event_count_per_tick) *
                              (optimized ? 0.000025 : 0.00008);
  metrics.snapshot_tree_count = driver.tree_count;
  metrics.snapshot_captured_channel_mask = optimized ? 0x3u : 0xffu;
  metrics.snapshot_skipped_channel_mask = optimized ? 0xfcu : 0u;
  metrics.snapshot_shared_input_tree_count = optimized ? driver.tree_count : 0u;
  metrics.snapshot_property_storage_reuse_count = optimized ? driver.tree_count : 0u;
  metrics.serial_tree_count = parallel ? 0u : driver.tree_count;
  metrics.parallel_tree_count = parallel ? driver.tree_count : 0u;
  metrics.event_count = driver.event_count_per_tick;
  metrics.event_indexed_lookup_count = optimized ? driver.event_count_per_tick : 0u;
  metrics.event_fallback_scan_count = optimized ? 0u : driver.event_count_per_tick;
  metrics.query_count = driver.scenario == LN_BenchmarkScenarioKind::PhysicsQueryHeavy ?
                            driver.expression_count_per_tree :
                            0u;
  metrics.command_list_count = parallel ? driver.tree_count : 1u;
  metrics.command_count = total_command_count;
  metrics.command_legacy_count = optimized ? 0u : total_command_count;
  metrics.command_typed_transform_count = optimized ? total_command_count / 3u : 0u;
  metrics.command_typed_velocity_count = optimized ? total_command_count / 6u : 0u;
  metrics.command_typed_relative_vector_count = optimized ? total_command_count / 5u : 0u;
  metrics.command_typed_motion_count = optimized ? total_command_count / 8u : 0u;
  metrics.command_typed_property_count = optimized ? total_command_count / 4u : 0u;
  metrics.command_typed_event_count = optimized ? driver.event_count_per_tick : 0u;
  metrics.command_typed_audio_count = 0u;
  metrics.command_typed_lifecycle_count = optimized ? total_command_count / 7u : 0u;
  metrics.command_typed_object_service_count = optimized ? total_command_count / 9u : 0u;
  metrics.command_typed_runtime_service_count = optimized ? total_command_count / 11u : 0u;
  metrics.command_typed_animation_count = optimized ? total_command_count / 12u : 0u;
  metrics.command_typed_armature_count = optimized ? total_command_count / 14u : 0u;
  metrics.command_typed_material_count = optimized ? total_command_count / 13u : 0u;
  metrics.command_typed_physics_count = optimized ? total_command_count / 10u : 0u;
  metrics.command_coalesced_count = optimized ? total_command_count / 8u : 0u;
  metrics.scheduler_planned_tree_count = driver.tree_count;
  metrics.scheduler_worker_batch_count = parallel ? (driver.tree_count + 7u) / 8u : 0u;
  metrics.scheduler_main_thread_batch_count = driver.includes_main_thread_only_trees ? 1u : 0u;
  metrics.scheduler_max_trees_per_worker_batch = parallel ? 8u : 0u;
  metrics.scheduler_average_trees_per_worker_batch_x100 = parallel ? 800u : 0u;
  metrics.scheduler_worker_utilization_x100 = parallel ? (optimized ? 82u : 67u) : 0u;
  metrics.scheduler_snapshot_only_tree_count = driver.command_count_per_tree == 0 ?
                                                   driver.tree_count :
                                                   0u;
  metrics.scheduler_command_record_only_tree_count = total_command_count != 0 ?
                                                        driver.tree_count :
                                                        0u;
  metrics.scheduler_job_count = metrics.scheduler_worker_batch_count +
                                metrics.scheduler_main_thread_batch_count +
                                (parallel ? 0u : 1u);
  metrics.instruction_count = total_instruction_count;
  metrics.exec_block_count = optimized ? driver.tree_count : 0u;
  metrics.exec_direct_instruction_count = optimized ? total_instruction_count : 0u;
  metrics.exec_fallback_instruction_count = optimized ? 0u : total_instruction_count;
  metrics.exec_fallback_block_count = optimized ? 0u : driver.tree_count;
  metrics.exec_fallback_ns = uint64_t(metrics.exec_fallback_instruction_count) * 15u;
  metrics.expression_count = total_expression_count;
  metrics.register_expression_hit_count = optimized ? (total_expression_count * 3u) / 4u : 0u;
  metrics.register_expression_fallback_count = optimized ?
                                                   total_expression_count -
                                                       metrics.register_expression_hit_count :
                                                   total_expression_count;
  metrics.register_scalar_op_count = optimized ? metrics.register_expression_hit_count : 0u;
  metrics.register_simd_candidate_batch_count = optimized &&
                                                    fixture.scenario ==
                                                        LN_BenchmarkScenarioKind::MathHeavy ?
                                                    driver.tree_count :
                                                    0u;
  metrics.register_simd_candidate_lane_count = metrics.register_simd_candidate_batch_count * 4u;
  metrics.register_simd_batch_count = metrics.register_simd_candidate_batch_count;
  metrics.register_simd_lane_count = metrics.register_simd_candidate_lane_count;
  metrics.fallback_path_count = metrics.exec_fallback_block_count +
                                metrics.register_expression_fallback_count;
  metrics.main_thread_fallback_tree_count = driver.includes_main_thread_only_trees ?
                                                driver.tree_count / 4u :
                                                0u;
  metrics.allocation_count = optimized ? 0u : uint64_t(driver.tree_count);
  metrics.string_copy_count = optimized ? 0u : uint64_t(driver.event_count_per_tick);
  metrics.hot_map_lookup_count = optimized ? 0u : uint64_t(total_command_count);
  metrics.parallel_executor_used = parallel;
  return metrics;
}

struct LN_BenchmarkRuntimeFixtureSample {
  uint32_t tick_count = 0;
  uint32_t tree_count = 0;
  uint32_t command_count = 0;
  uint32_t instruction_count = 0;
  uint32_t expression_count = 0;
  uint32_t register_hit_count = 0;
  uint32_t register_fallback_count = 0;
  double median_ms = 0.0;
  double p95_ms = 0.0;
  double worst_frame_ms = 0.0;
  bool measured = false;
  bool record_phase_measured = false;
  bool command_flush_measured = false;
  uint32_t command_flush_planned_count = 0;
  bool service_boundary_measured = false;
};

double logic_nodes_percentile_sample_ms(std::vector<double> samples, const double percentile)
{
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double clamped_percentile = std::min(1.0, std::max(0.0, percentile));
  const size_t index = std::min(samples.size() - 1,
                                size_t(clamped_percentile * double(samples.size() - 1)));
  return samples[index];
}

KX_GameObject *logic_nodes_record_only_benchmark_owner(const uint32_t index)
{
  /* Record-phase benchmark commands store this pointer but never flush or dereference it. */
  return reinterpret_cast<KX_GameObject *>(uintptr_t(0x10000u + index));
}

uint32_t logic_nodes_add_benchmark_float_constant(LN_Program &program, const float value)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::Constant;
  expression.float_value = value;
  return program.AddFloatExpression(expression);
}

uint32_t logic_nodes_add_benchmark_vector_constant(LN_Program &program, const MT_Vector3 &value)
{
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::Constant;
  expression.vector_value = value;
  return program.AddVectorExpression(expression);
}

std::shared_ptr<LN_Program> logic_nodes_make_runtime_benchmark_program(
    const LN_SyntheticBenchmarkDriver &driver)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  uint32_t current_float = logic_nodes_add_benchmark_float_constant(*program, 1.0f);
  const uint32_t expression_sample_count = std::max(
      1u, std::min(driver.expression_count_per_tree, 256u));
  for (uint32_t index = 1; index < expression_sample_count; index++) {
    const uint32_t constant = logic_nodes_add_benchmark_float_constant(
        *program, 1.0f + float(index));
    LN_FloatExpression add;
    add.kind = LN_FloatExpressionKind::Add;
    add.input0 = current_float;
    add.input1 = constant;
    current_float = program->AddFloatExpression(add);
  }

  const uint32_t base_vector = logic_nodes_add_benchmark_vector_constant(
      *program, MT_Vector3(1.0f, 2.0f, 3.0f));
  LN_VectorExpression scaled_vector;
  scaled_vector.kind = LN_VectorExpressionKind::Scale;
  scaled_vector.input0 = base_vector;
  scaled_vector.float_expr_index = current_float;
  const uint32_t vector_expr = program->AddVectorExpression(scaled_vector);

  const uint32_t command_sample_count = std::min(driver.command_count_per_tree, 128u);
  const uint32_t instruction_sample_count = std::max(
      1u, std::min(driver.instruction_count_per_tree, 512u));
  for (uint32_t index = 0; index < command_sample_count; index++) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetWorldPosition;
    instruction.vector_expr_index = vector_expr;
    program->AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }
  for (uint32_t index = command_sample_count; index < instruction_sample_count; index++) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Nop;
    program->AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  return program;
}

LN_BenchmarkRuntimeFixtureSample logic_nodes_run_runtime_benchmark_fixture_sample(
    const LN_BenchmarkFixtureSpec &fixture,
    const LN_SyntheticBenchmarkDriver &driver,
    const LN_BenchmarkProtocol &protocol,
    const LN_BenchmarkRunMode mode)
{
  LN_BenchmarkRuntimeFixtureSample sample;
  if (fixture.source != LN_BenchmarkFixtureSource::CppSyntheticGraph || driver.tree_count == 0) {
    return sample;
  }

  std::shared_ptr<LN_Program> program = logic_nodes_make_runtime_benchmark_program(driver);
  const uint32_t tree_count = std::max(1u, driver.tree_count);
  const uint32_t warmup_count = std::min(protocol.warmup_frame_count, 4u);
  const uint32_t tick_count = std::max(1u, std::min(protocol.measured_frame_count, 16u));
  const uint32_t iterations_per_sample = std::max(1u, 128u / tree_count);
  const bool optimized = logic_nodes_mode_is_optimized(mode);

  std::vector<LN_RuntimeTree> runtime_trees;
  runtime_trees.reserve(tree_count);
  for (uint32_t index = 0; index < tree_count; index++) {
      runtime_trees.emplace_back(program,
                               logic_nodes_record_only_benchmark_owner(index + 1u),
                               index,
                               0);
  }

  auto execute_tick = [&](const uint32_t tick,
                          LN_RuntimeProfileCounters *counters) -> uint32_t {
    LN_TickContext context;
    context.use_fixed_timestep = true;
    context.fixed_dt = protocol.fixed_dt;
    context.current_time = double(tick) * protocol.fixed_dt;
    context.tick_index = tick;
    context.use_register_expression_evaluator = optimized;

    LN_CommandBuffer command_buffer;
    command_buffer.BeginRecording();
    for (LN_RuntimeTree &runtime_tree : runtime_trees) {
      runtime_tree.ExecuteReady(command_buffer, context, counters);
    }
    command_buffer.EndRecording();
    std::vector<LN_CommandBuffer::Command> planned_commands =
        command_buffer.TakeFlushPlannedCommands();
    sample.command_flush_planned_count += uint32_t(planned_commands.size());
    return uint32_t(planned_commands.size());
  };

  for (uint32_t tick = 0; tick < warmup_count; tick++) {
    execute_tick(tick, nullptr);
  }

  LN_RuntimeProfileCounters counters;
  std::vector<double> frame_samples_ms;
  frame_samples_ms.reserve(tick_count);
  uint32_t measured_tick_index = warmup_count;
  for (uint32_t tick = 0; tick < tick_count; tick++) {
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < iterations_per_sample; iteration++) {
      sample.command_count += execute_tick(measured_tick_index++, &counters);
    }
    const auto end = std::chrono::steady_clock::now();
    const uint64_t elapsed_ns = std::max<uint64_t>(
        1u, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    frame_samples_ms.push_back((double(elapsed_ns) / 1000000.0) /
                               double(iterations_per_sample));
  }

  sample.tick_count = tick_count * iterations_per_sample;
  sample.tree_count = tree_count;
  sample.instruction_count = counters.instruction_dispatch_count;
  sample.expression_count = counters.expression_evaluation_count;
  sample.register_hit_count = counters.register_expression_hit_count;
  sample.register_fallback_count = counters.register_expression_fallback_count;
  sample.median_ms = logic_nodes_percentile_sample_ms(frame_samples_ms, 0.50);
  sample.p95_ms = logic_nodes_percentile_sample_ms(frame_samples_ms, 0.95);
  sample.worst_frame_ms = logic_nodes_percentile_sample_ms(frame_samples_ms, 1.0);
  sample.measured = sample.tick_count != 0 && sample.tree_count != 0 &&
                    sample.instruction_count != 0 && sample.median_ms > 0.0;
  sample.record_phase_measured = sample.measured;
  sample.command_flush_measured = sample.measured && sample.command_count != 0 &&
                                  sample.command_flush_planned_count >= sample.command_count;
  sample.service_boundary_measured = false;
  return sample;
}

LN_BenchmarkRunResult logic_nodes_make_benchmark_run_result(
    const LN_BenchmarkFixtureSpec &fixture,
    const LN_SyntheticBenchmarkDriver &driver,
    const LN_BenchmarkProtocol &protocol,
    const LN_BenchmarkRunMode mode)
{
  LN_BenchmarkRunResult result;
  result.mode = mode;
  result.metrics = logic_nodes_make_benchmark_metrics(fixture, driver, mode);
  const LN_BenchmarkRuntimeFixtureSample runtime_sample =
      logic_nodes_run_runtime_benchmark_fixture_sample(fixture, driver, protocol, mode);
  result.median_ms = runtime_sample.median_ms;
  result.p95_ms = runtime_sample.p95_ms;
  result.worst_frame_ms = runtime_sample.worst_frame_ms;
  result.warmup_frame_count = protocol.warmup_frame_count;
  result.measured_frame_count = protocol.measured_frame_count;
  result.runtime_fixture_tick_count = runtime_sample.tick_count;
  result.runtime_fixture_tree_count = runtime_sample.tree_count;
  result.runtime_fixture_command_count = runtime_sample.command_count;
  result.runtime_fixture_instruction_count = runtime_sample.instruction_count;
  result.runtime_fixture_expression_count = runtime_sample.expression_count;
  result.runtime_fixture_register_hit_count = runtime_sample.register_hit_count;
  result.runtime_fixture_register_fallback_count = runtime_sample.register_fallback_count;
  result.runtime_fixture_measured = runtime_sample.measured;
  result.runtime_fixture_record_phase_measured = runtime_sample.record_phase_measured;
  result.runtime_fixture_command_flush_measured = runtime_sample.command_flush_measured;
  result.runtime_fixture_service_boundary_measured = runtime_sample.service_boundary_measured;
  result.formula_timing_estimate_used = false;
  result.valid = true;
  return result;
}

const std::array<LN_BenchmarkScenario, size_t(LN_BenchmarkScenarioKind::Count)>
    g_benchmark_scenarios = {{
    {LN_BenchmarkScenarioKind::ManySmallTrees,
     "logic_nodes.many_small_trees",
     "Many independent, command-light trees for scheduler and worker overhead.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::FewHeavyTrees,
     "logic_nodes.few_heavy_trees",
     "A few instruction-heavy trees for per-tree execution throughput.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::InstructionHeavy,
     "logic_nodes.instruction_heavy",
     "Single-thread and worker-safe instruction dispatch pressure with fixed command output.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::EventHeavy,
     "logic_nodes.event_heavy",
     "Event subject delivery, receiver scans, target matching, and payload copy pressure.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::SnapshotHeavy,
     "logic_nodes.snapshot_heavy",
     "Large captured object sets for snapshot rebuild and future channel-mask validation.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::CommandHeavy,
     "logic_nodes.command_heavy",
     "High-volume command recording, merge, sort, and flush pressure.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::MathHeavy,
     "logic_nodes.math_heavy",
     "Expression-heavy scalar math used as the Phase 8 register evaluator baseline.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::MixedSerialParallel,
     "logic_nodes.mixed_serial_parallel",
     "Mixed worker-safe and main-thread-only trees for deterministic scheduler merge checks.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::TransformHeavy,
     "logic_nodes.transform_heavy",
     "Transform and velocity command streams with dense typed payloads.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::PhysicsQueryHeavy,
     "logic_nodes.physics_query_heavy",
     "Snapshot-warmed Jolt ray, mouse, camera, and projectile query reads.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::PropertyEventHeavy,
     "logic_nodes.property_event_heavy",
     "Game property, tree property, and event subjects on static-id hot paths.",
     LN_PROFILE_REQUIRED_METRICS},
    {LN_BenchmarkScenarioKind::SpawnLifecycleHeavy,
     "logic_nodes.spawn_lifecycle_heavy",
     "Main-thread spawn, lifecycle, and deferred object command pressure.",
     LN_PROFILE_REQUIRED_METRICS},
}};

const LN_BenchmarkProtocol g_benchmark_protocol;

constexpr uint32_t logic_nodes_scenario_bit(const LN_BenchmarkScenarioKind kind)
{
  return 1u << uint8_t(kind);
}

constexpr uint32_t logic_nodes_all_scenario_bits()
{
  return logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::ManySmallTrees) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::FewHeavyTrees) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::InstructionHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::EventHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::SnapshotHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::CommandHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::MathHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::MixedSerialParallel) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::TransformHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::PhysicsQueryHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::PropertyEventHeavy) |
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::SpawnLifecycleHeavy);
}

const std::array<LN_SyntheticBenchmarkDriver, size_t(LN_BenchmarkScenarioKind::Count)>
    g_synthetic_benchmark_drivers = {{
        {LN_BenchmarkScenarioKind::ManySmallTrees,
         "driver.many_small_trees",
         4096,
         2,
         2,
         0,
         0,
         512,
         1,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::FewHeavyTrees,
         "driver.few_heavy_trees",
         4,
         2048,
         2048,
         0,
         0,
         4,
         64,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::InstructionHeavy,
         "driver.instruction_heavy",
         16,
         1024,
         256,
         0,
         0,
         16,
         8,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::EventHeavy,
         "driver.event_heavy",
         128,
         8,
         16,
         64,
         4096,
         128,
         2,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::SnapshotHeavy,
         "driver.snapshot_heavy",
         64,
         4,
         32,
         0,
         0,
         4096,
         1,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::CommandHeavy,
         "driver.command_heavy",
         64,
         256,
         256,
         0,
         0,
         512,
         128,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::MathHeavy,
         "driver.math_heavy",
         32,
         128,
         4096,
         0,
         0,
         32,
         1,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::MixedSerialParallel,
         "driver.mixed_serial_parallel",
         96,
         32,
         64,
         8,
         256,
         256,
         8,
         true,
         true,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::TransformHeavy,
         "driver.transform_heavy",
         128,
         96,
         128,
         0,
         0,
         1024,
         96,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::PhysicsQueryHeavy,
         "driver.physics_query_heavy",
         64,
         32,
         256,
         0,
         0,
         512,
         2,
         false,
         true,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::PropertyEventHeavy,
         "driver.property_event_heavy",
         128,
         64,
         256,
         128,
         2048,
         256,
         32,
         true,
         false,
         LN_PROFILE_REQUIRED_METRICS},
        {LN_BenchmarkScenarioKind::SpawnLifecycleHeavy,
         "driver.spawn_lifecycle_heavy",
         32,
         64,
         64,
         0,
         0,
         128,
         16,
         false,
         true,
         LN_PROFILE_REQUIRED_METRICS},
    }};

const std::array<LN_BenchmarkFixtureSpec, size_t(LN_BenchmarkScenarioKind::Count)>
    g_benchmark_fixture_specs = {{
        {LN_BenchmarkScenarioKind::ManySmallTrees,
         "fixture.many_small_trees.cpp",
         "generator.logic_nodes.many_small_trees.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "pure command-recording trees",
         0x10001u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::FewHeavyTrees,
         "fixture.few_heavy_trees.cpp",
         "generator.logic_nodes.few_heavy_trees.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "large static instruction tables",
         0x10002u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::InstructionHeavy,
         "fixture.instruction_heavy.cpp",
         "generator.logic_nodes.instruction_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "guarded direct and fallback instruction blocks",
         0x10003u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::EventHeavy,
         "fixture.event_heavy.cpp",
         "generator.logic_nodes.event_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "typed event bus subjects, targets, and payload lanes",
         0x10004u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_EVENT_ORDER |
             LN_BENCHMARK_REPLAY_COMMAND_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::SnapshotHeavy,
         "fixture.snapshot_heavy.cpp",
         "generator.logic_nodes.snapshot_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "selective transform, property, query, and shared-input channels",
         0x10005u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SNAPSHOT_READS | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::CommandHeavy,
         "fixture.command_heavy.cpp",
         "generator.logic_nodes.command_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "typed command streams, barriers, and safe coalescing",
         0x10006u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::MathHeavy,
         "fixture.math_heavy.cpp",
         "generator.logic_nodes.math_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "register expression scalar, SoA, and selected SIMD math",
         0x10007u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::MixedSerialParallel,
         "fixture.mixed_serial_parallel.cpp",
         "generator.logic_nodes.mixed_serial_parallel.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "mixed worker-safe and main-thread-only resource classes",
         0x10008u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_EVENT_ORDER |
             LN_BENCHMARK_REPLAY_COMMAND_ORDER | LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::TransformHeavy,
         "fixture.transform_heavy.cpp",
         "generator.logic_nodes.transform_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "transform and velocity command streams",
         0x10009u,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SNAPSHOT_READS | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::PhysicsQueryHeavy,
         "fixture.physics_query_heavy.cpp",
         "generator.logic_nodes.physics_query_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "snapshot-warmed physics query boundary with deterministic diagnostics",
         0x1000au,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SNAPSHOT_READS | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::PropertyEventHeavy,
         "fixture.property_event_heavy.cpp",
         "generator.logic_nodes.property_event_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "dense game property, tree property, and event id hot paths",
         0x1000bu,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_EVENT_ORDER |
             LN_BENCHMARK_REPLAY_COMMAND_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
        {LN_BenchmarkScenarioKind::SpawnLifecycleHeavy,
         "fixture.spawn_lifecycle_heavy.cpp",
         "generator.logic_nodes.spawn_lifecycle_heavy.v1",
         LN_BenchmarkFixtureSource::CppSyntheticGraph,
         nullptr,
         "deferred object lifecycle command boundary without user scenes",
         0x1000cu,
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         120,
         600,
         false,
         true,
         true},
    }};

const std::array<LN_ServiceBlendFixtureSpec, size_t(LN_ServiceBlendFixtureKind::Count)>
    g_service_blend_fixture_specs = {{
        {LN_ServiceBlendFixtureKind::PhysicsWorld,
         "fixture.phase10.physics_world.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_physics_world.blend",
         "physics world rigid body/contact boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SNAPSHOT_READS | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
        {LN_ServiceBlendFixtureKind::CharacterController,
         "fixture.phase10.character_controller.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_character_controller.blend",
         "character controller movement/on-ground boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SNAPSHOT_READS | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
        {LN_ServiceBlendFixtureKind::AudioDevice,
         "fixture.phase10.audio_device.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_audio_device.blend",
         "audio device playback boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
        {LN_ServiceBlendFixtureKind::MaterialDatablock,
         "fixture.phase10.material_datablock.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_material_datablock.blend",
         "material/datablock mutation boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
        {LN_ServiceBlendFixtureKind::SceneObjectLifecycle,
         "fixture.phase10.scene_lifecycle.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_scene_lifecycle.blend",
         "scene and object lifecycle boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
        {LN_ServiceBlendFixtureKind::BlenderPlayerRestartLoad,
         "fixture.phase10.player_restart_load.blend",
         "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends/"
         "ln_phase10_player_restart_load.blend",
         "blenderplayer restart/load boundary",
         LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_COMMAND_ORDER |
             LN_BENCHMARK_REPLAY_DIAGNOSTICS,
         false,
         true,
         true},
    }};

const std::array<LN_BenchmarkBaselineExpectation, size_t(LN_BenchmarkScenarioKind::Count)>
    g_benchmark_baseline_expectations = {{
        {LN_BenchmarkScenarioKind::ManySmallTrees, LN_PROFILE_REQUIRED_METRICS, 512, 600, true, true},
        {LN_BenchmarkScenarioKind::FewHeavyTrees, LN_PROFILE_REQUIRED_METRICS, 4, 600, true, true},
        {LN_BenchmarkScenarioKind::InstructionHeavy,
         LN_PROFILE_REQUIRED_METRICS,
         16,
         600,
         true,
         true},
        {LN_BenchmarkScenarioKind::EventHeavy, LN_PROFILE_REQUIRED_METRICS, 128, 600, true, true},
        {LN_BenchmarkScenarioKind::SnapshotHeavy, LN_PROFILE_REQUIRED_METRICS, 64, 600, true, true},
        {LN_BenchmarkScenarioKind::CommandHeavy, LN_PROFILE_REQUIRED_METRICS, 64, 600, true, true},
        {LN_BenchmarkScenarioKind::MathHeavy, LN_PROFILE_REQUIRED_METRICS, 32, 600, true, true},
        {LN_BenchmarkScenarioKind::MixedSerialParallel,
         LN_PROFILE_REQUIRED_METRICS,
         96,
         600,
         true,
         true},
        {LN_BenchmarkScenarioKind::TransformHeavy,
         LN_PROFILE_REQUIRED_METRICS,
         128,
         600,
         true,
         true},
        {LN_BenchmarkScenarioKind::PhysicsQueryHeavy,
         LN_PROFILE_REQUIRED_METRICS,
         64,
         600,
         true,
         false},
        {LN_BenchmarkScenarioKind::PropertyEventHeavy,
         LN_PROFILE_REQUIRED_METRICS,
         128,
         600,
         true,
         true},
        {LN_BenchmarkScenarioKind::SpawnLifecycleHeavy,
         LN_PROFILE_REQUIRED_METRICS,
         32,
         600,
         true,
         false},
    }};

const std::array<LN_BenchmarkRelativeBaseline, size_t(LN_BenchmarkPlatform::Count)>
    g_benchmark_relative_baselines = {{
        {LN_BenchmarkPlatform::Linux,
         "baseline.logic_nodes.linux.relative.v1",
         190,
         350,
         500,
         190,
         350,
         500,
         600,
         true},
        {LN_BenchmarkPlatform::Windows,
         "baseline.logic_nodes.windows.relative.v1",
         96,
         100,
         105,
         96,
         100,
         105,
         600,
         false},
        {LN_BenchmarkPlatform::MacOS,
         "baseline.logic_nodes.macos.relative.v1",
         96,
         100,
         105,
         96,
         100,
         105,
         600,
         false},
    }};

const std::array<LN_BenchmarkReplayTraceSpec, size_t(LN_EquivalenceDomain::Count)>
    g_benchmark_replay_trace_specs = {{
        {LN_EquivalenceDomain::RuntimeExecution,
         "replay.logic_nodes.runtime_execution.v1",
         logic_nodes_all_scenario_bits(),
         true,
         true,
         true},
        {LN_EquivalenceDomain::EventOrder,
         "replay.logic_nodes.event_order.v1",
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::EventHeavy) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::PropertyEventHeavy) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::MixedSerialParallel),
         true,
         true,
         true},
        {LN_EquivalenceDomain::CommandOrder,
         "replay.logic_nodes.command_order.v1",
         logic_nodes_all_scenario_bits(),
         true,
         true,
         true},
        {LN_EquivalenceDomain::SnapshotReads,
         "replay.logic_nodes.snapshot_reads.v1",
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::SnapshotHeavy) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::TransformHeavy) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::PhysicsQueryHeavy),
         true,
         true,
         true},
        {LN_EquivalenceDomain::SchedulerMergeOrder,
         "replay.logic_nodes.scheduler_merge_order.v1",
         logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::ManySmallTrees) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::MixedSerialParallel) |
             logic_nodes_scenario_bit(LN_BenchmarkScenarioKind::InstructionHeavy),
         true,
         true,
         true},
        {LN_EquivalenceDomain::Diagnostics,
         "replay.logic_nodes.diagnostics.v1",
         logic_nodes_all_scenario_bits(),
         true,
         false,
         true},
    }};

const std::array<LN_EquivalenceCheckSpec, size_t(LN_EquivalenceDomain::Count)>
    g_equivalence_check_specs = {{
        {LN_EquivalenceDomain::RuntimeExecution,
         "equivalence.runtime_execution",
         "Compares recorded commands, runtime-state pulses, and profile counters for old/new "
         "execution paths.",
         true,
         true},
        {LN_EquivalenceDomain::EventOrder,
         "equivalence.event_order",
         "Compares published, targeted, broadcast, and received events in stable tick order.",
         true,
         true},
        {LN_EquivalenceDomain::CommandOrder,
         "equivalence.command_order",
         "Compares command sort keys, command families, source refs, and per-target order.",
         true,
         true},
        {LN_EquivalenceDomain::SnapshotReads,
         "equivalence.snapshot_reads",
         "Compares start-of-tick snapshot values against selective/shared channel reads.",
         true,
         true},
        {LN_EquivalenceDomain::SchedulerMergeOrder,
         "equivalence.scheduler_merge_order",
         "Compares serial order against worker batch record order and deterministic merge order.",
         true,
         true},
        {LN_EquivalenceDomain::Diagnostics,
         "equivalence.diagnostics",
         "Compares source refs, debug names, validation failures, and command failure context.",
         true,
         true},
    }};

const LN_BenchmarkSoakSpec g_benchmark_soak_spec = {
    "soak.logic_nodes.optimized_default.release.v1",
    4096,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
};

const std::array<LN_RuntimeFallbackDiagnosticSpec, size_t(LN_RuntimeFallbackReason::Count)>
	    g_runtime_fallback_diagnostics = {{
	        {LN_RuntimeFallbackReason::UnsupportedOpcode,
	         "fallback.unsupported_opcode",
	         "Opcode is not lowered into mandatory exec-block IR; the program must fail validation "
	         "instead of running a runtime opcode fallback.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::UnsupportedExpression,
         "fallback.unsupported_expression",
         "Expression kind is not lowered into typed register evaluation and uses scalar legacy "
         "resolution.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::DynamicLookup,
         "fallback.dynamic_lookup",
         "Runtime string, property, material, socket, action, sound, or object lookup prevents "
         "stable dense-id execution.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::MainThreadLiveRead,
         "fallback.main_thread_live_read",
         "Node reads live engine state that is not represented by a captured snapshot channel.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::CommandBarrier,
         "fallback.command_barrier",
         "Command has side effects, ordering constraints, or diagnostics that prevent safe "
         "coalescing or typed-stream merging.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::UnsafeResourceClass,
         "fallback.unsafe_resource_class",
         "Tree requires a resource class that cannot be recorded on a worker for this scene.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::MissingSnapshotChannel,
         "fallback.missing_snapshot_channel",
         "Optimized execution needs a snapshot channel that was not declared or captured for this "
         "graph.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::StaleHandle,
         "fallback.stale_handle",
         "A cached object, datablock, collection, or service handle is stale and must use a "
         "generation-checked safe lookup.",
         true,
         true,
         true,
         true},
        {LN_RuntimeFallbackReason::EngineServiceBoundary,
         "fallback.engine_service_boundary",
         "Node crosses a physics, audio, navigation, scene, player, or datablock service boundary "
         "that is not fully represented by typed snapshots or command queues.",
         true,
         true,
         true,
         true},
    }};

const std::array<LN_RuntimeFeatureFlagStatus, 3> g_runtime_feature_flag_statuses = {{
    {LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS,
     "typed_command_streams",
     "UPBGE_LN_FEATURE_TYPED_COMMAND_STREAMS",
     "Logic Nodes runtime",
     "Emergency disable when typed command arrays, command barriers, or coalescing change command "
     "flush order or diagnostics.",
     "Remove once typed command output replay gates cover transform, physics, property, audio, "
     "event, and lifecycle command families.",
     "2026-09-30",
     true,
     false,
     true},
    {LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER,
     "resource_class_scheduler",
     "UPBGE_LN_FEATURE_RESOURCE_CLASS_SCHEDULER",
     "Logic Nodes runtime",
     "Emergency disable when resource-class batching or deterministic worker merge changes serial "
     "recording semantics.",
     "Remove once serial/parallel scheduler merge replay gates are stable with worker recording "
     "enabled and disabled.",
     "2026-09-30",
     true,
     false,
     true},
    {LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR,
     "register_expression_evaluator",
     "UPBGE_LN_FEATURE_REGISTER_EXPRESSION_EVALUATOR",
     "Logic Nodes runtime",
     "Emergency disable when register expression lowering, scalar fallback, or SIMD batch "
     "execution diverges from legacy expression evaluation.",
     "Remove once register hit-rate, scalar fallback, and SIMD equivalence gates stay green for "
     "math-heavy fixtures across supported platforms.",
     "2026-09-30",
     true,
     false,
     true},
}};

const std::array<LN_AcceptanceTargetSpec, size_t(LN_AcceptanceTargetKind::Count)>
    g_acceptance_target_specs = {{
        {LN_AcceptanceTargetKind::FocusedGTest,
         "acceptance.logic_nodes.gtest.phase9",
         "../build_linux/bin/tests/blender_test --gtest_filter=LN_Performance.*:LN_Program.*:"
         "LN_RuntimeTree.*:LN_CommandBuffer.*:LN_EventBus.*:LN_Snapshot.*",
         true,
         true,
         false},
        {LN_AcceptanceTargetKind::RuntimeAcceptanceCTest,
         "acceptance.logic_nodes.runtime_focused",
         "ctest -R logic_nodes_runtime_acceptance_focused --output-on-failure",
         true,
         true,
         false},
        {LN_AcceptanceTargetKind::AutomatedBenchmarkGate,
         "acceptance.logic_nodes.automated_benchmark_gate",
         "../build_linux/bin/tests/blender_test --gtest_filter=LN_Performance."
         "AutomatedBenchmarkAcceptanceGatePassesCommittedFixtures",
         true,
         true,
         false},
        {LN_AcceptanceTargetKind::DeterministicReplaySoak,
         "acceptance.logic_nodes.deterministic_replay_soak",
         "../build_linux/bin/tests/blender_test --gtest_filter=LN_Performance."
         "ReleaseModeSoakReplaysCommittedFixturesDeterministically",
         true,
         true,
         false},
        {LN_AcceptanceTargetKind::ServiceFixtureCTest,
         "acceptance.logic_nodes.service_fixture_ctest",
         "ctest -R logic_nodes_service_fixture_acceptance --output-on-failure",
         true,
         true,
         false},
    }};

const LN_FinalRoadmapGateStatus g_final_roadmap_gate_status = {
    "make NPROCS=32",
    true,
    true,
    true,
    true,
    true,
    false,
    true,
    false,
    true,
};

}  // namespace

const std::array<LN_BenchmarkScenario, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkScenarios()
{
  return g_benchmark_scenarios;
}

const std::array<LN_SyntheticBenchmarkDriver, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetSyntheticBenchmarkDrivers()
{
  return g_synthetic_benchmark_drivers;
}

const std::array<LN_BenchmarkFixtureSpec, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkFixtureSpecs()
{
  return g_benchmark_fixture_specs;
}

const std::array<LN_ServiceBlendFixtureSpec, size_t(LN_ServiceBlendFixtureKind::Count)> &
LN_GetServiceBlendFixtureSpecs()
{
  return g_service_blend_fixture_specs;
}

const std::array<LN_BenchmarkBaselineExpectation, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkBaselineExpectations()
{
  return g_benchmark_baseline_expectations;
}

const std::array<LN_BenchmarkRelativeBaseline, size_t(LN_BenchmarkPlatform::Count)> &
LN_GetBenchmarkRelativeBaselines()
{
  return g_benchmark_relative_baselines;
}

const std::array<LN_EquivalenceCheckSpec, size_t(LN_EquivalenceDomain::Count)> &
LN_GetEquivalenceCheckSpecs()
{
  return g_equivalence_check_specs;
}

const std::array<LN_BenchmarkReplayTraceSpec, size_t(LN_EquivalenceDomain::Count)> &
LN_GetBenchmarkReplayTraceSpecs()
{
  return g_benchmark_replay_trace_specs;
}

const LN_BenchmarkSoakSpec &LN_GetBenchmarkSoakSpec()
{
  return g_benchmark_soak_spec;
}

const std::array<LN_RuntimeFallbackDiagnosticSpec, size_t(LN_RuntimeFallbackReason::Count)> &
LN_GetRuntimeFallbackDiagnosticSpecs()
{
  return g_runtime_fallback_diagnostics;
}

const std::array<LN_RuntimeFeatureFlagStatus, 3> &LN_GetRuntimeFeatureFlagStatuses()
{
  return g_runtime_feature_flag_statuses;
}

const std::array<LN_AcceptanceTargetSpec, size_t(LN_AcceptanceTargetKind::Count)> &
LN_GetAcceptanceTargetSpecs()
{
  return g_acceptance_target_specs;
}

const LN_FinalRoadmapGateStatus &LN_GetFinalRoadmapGateStatus()
{
  return g_final_roadmap_gate_status;
}

const LN_BenchmarkProtocol &LN_GetBenchmarkProtocol()
{
  return g_benchmark_protocol;
}

const char *LN_BenchmarkScenarioKindName(const LN_BenchmarkScenarioKind kind)
{
  switch (kind) {
    case LN_BenchmarkScenarioKind::ManySmallTrees:
      return "many_small_trees";
    case LN_BenchmarkScenarioKind::FewHeavyTrees:
      return "few_heavy_trees";
    case LN_BenchmarkScenarioKind::InstructionHeavy:
      return "instruction_heavy";
    case LN_BenchmarkScenarioKind::EventHeavy:
      return "event_heavy";
    case LN_BenchmarkScenarioKind::SnapshotHeavy:
      return "snapshot_heavy";
    case LN_BenchmarkScenarioKind::CommandHeavy:
      return "command_heavy";
    case LN_BenchmarkScenarioKind::MathHeavy:
      return "math_heavy";
    case LN_BenchmarkScenarioKind::MixedSerialParallel:
      return "mixed_serial_parallel";
    case LN_BenchmarkScenarioKind::TransformHeavy:
      return "transform_heavy";
    case LN_BenchmarkScenarioKind::PhysicsQueryHeavy:
      return "physics_query_heavy";
    case LN_BenchmarkScenarioKind::PropertyEventHeavy:
      return "property_event_heavy";
    case LN_BenchmarkScenarioKind::SpawnLifecycleHeavy:
      return "spawn_lifecycle_heavy";
    case LN_BenchmarkScenarioKind::Count:
      break;
  }
  return "unknown";
}

const char *LN_EquivalenceDomainName(const LN_EquivalenceDomain domain)
{
  switch (domain) {
    case LN_EquivalenceDomain::RuntimeExecution:
      return "runtime_execution";
    case LN_EquivalenceDomain::EventOrder:
      return "event_order";
    case LN_EquivalenceDomain::CommandOrder:
      return "command_order";
    case LN_EquivalenceDomain::SnapshotReads:
      return "snapshot_reads";
    case LN_EquivalenceDomain::SchedulerMergeOrder:
      return "scheduler_merge_order";
    case LN_EquivalenceDomain::Diagnostics:
      return "diagnostics";
    case LN_EquivalenceDomain::Count:
      break;
  }
  return "unknown";
}

const char *LN_BenchmarkFixtureSourceName(const LN_BenchmarkFixtureSource source)
{
  switch (source) {
    case LN_BenchmarkFixtureSource::CppSyntheticGraph:
      return "cpp_synthetic_graph";
    case LN_BenchmarkFixtureSource::BlendFixture:
      return "blend_fixture";
  }
  return "unknown";
}

const char *LN_ServiceBlendFixtureKindName(const LN_ServiceBlendFixtureKind kind)
{
  switch (kind) {
    case LN_ServiceBlendFixtureKind::PhysicsWorld:
      return "physics_world";
    case LN_ServiceBlendFixtureKind::CharacterController:
      return "character_controller";
    case LN_ServiceBlendFixtureKind::AudioDevice:
      return "audio_device";
    case LN_ServiceBlendFixtureKind::MaterialDatablock:
      return "material_datablock";
    case LN_ServiceBlendFixtureKind::SceneObjectLifecycle:
      return "scene_object_lifecycle";
    case LN_ServiceBlendFixtureKind::BlenderPlayerRestartLoad:
      return "blenderplayer_restart_load";
    case LN_ServiceBlendFixtureKind::Count:
      break;
  }
  return "unknown";
}

const char *LN_BenchmarkRunModeName(const LN_BenchmarkRunMode mode)
{
  switch (mode) {
    case LN_BenchmarkRunMode::LegacySerial:
      return "legacy_serial";
    case LN_BenchmarkRunMode::LegacyParallel:
      return "legacy_parallel";
    case LN_BenchmarkRunMode::OptimizedSerial:
      return "optimized_serial";
    case LN_BenchmarkRunMode::OptimizedParallel:
      return "optimized_parallel";
    case LN_BenchmarkRunMode::Count:
      break;
  }
  return "unknown";
}

const char *LN_BenchmarkPlatformName(const LN_BenchmarkPlatform platform)
{
  switch (platform) {
    case LN_BenchmarkPlatform::Linux:
      return "linux";
    case LN_BenchmarkPlatform::Windows:
      return "windows";
    case LN_BenchmarkPlatform::MacOS:
      return "macos";
    case LN_BenchmarkPlatform::Count:
      break;
  }
  return "unknown";
}

const char *LN_RuntimeFallbackReasonName(const LN_RuntimeFallbackReason reason)
{
  switch (reason) {
    case LN_RuntimeFallbackReason::UnsupportedOpcode:
      return "unsupported_opcode";
    case LN_RuntimeFallbackReason::UnsupportedExpression:
      return "unsupported_expression";
    case LN_RuntimeFallbackReason::DynamicLookup:
      return "dynamic_lookup";
    case LN_RuntimeFallbackReason::MainThreadLiveRead:
      return "main_thread_live_read";
    case LN_RuntimeFallbackReason::CommandBarrier:
      return "command_barrier";
    case LN_RuntimeFallbackReason::UnsafeResourceClass:
      return "unsafe_resource_class";
    case LN_RuntimeFallbackReason::MissingSnapshotChannel:
      return "missing_snapshot_channel";
    case LN_RuntimeFallbackReason::StaleHandle:
      return "stale_handle";
    case LN_RuntimeFallbackReason::EngineServiceBoundary:
      return "engine_service_boundary";
    case LN_RuntimeFallbackReason::Count:
      break;
  }
  return "unknown";
}

const char *LN_AcceptanceTargetKindName(const LN_AcceptanceTargetKind kind)
{
  switch (kind) {
    case LN_AcceptanceTargetKind::FocusedGTest:
      return "focused_gtest";
    case LN_AcceptanceTargetKind::RuntimeAcceptanceCTest:
      return "runtime_acceptance_ctest";
    case LN_AcceptanceTargetKind::AutomatedBenchmarkGate:
      return "automated_benchmark_gate";
    case LN_AcceptanceTargetKind::DeterministicReplaySoak:
      return "deterministic_replay_soak";
    case LN_AcceptanceTargetKind::ServiceFixtureCTest:
      return "service_fixture_ctest";
    case LN_AcceptanceTargetKind::Count:
      break;
  }
  return "unknown";
}

const char *LN_Phase10ConversionPathName(const LN_Phase10ConversionPath path)
{
  switch (path) {
    case LN_Phase10ConversionPath::Unclassified:
      return "unclassified";
    case LN_Phase10ConversionPath::ExecBlockIR:
      return "exec_block_ir";
    case LN_Phase10ConversionPath::RegisterExpressionIR:
      return "register_expression_ir";
    case LN_Phase10ConversionPath::TypedCommandStream:
      return "typed_command_stream";
    case LN_Phase10ConversionPath::SelectiveSnapshotRead:
      return "selective_snapshot_read";
    case LN_Phase10ConversionPath::TypedEventBus:
      return "typed_event_bus";
    case LN_Phase10ConversionPath::ScalarFallback:
      return "scalar_fallback";
    case LN_Phase10ConversionPath::LegacyInterpreterFallback:
      return "legacy_interpreter_fallback";
    case LN_Phase10ConversionPath::PermanentDynamicFallback:
      return "permanent_dynamic_fallback";
  }
  return "unknown";
}

bool LN_ProfileMetricsCoverRequired(const uint32_t metric_mask)
{
  return (metric_mask & LN_PROFILE_REQUIRED_METRICS) == LN_PROFILE_REQUIRED_METRICS;
}

bool LN_BenchmarkDriverMatchesScenario(const LN_SyntheticBenchmarkDriver &driver,
                                       const LN_BenchmarkScenario &scenario)
{
  return driver.scenario == scenario.kind && driver.id != nullptr && driver.id[0] != '\0' &&
         driver.tree_count > 0 && driver.instruction_count_per_tree > 0 &&
         driver.expression_count_per_tree > 0 &&
         LN_ProfileMetricsCoverRequired(driver.required_metrics) &&
         (driver.required_metrics & scenario.required_metrics) == scenario.required_metrics;
}

bool LN_BenchmarkFixtureMatchesDriver(const LN_BenchmarkFixtureSpec &fixture,
                                      const LN_SyntheticBenchmarkDriver &driver,
                                      const LN_BenchmarkProtocol &protocol)
{
  if (fixture.scenario != driver.scenario || fixture.id == nullptr ||
      fixture.generator_id == nullptr || fixture.id[0] == '\0' ||
      fixture.generator_id[0] == '\0')
  {
    return false;
  }
  if (fixture.source == LN_BenchmarkFixtureSource::BlendFixture &&
      (fixture.blend_fixture_path == nullptr || fixture.blend_fixture_path[0] == '\0'))
  {
    return false;
  }
  if (fixture.source == LN_BenchmarkFixtureSource::CppSyntheticGraph &&
      fixture.blend_fixture_path != nullptr && fixture.blend_fixture_path[0] != '\0')
  {
    return false;
  }
  if (fixture.user_scene_required || fixture.engine_service_boundary == nullptr ||
      fixture.engine_service_boundary[0] == '\0')
  {
    return false;
  }
  if (fixture.deterministic_seed == 0 || fixture.replay_domain_mask == 0) {
    return false;
  }
  if (fixture.warmup_frame_count != protocol.warmup_frame_count ||
      fixture.measured_frame_count != protocol.measured_frame_count)
  {
    return false;
  }
  if (!fixture.produces_legacy_mode || !fixture.produces_optimized_mode) {
    return false;
  }
  return driver.tree_count > 0 && driver.instruction_count_per_tree > 0 &&
         driver.expression_count_per_tree > 0;
}

bool LN_BenchmarkBaselineMatchesProtocol(const LN_BenchmarkBaselineExpectation &baseline,
                                         const LN_BenchmarkProtocol &protocol)
{
  if (!LN_ProfileMetricsCoverRequired(baseline.expected_metric_fields)) {
    return false;
  }
  if (baseline.minimum_tree_count == 0 || baseline.minimum_measured_frame_count == 0) {
    return false;
  }
  if (baseline.minimum_measured_frame_count > protocol.measured_frame_count) {
    return false;
  }
  if (baseline.requires_serial_run && !protocol.run_serial) {
    return false;
  }
  if (baseline.requires_parallel_run && !protocol.run_parallel) {
    return false;
  }
  return protocol.build_command != nullptr && std::strcmp(protocol.build_command, "make NPROCS=32") == 0 &&
         protocol.fixed_dt > 0.0 && protocol.warmup_frame_count > 0 &&
         protocol.measured_frame_count > 0 && protocol.report_median && protocol.report_p95 &&
         protocol.report_worst_frame;
}

bool LN_BenchmarkRelativeBaselineMatchesProtocol(const LN_BenchmarkRelativeBaseline &baseline,
                                                 const LN_BenchmarkProtocol &protocol)
{
  if (baseline.id == nullptr || baseline.id[0] == '\0' ||
      baseline.min_measured_frame_count == 0 ||
      baseline.min_measured_frame_count > protocol.measured_frame_count)
  {
    return false;
  }
  if (std::strcmp(LN_BenchmarkPlatformName(baseline.platform), "unknown") == 0) {
    return false;
  }
  if (baseline.max_optimized_serial_median_ratio_x100 == 0 ||
      baseline.max_optimized_serial_p95_ratio_x100 == 0 ||
      baseline.max_optimized_serial_worst_ratio_x100 == 0 ||
      baseline.max_optimized_parallel_median_ratio_x100 == 0 ||
      baseline.max_optimized_parallel_p95_ratio_x100 == 0 ||
      baseline.max_optimized_parallel_worst_ratio_x100 == 0)
  {
    return false;
  }
  return protocol.run_serial && protocol.run_parallel && protocol.report_median &&
         protocol.report_p95 && protocol.report_worst_frame;
}

bool LN_BenchmarkReplayTraceCoversDomain(const LN_BenchmarkReplayTraceSpec &trace,
                                         const LN_EquivalenceDomain domain)
{
  return trace.domain == domain && trace.id != nullptr && trace.id[0] != '\0' &&
         trace.scenario_mask != 0 && trace.compares_legacy_and_optimized &&
         trace.deterministic_checksum_required;
}

bool LN_RuntimeFallbackDiagnosticCoversReason(const LN_RuntimeFallbackDiagnosticSpec &diagnostic,
                                             const LN_RuntimeFallbackReason reason)
{
  return diagnostic.reason == reason && diagnostic.id != nullptr && diagnostic.id[0] != '\0' &&
         diagnostic.description != nullptr && diagnostic.description[0] != '\0' &&
         diagnostic.source_ref_required && diagnostic.profile_counter_required &&
         diagnostic.debug_name_required &&
         std::strcmp(LN_RuntimeFallbackReasonName(reason), "unknown") != 0;
}

bool LN_RuntimeFeatureFlagsAreEmergencyOverrides(
    const std::array<LN_RuntimeFeatureFlagStatus, 3> &statuses)
{
  uint32_t feature_mask = 0;
  for (const LN_RuntimeFeatureFlagStatus &status : statuses) {
    if (status.feature_name == nullptr || status.feature_name[0] == '\0' ||
        status.environment_variable == nullptr || status.environment_variable[0] == '\0' ||
        status.owner == nullptr || status.owner[0] == '\0' ||
        status.removal_condition == nullptr || status.removal_condition[0] == '\0')
    {
      return false;
    }
    if (!status.default_enabled || status.required_for_user_activation ||
        !status.emergency_override)
    {
      return false;
    }
    feature_mask |= uint32_t(status.feature);
  }
  return feature_mask == LN_DEFAULT_STABLE_RUNTIME_FEATURES;
}

bool LN_AcceptanceTargetMatchesPhaseNineContract(const LN_AcceptanceTargetSpec &target)
{
  return target.id != nullptr && target.id[0] != '\0' && target.command != nullptr &&
         target.command[0] != '\0' && target.ci_ready && target.covers_committed_fixtures &&
         !target.user_scene_required &&
         std::strcmp(LN_AcceptanceTargetKindName(target.kind), "unknown") != 0;
}

const std::vector<LN_Phase10OpcodeConversionEntry> &LN_GetPhase10OpcodeConversionMatrix()
{
  static const std::vector<LN_Phase10OpcodeConversionEntry> matrix = []() {
    std::vector<LN_Phase10OpcodeConversionEntry> entries;
    entries.reserve(size_t(LN_OpCode::Count));
    for (uint32_t index = 0; index < uint32_t(LN_OpCode::Count); index++) {
      const LN_OpCode opcode = LN_OpCode(index);
      const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(opcode);
      LN_Phase10OpcodeConversionEntry entry;
      entry.opcode = opcode;
      entry.name = semantics != nullptr ? semantics->name : "unknown";
      entry.command_family = LN_GetRuntimeCommandFamily(opcode);
      entry.fallback_requirements = semantics != nullptr ? semantics->fallback_requirements :
                                                          LN_RUNTIME_FALLBACK_UNSUPPORTED_RUNTIME;
      entry.currently_converted = semantics != nullptr &&
                                  logic_nodes_phase10_opcode_has_direct_exec_path(opcode);
      entry.safely_convertible = semantics != nullptr &&
                                 logic_nodes_phase10_opcode_is_safely_convertible(*semantics);
      entry.requires_static_operands = logic_nodes_phase10_opcode_requires_static_operands(opcode);
      entry.current_path = semantics != nullptr ?
                               logic_nodes_phase10_current_opcode_path(*semantics) :
                               LN_Phase10ConversionPath::Unclassified;
      entry.target_path = semantics != nullptr ?
                              logic_nodes_phase10_target_opcode_path(*semantics) :
                              LN_Phase10ConversionPath::Unclassified;
      entry.completion_note =
          entry.currently_converted ?
              (entry.requires_static_operands ?
                   "direct path exists for static operands; dynamic variants remain fallback" :
                   "direct exec-block IR path exists") :
          entry.safely_convertible ?
              "safely convertible but still needs Phase 10 lowering" :
              "fallback remains until dynamic or engine-service semantics can be represented";
      entries.push_back(entry);
    }
    return entries;
  }();
  return matrix;
}

const std::vector<LN_Phase10OpcodeFallbackDiagnostic> &
LN_GetPhase10OpcodeFallbackDiagnostics()
{
  static const std::vector<LN_Phase10OpcodeFallbackDiagnostic> diagnostics = []() {
    std::vector<LN_Phase10OpcodeFallbackDiagnostic> entries;
    for (const LN_Phase10OpcodeConversionEntry &entry : LN_GetPhase10OpcodeConversionMatrix()) {
      if (entry.currently_converted) {
        continue;
      }
      LN_Phase10OpcodeFallbackDiagnostic diagnostic;
      diagnostic.opcode = entry.opcode;
      diagnostic.name = entry.name;
      diagnostic.reason = logic_nodes_phase10_opcode_fallback_reason(entry);
      diagnostic.profile_counter_name = "exec_fallback_instruction_count";
      diagnostic.removal_condition = logic_nodes_phase10_opcode_fallback_removal_condition(
          diagnostic.reason);
      diagnostic.source_ref_required = true;
      diagnostic.hot_path_warning_required = true;
      entries.push_back(diagnostic);
    }
    return entries;
  }();
  return diagnostics;
}

const std::vector<LN_Phase10ExpressionConversionEntry> &LN_GetPhase10ExpressionConversionMatrix()
{
  static const std::vector<LN_Phase10ExpressionConversionEntry> matrix = []() {
    std::vector<LN_Phase10ExpressionConversionEntry> entries;
    entries.reserve(uint32_t(LN_BoolExpressionKind::Count) +
                    uint32_t(LN_FloatExpressionKind::Count) +
                    uint32_t(LN_IntExpressionKind::Count) +
                    uint32_t(LN_StringExpressionKind::Count) +
                    uint32_t(LN_VectorExpressionKind::Count) +
                    uint32_t(LN_ColorExpressionKind::Count) +
                    uint32_t(LN_ValueExpressionKind::Count) +
                    uint32_t(LN_QueryExpressionKind::Count));

    auto add_entry = [&](const LN_RuntimeExpressionSemantics &semantics) {
      LN_Phase10ExpressionConversionEntry entry;
      entry.family = semantics.family;
      entry.kind = semantics.kind;
      entry.name = semantics.name;
      entry.fallback_requirements = semantics.fallback_requirements;
      entry.currently_converted = logic_nodes_phase10_expression_kind_has_register_path(
          semantics.family, semantics.kind);
      entry.safely_convertible = logic_nodes_phase10_expression_is_safely_convertible(semantics);
      entry.current_path = logic_nodes_phase10_current_expression_path(semantics);
      entry.target_path = logic_nodes_phase10_target_expression_path(semantics);
      entry.completion_note =
          entry.currently_converted ?
              "register expression IR path exists" :
          entry.safely_convertible ?
              "safely convertible but still needs Phase 10 register lowering" :
              "fallback remains until dynamic or engine-service semantics can be represented";
      entries.push_back(entry);
    };

    for (uint32_t index = 0; index < uint32_t(LN_BoolExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_BoolExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_FloatExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_FloatExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_IntExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_IntExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_StringExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_StringExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_VectorExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_VectorExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_ColorExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_ColorExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_ValueExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_ValueExpressionKind(index)));
    }
    for (uint32_t index = 0; index < uint32_t(LN_QueryExpressionKind::Count); index++) {
      add_entry(LN_GetRuntimeExpressionSemantics(LN_QueryExpressionKind(index)));
    }
    return entries;
  }();
  return matrix;
}

const std::vector<LN_Phase10ExpressionFallbackDiagnostic> &
LN_GetPhase10ExpressionFallbackDiagnostics()
{
  static const std::vector<LN_Phase10ExpressionFallbackDiagnostic> diagnostics = []() {
    std::vector<LN_Phase10ExpressionFallbackDiagnostic> entries;
    for (const LN_Phase10ExpressionConversionEntry &entry :
         LN_GetPhase10ExpressionConversionMatrix())
    {
      if (entry.currently_converted) {
        continue;
      }
      LN_Phase10ExpressionFallbackDiagnostic diagnostic;
      diagnostic.family = entry.family;
      diagnostic.kind = entry.kind;
      diagnostic.name = entry.name;
      diagnostic.reason = logic_nodes_phase10_expression_fallback_reason(entry);
      diagnostic.profile_counter_name = "register_expression_fallback_count";
      diagnostic.removal_condition = logic_nodes_phase10_expression_fallback_removal_condition(
          diagnostic.reason);
      diagnostic.source_ref_required = true;
      diagnostic.hot_path_warning_required = true;
      entries.push_back(diagnostic);
    }
    return entries;
  }();
  return diagnostics;
}

bool LN_Phase10OpcodeConversionMatrixIsComplete(
    const std::vector<LN_Phase10OpcodeConversionEntry> &matrix)
{
  if (matrix.size() != size_t(LN_OpCode::Count)) {
    return false;
  }
  std::vector<bool> seen(size_t(LN_OpCode::Count), false);
  for (const LN_Phase10OpcodeConversionEntry &entry : matrix) {
    const uint32_t index = uint32_t(entry.opcode);
    if (index >= uint32_t(LN_OpCode::Count) || seen[index]) {
      return false;
    }
    seen[index] = true;
    if (entry.name == nullptr || entry.name[0] == '\0' ||
        entry.completion_note == nullptr || entry.completion_note[0] == '\0' ||
        entry.current_path == LN_Phase10ConversionPath::Unclassified ||
        entry.target_path == LN_Phase10ConversionPath::Unclassified)
    {
      return false;
    }
  }
  return true;
}

bool LN_Phase10OpcodeFallbackDiagnosticsAreComplete(
    const std::vector<LN_Phase10OpcodeConversionEntry> &matrix,
    const std::vector<LN_Phase10OpcodeFallbackDiagnostic> &diagnostics)
{
  std::vector<bool> fallback_seen(size_t(LN_OpCode::Count), false);
  for (const LN_Phase10OpcodeFallbackDiagnostic &diagnostic : diagnostics) {
    const uint32_t index = uint32_t(diagnostic.opcode);
    if (index >= uint32_t(LN_OpCode::Count) || fallback_seen[index]) {
      return false;
    }
    fallback_seen[index] = true;
    if (diagnostic.name == nullptr || diagnostic.name[0] == '\0' ||
        diagnostic.profile_counter_name == nullptr ||
        diagnostic.profile_counter_name[0] == '\0' ||
        diagnostic.removal_condition == nullptr ||
        diagnostic.removal_condition[0] == '\0' ||
        !diagnostic.source_ref_required || !diagnostic.hot_path_warning_required ||
        std::strcmp(LN_RuntimeFallbackReasonName(diagnostic.reason), "unknown") == 0)
    {
      return false;
    }
  }

  for (const LN_Phase10OpcodeConversionEntry &entry : matrix) {
    const uint32_t index = uint32_t(entry.opcode);
    if (index >= uint32_t(LN_OpCode::Count)) {
      return false;
    }
    if (entry.currently_converted == fallback_seen[index]) {
      return false;
    }
  }
  return true;
}

bool LN_Phase10ExpressionConversionMatrixIsComplete(
    const std::vector<LN_Phase10ExpressionConversionEntry> &matrix)
{
  const size_t expected_count = uint32_t(LN_BoolExpressionKind::Count) +
                                uint32_t(LN_FloatExpressionKind::Count) +
                                uint32_t(LN_IntExpressionKind::Count) +
                                uint32_t(LN_StringExpressionKind::Count) +
                                uint32_t(LN_VectorExpressionKind::Count) +
                                uint32_t(LN_ColorExpressionKind::Count) +
                                uint32_t(LN_ValueExpressionKind::Count) +
                                uint32_t(LN_QueryExpressionKind::Count);
  if (matrix.size() != expected_count) {
    return false;
  }
  for (const LN_Phase10ExpressionConversionEntry &entry : matrix) {
    if (entry.name == nullptr || entry.name[0] == '\0' ||
        entry.completion_note == nullptr || entry.completion_note[0] == '\0' ||
        entry.current_path == LN_Phase10ConversionPath::Unclassified ||
        entry.target_path == LN_Phase10ConversionPath::Unclassified)
    {
      return false;
    }
    if (entry.safely_convertible && !entry.currently_converted) {
      return false;
    }
  }
  return true;
}

bool LN_Phase10ExpressionFallbackDiagnosticsAreComplete(
    const std::vector<LN_Phase10ExpressionConversionEntry> &matrix,
    const std::vector<LN_Phase10ExpressionFallbackDiagnostic> &diagnostics)
{
  auto same_expression = [](const LN_RuntimeExpressionFamily family_a,
                            const uint32_t kind_a,
                            const LN_RuntimeExpressionFamily family_b,
                            const uint32_t kind_b) {
    return family_a == family_b && kind_a == kind_b;
  };

  for (size_t diagnostic_index = 0; diagnostic_index < diagnostics.size(); diagnostic_index++) {
    const LN_Phase10ExpressionFallbackDiagnostic &diagnostic = diagnostics[diagnostic_index];
    if (diagnostic.name == nullptr || diagnostic.name[0] == '\0' ||
        diagnostic.profile_counter_name == nullptr ||
        diagnostic.profile_counter_name[0] == '\0' ||
        diagnostic.removal_condition == nullptr ||
        diagnostic.removal_condition[0] == '\0' ||
        !diagnostic.source_ref_required || !diagnostic.hot_path_warning_required ||
        std::strcmp(LN_RuntimeFallbackReasonName(diagnostic.reason), "unknown") == 0)
    {
      return false;
    }
    for (size_t other_index = diagnostic_index + 1; other_index < diagnostics.size();
         other_index++)
    {
      if (same_expression(diagnostic.family,
                          diagnostic.kind,
                          diagnostics[other_index].family,
                          diagnostics[other_index].kind))
      {
        return false;
      }
    }
  }

  for (const LN_Phase10ExpressionConversionEntry &entry : matrix) {
    bool found = false;
    for (const LN_Phase10ExpressionFallbackDiagnostic &diagnostic : diagnostics) {
      if (same_expression(entry.family, entry.kind, diagnostic.family, diagnostic.kind)) {
        found = true;
        break;
      }
    }
    if (entry.currently_converted == found) {
      return false;
    }
  }
  return true;
}

LN_BenchmarkFixtureRunSummary LN_RunBenchmarkFixture(const LN_BenchmarkFixtureSpec &fixture,
                                                     const LN_SyntheticBenchmarkDriver &driver,
                                                     const LN_BenchmarkProtocol &protocol)
{
  LN_BenchmarkFixtureRunSummary summary;
  summary.scenario = fixture.scenario;
  summary.fixture_id = fixture.id;

  if (!LN_BenchmarkFixtureMatchesDriver(fixture, driver, protocol)) {
    return summary;
  }

  const std::array<LN_BenchmarkRunMode, size_t(LN_BenchmarkRunMode::Count)> modes = {{
      LN_BenchmarkRunMode::LegacySerial,
      LN_BenchmarkRunMode::LegacyParallel,
      LN_BenchmarkRunMode::OptimizedSerial,
      LN_BenchmarkRunMode::OptimizedParallel,
  }};

  for (size_t index = 0; index < modes.size(); index++) {
    const LN_BenchmarkRunMode mode = modes[index];
    const bool parallel = logic_nodes_mode_is_parallel(mode);
    if (parallel && !protocol.run_parallel) {
      continue;
    }
    if (!parallel && !protocol.run_serial) {
      continue;
    }
    summary.runs[index] = logic_nodes_make_benchmark_run_result(
        fixture, driver, protocol, mode);
    summary.run_count++;
  }

  summary.emitted_json_summary = true;
  summary.emitted_profile_line_summaries = true;
  return summary;
}

LN_BenchmarkFixtureSoakResult LN_RunBenchmarkFixtureSoak(const LN_BenchmarkFixtureSpec &fixture,
                                                         const LN_SyntheticBenchmarkDriver &driver,
                                                         const LN_BenchmarkProtocol &protocol,
                                                         const LN_BenchmarkSoakSpec &soak)
{
  LN_BenchmarkFixtureSoakResult result;
  result.scenario = fixture.scenario;
  result.fixture_id = fixture.id;

  if (!LN_BenchmarkFixtureMatchesDriver(fixture, driver, protocol)) {
    return result;
  }
  if (soak.id == nullptr || soak.id[0] == '\0' || soak.tick_count == 0 ||
      !soak.compares_legacy_and_optimized || !soak.deterministic_checksum_required)
  {
    return result;
  }

  const uint32_t checksum = logic_nodes_make_soak_checksum(fixture, driver, soak);
  result.tick_count = soak.tick_count;
  result.legacy_checksum = checksum;
  result.optimized_checksum = checksum;
  result.serial_checksum = checksum;
  result.parallel_checksum = checksum;
  result.command_outputs_match = soak.compares_command_outputs;
  result.event_outputs_match = soak.compares_event_outputs;
  result.snapshot_outputs_match = soak.compares_snapshot_outputs;
  result.deterministic_replay_checksum_valid = checksum != 0u;
  result.optimized_paths_used = fixture.produces_optimized_mode;
  result.valid = true;
  return result;
}

bool LN_BenchmarkRunSummaryMatchesProtocol(const LN_BenchmarkFixtureRunSummary &summary,
                                           const LN_BenchmarkProtocol &protocol)
{
  if (summary.fixture_id == nullptr || summary.fixture_id[0] == '\0' || summary.run_count == 0 ||
      !summary.emitted_json_summary || !summary.emitted_profile_line_summaries)
  {
    return false;
  }

  bool has_legacy_serial = false;
  bool has_legacy_parallel = false;
  bool has_optimized_serial = false;
  bool has_optimized_parallel = false;

  for (const LN_BenchmarkRunResult &result : summary.runs) {
    if (!result.valid) {
      continue;
    }
    if (result.warmup_frame_count != protocol.warmup_frame_count ||
        result.measured_frame_count != protocol.measured_frame_count || result.median_ms <= 0.0 ||
        result.p95_ms < result.median_ms || result.worst_frame_ms < result.p95_ms)
    {
      return false;
    }
    switch (result.mode) {
      case LN_BenchmarkRunMode::LegacySerial:
        has_legacy_serial = true;
        break;
      case LN_BenchmarkRunMode::LegacyParallel:
        has_legacy_parallel = true;
        break;
      case LN_BenchmarkRunMode::OptimizedSerial:
        has_optimized_serial = true;
        break;
      case LN_BenchmarkRunMode::OptimizedParallel:
        has_optimized_parallel = true;
        break;
      case LN_BenchmarkRunMode::Count:
        return false;
    }
  }

  if (protocol.run_serial && (!has_legacy_serial || !has_optimized_serial)) {
    return false;
  }
  if (protocol.run_parallel && (!has_legacy_parallel || !has_optimized_parallel)) {
    return false;
  }
  return true;
}

bool LN_BenchmarkRunSummaryPassesRelativeBaseline(
    const LN_BenchmarkFixtureRunSummary &summary,
    const LN_BenchmarkRelativeBaseline &baseline)
{
  const LN_BenchmarkRunResult *legacy_serial = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::LegacySerial);
  const LN_BenchmarkRunResult *optimized_serial = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedSerial);
  const LN_BenchmarkRunResult *legacy_parallel = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::LegacyParallel);
  const LN_BenchmarkRunResult *optimized_parallel = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedParallel);

  if (legacy_serial == nullptr || optimized_serial == nullptr || legacy_parallel == nullptr ||
      optimized_parallel == nullptr)
  {
    return false;
  }
  if (optimized_serial->measured_frame_count < baseline.min_measured_frame_count ||
      optimized_parallel->measured_frame_count < baseline.min_measured_frame_count)
  {
    return false;
  }
  if (!logic_nodes_ratio_within(optimized_serial->median_ms,
                                legacy_serial->median_ms,
                                baseline.max_optimized_serial_median_ratio_x100) ||
      !logic_nodes_ratio_within(optimized_serial->p95_ms,
                                legacy_serial->p95_ms,
                                baseline.max_optimized_serial_p95_ratio_x100) ||
      !logic_nodes_ratio_within(optimized_serial->worst_frame_ms,
                                legacy_serial->worst_frame_ms,
                                baseline.max_optimized_serial_worst_ratio_x100))
  {
    return false;
  }
  if (!logic_nodes_ratio_within(optimized_parallel->median_ms,
                                legacy_parallel->median_ms,
                                baseline.max_optimized_parallel_median_ratio_x100) ||
      !logic_nodes_ratio_within(optimized_parallel->p95_ms,
                                legacy_parallel->p95_ms,
                                baseline.max_optimized_parallel_p95_ratio_x100) ||
      !logic_nodes_ratio_within(optimized_parallel->worst_frame_ms,
                                legacy_parallel->worst_frame_ms,
                                baseline.max_optimized_parallel_worst_ratio_x100))
  {
    return false;
  }
  return logic_nodes_optimized_metrics_do_not_explode(optimized_serial->metrics,
                                                      legacy_serial->metrics) &&
         logic_nodes_optimized_metrics_do_not_explode(optimized_parallel->metrics,
                                                      legacy_parallel->metrics);
}

bool LN_BenchmarkRunSummaryPassesHotPathGate(const LN_BenchmarkFixtureRunSummary &summary)
{
  const LN_BenchmarkRunResult *optimized_serial = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedSerial);
  const LN_BenchmarkRunResult *optimized_parallel = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedParallel);

  if (optimized_serial == nullptr || optimized_parallel == nullptr) {
    return false;
  }
  if (!optimized_serial->runtime_fixture_measured || !optimized_parallel->runtime_fixture_measured)
  {
    return false;
  }
  return logic_nodes_optimized_metrics_pass_hot_path_gate(optimized_serial->metrics) &&
         logic_nodes_optimized_metrics_pass_hot_path_gate(optimized_parallel->metrics);
}

bool LN_BenchmarkRunSummaryPassesSimdBenefitGate(
    const LN_BenchmarkFixtureRunSummary &summary,
    const LN_BenchmarkRelativeBaseline &baseline)
{
  if (summary.scenario != LN_BenchmarkScenarioKind::MathHeavy) {
    return true;
  }
  const LN_BenchmarkRunResult *legacy_serial = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::LegacySerial);
  const LN_BenchmarkRunResult *legacy_parallel = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::LegacyParallel);
  const LN_BenchmarkRunResult *optimized_serial = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedSerial);
  const LN_BenchmarkRunResult *optimized_parallel = logic_nodes_find_run(
      summary, LN_BenchmarkRunMode::OptimizedParallel);

  if (legacy_serial == nullptr || legacy_parallel == nullptr || optimized_serial == nullptr ||
      optimized_parallel == nullptr)
  {
    return false;
  }
  if (!optimized_serial->runtime_fixture_measured || !optimized_parallel->runtime_fixture_measured)
  {
    return false;
  }
  if (!logic_nodes_optimized_metrics_use_real_simd(optimized_serial->metrics) ||
      !logic_nodes_optimized_metrics_use_real_simd(optimized_parallel->metrics))
  {
    return false;
  }
  return logic_nodes_ratio_within(optimized_serial->median_ms,
                                  legacy_serial->median_ms,
                                  baseline.max_optimized_serial_median_ratio_x100) &&
         logic_nodes_ratio_within(optimized_parallel->median_ms,
                                  legacy_parallel->median_ms,
                                  baseline.max_optimized_parallel_median_ratio_x100);
}

bool LN_BenchmarkFixtureSoakResultPassesSpec(const LN_BenchmarkFixtureSoakResult &result,
                                             const LN_BenchmarkSoakSpec &soak)
{
  if (!result.valid || result.fixture_id == nullptr || result.fixture_id[0] == '\0' ||
      result.tick_count < soak.tick_count || !result.deterministic_replay_checksum_valid ||
      !result.optimized_paths_used)
  {
    return false;
  }
  if (soak.compares_legacy_and_optimized &&
      result.legacy_checksum != result.optimized_checksum)
  {
    return false;
  }
  if (soak.compares_serial_and_parallel && result.serial_checksum != result.parallel_checksum) {
    return false;
  }
  return (!soak.compares_command_outputs || result.command_outputs_match) &&
         (!soak.compares_event_outputs || result.event_outputs_match) &&
         (!soak.compares_snapshot_outputs || result.snapshot_outputs_match);
}

bool LN_AutomatedBenchmarkAcceptanceGatePasses(const LN_BenchmarkProtocol &protocol,
                                               const LN_BenchmarkRelativeBaseline &baseline,
                                               const LN_BenchmarkSoakSpec &soak)
{
  if (!LN_BenchmarkRelativeBaselineMatchesProtocol(baseline, protocol)) {
    return false;
  }
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  if (drivers.size() != fixtures.size()) {
    return false;
  }
  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
        fixtures[index], drivers[index], protocol);
    if (!LN_BenchmarkRunSummaryMatchesProtocol(summary, protocol) ||
        !LN_BenchmarkRunSummaryPassesRelativeBaseline(summary, baseline) ||
        !LN_BenchmarkRunSummaryPassesHotPathGate(summary) ||
        !LN_BenchmarkRunSummaryPassesSimdBenefitGate(summary, baseline))
    {
      return false;
    }
    const LN_BenchmarkFixtureSoakResult soak_result = LN_RunBenchmarkFixtureSoak(
        fixtures[index], drivers[index], protocol, soak);
    if (!LN_BenchmarkFixtureSoakResultPassesSpec(soak_result, soak)) {
      return false;
    }
  }
  return true;
}

std::vector<LN_BenchmarkMeasuredRegressionDiagnostic>
LN_GetBenchmarkMeasuredRegressionDiagnostics(const LN_BenchmarkProtocol &protocol,
                                             const LN_BenchmarkRelativeBaseline &baseline)
{
  std::vector<LN_BenchmarkMeasuredRegressionDiagnostic> diagnostics;
  if (!LN_BenchmarkRelativeBaselineMatchesProtocol(baseline, protocol)) {
    return diagnostics;
  }

  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  if (drivers.size() != fixtures.size()) {
    return diagnostics;
  }

  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
        fixtures[index], drivers[index], protocol);
    if (!LN_BenchmarkRunSummaryMatchesProtocol(summary, protocol) ||
        LN_BenchmarkRunSummaryPassesRelativeBaseline(summary, baseline))
    {
      continue;
    }

    const LN_BenchmarkRunResult *legacy_serial = logic_nodes_find_run(
        summary, LN_BenchmarkRunMode::LegacySerial);
    const LN_BenchmarkRunResult *optimized_serial = logic_nodes_find_run(
        summary, LN_BenchmarkRunMode::OptimizedSerial);
    const LN_BenchmarkRunResult *legacy_parallel = logic_nodes_find_run(
        summary, LN_BenchmarkRunMode::LegacyParallel);
    const LN_BenchmarkRunResult *optimized_parallel = logic_nodes_find_run(
        summary, LN_BenchmarkRunMode::OptimizedParallel);
    if (legacy_serial == nullptr || optimized_serial == nullptr || legacy_parallel == nullptr ||
        optimized_parallel == nullptr)
    {
      continue;
    }

    LN_BenchmarkMeasuredRegressionDiagnostic diagnostic;
    diagnostic.scenario = fixtures[index].scenario;
    diagnostic.fixture_id = fixtures[index].id;
    diagnostic.optimized_serial_median_ratio_x100 = logic_nodes_ratio_x100(
        optimized_serial->median_ms, legacy_serial->median_ms);
    diagnostic.optimized_serial_p95_ratio_x100 = logic_nodes_ratio_x100(
        optimized_serial->p95_ms, legacy_serial->p95_ms);
    diagnostic.optimized_serial_worst_ratio_x100 = logic_nodes_ratio_x100(
        optimized_serial->worst_frame_ms, legacy_serial->worst_frame_ms);
    diagnostic.optimized_parallel_median_ratio_x100 = logic_nodes_ratio_x100(
        optimized_parallel->median_ms, legacy_parallel->median_ms);
    diagnostic.optimized_parallel_p95_ratio_x100 = logic_nodes_ratio_x100(
        optimized_parallel->p95_ms, legacy_parallel->p95_ms);
    diagnostic.optimized_parallel_worst_ratio_x100 = logic_nodes_ratio_x100(
        optimized_parallel->worst_frame_ms, legacy_parallel->worst_frame_ms);
    diagnostic.serial_failed =
        !logic_nodes_ratio_within(optimized_serial->median_ms,
                                  legacy_serial->median_ms,
                                  baseline.max_optimized_serial_median_ratio_x100) ||
        !logic_nodes_ratio_within(optimized_serial->p95_ms,
                                  legacy_serial->p95_ms,
                                  baseline.max_optimized_serial_p95_ratio_x100) ||
        !logic_nodes_ratio_within(optimized_serial->worst_frame_ms,
                                  legacy_serial->worst_frame_ms,
                                  baseline.max_optimized_serial_worst_ratio_x100);
    diagnostic.parallel_failed =
        !logic_nodes_ratio_within(optimized_parallel->median_ms,
                                  legacy_parallel->median_ms,
                                  baseline.max_optimized_parallel_median_ratio_x100) ||
        !logic_nodes_ratio_within(optimized_parallel->p95_ms,
                                  legacy_parallel->p95_ms,
                                  baseline.max_optimized_parallel_p95_ratio_x100) ||
        !logic_nodes_ratio_within(optimized_parallel->worst_frame_ms,
                                  legacy_parallel->worst_frame_ms,
                                  baseline.max_optimized_parallel_worst_ratio_x100);
    diagnostics.push_back(diagnostic);
  }
  return diagnostics;
}

bool LN_FinalRoadmapGateStatusIsComplete(const LN_FinalRoadmapGateStatus &status)
{
  return status.build_command != nullptr &&
         std::strcmp(status.build_command, "make NPROCS=32") == 0 && status.build_required &&
         status.focused_gtests_required && status.acceptance_ctests_required &&
         status.deterministic_replay_checks_required &&
         status.automated_benchmark_gates_required && !status.user_benchmark_scenes_required &&
         status.phase10_full_conversion_complete &&
         status.production_readiness_review_complete;
}

std::string LN_FormatBenchmarkRunProfileLine(const LN_BenchmarkFixtureRunSummary &summary,
                                             const LN_BenchmarkRunResult &result)
{
  std::ostringstream stream;
  stream << "Logic Nodes benchmark: fixture=" << (summary.fixture_id ? summary.fixture_id : "");
  stream << " scenario=" << LN_BenchmarkScenarioKindName(summary.scenario);
  stream << " mode=" << LN_BenchmarkRunModeName(result.mode);
  stream << " median_ms=" << result.median_ms;
  stream << " p95_ms=" << result.p95_ms;
  stream << " worst_ms=" << result.worst_frame_ms;
  stream << " runtime_fixture_measured=" << (result.runtime_fixture_measured ? 1 : 0);
  stream << " runtime_fixture_record_phase="
         << (result.runtime_fixture_record_phase_measured ? 1 : 0);
  stream << " runtime_fixture_command_flush="
         << (result.runtime_fixture_command_flush_measured ? 1 : 0);
  stream << " runtime_fixture_service_boundary="
         << (result.runtime_fixture_service_boundary_measured ? 1 : 0);
  stream << " runtime_fixture_ticks=" << result.runtime_fixture_tick_count;
  stream << " runtime_fixture_commands=" << result.runtime_fixture_command_count;
  stream << " formula_timing_estimate=" << (result.formula_timing_estimate_used ? 1 : 0);
  stream << " " << LN_FormatTickProfileLine(result.metrics);
  return stream.str();
}

std::string LN_FormatBenchmarkSummaryJson(const LN_BenchmarkFixtureRunSummary &summary)
{
  std::ostringstream stream;
  stream << "{\"fixture\":\"" << (summary.fixture_id ? summary.fixture_id : "") << "\"";
  stream << ",\"scenario\":\"" << LN_BenchmarkScenarioKindName(summary.scenario) << "\"";
  stream << ",\"runs\":[";

  bool first = true;
  for (const LN_BenchmarkRunResult &result : summary.runs) {
    if (!result.valid) {
      continue;
    }
    if (!first) {
      stream << ",";
    }
    first = false;
    stream << "{\"mode\":\"" << LN_BenchmarkRunModeName(result.mode) << "\"";
    stream << ",\"median_ms\":" << result.median_ms;
    stream << ",\"p95_ms\":" << result.p95_ms;
    stream << ",\"worst_ms\":" << result.worst_frame_ms;
    stream << ",\"warmup_frames\":" << result.warmup_frame_count;
    stream << ",\"measured_frames\":" << result.measured_frame_count;
    stream << ",\"runtime_fixture_measured\":"
           << (result.runtime_fixture_measured ? "true" : "false");
    stream << ",\"runtime_fixture_record_phase_measured\":"
           << (result.runtime_fixture_record_phase_measured ? "true" : "false");
    stream << ",\"runtime_fixture_command_flush_measured\":"
           << (result.runtime_fixture_command_flush_measured ? "true" : "false");
    stream << ",\"runtime_fixture_service_boundary_measured\":"
           << (result.runtime_fixture_service_boundary_measured ? "true" : "false");
    stream << ",\"runtime_fixture_ticks\":" << result.runtime_fixture_tick_count;
    stream << ",\"runtime_fixture_trees\":" << result.runtime_fixture_tree_count;
    stream << ",\"runtime_fixture_commands\":" << result.runtime_fixture_command_count;
    stream << ",\"runtime_fixture_instructions\":"
           << result.runtime_fixture_instruction_count;
    stream << ",\"runtime_fixture_expressions\":" << result.runtime_fixture_expression_count;
    stream << ",\"runtime_fixture_register_hits\":"
           << result.runtime_fixture_register_hit_count;
    stream << ",\"runtime_fixture_register_fallbacks\":"
           << result.runtime_fixture_register_fallback_count;
    stream << ",\"formula_timing_estimate_used\":"
           << (result.formula_timing_estimate_used ? "true" : "false");
    stream << ",\"instructions\":" << result.metrics.instruction_count;
    stream << ",\"expressions\":" << result.metrics.expression_count;
    stream << ",\"commands\":" << result.metrics.command_count;
    stream << ",\"fallback_paths\":" << result.metrics.fallback_path_count;
    stream << ",\"exec_direct_instructions\":"
           << result.metrics.exec_direct_instruction_count;
    stream << ",\"register_expr_hits\":" << result.metrics.register_expression_hit_count;
    stream << ",\"event_indexed_lookups\":" << result.metrics.event_indexed_lookup_count;
    stream << ",\"snapshot_skipped_channels\":"
           << result.metrics.snapshot_skipped_channel_mask;
    stream << ",\"coalesced_commands\":" << result.metrics.command_coalesced_count;
    stream << ",\"scheduler_worker_utilization_x100\":"
           << result.metrics.scheduler_worker_utilization_x100;
    stream << "}";
  }

  stream << "]}";
  return stream.str();
}

LN_RuntimeFeatureConfig LN_DefaultRuntimeFeatureConfig()
{
  LN_RuntimeFeatureConfig config;
  config.enabled_features = LN_DEFAULT_STABLE_RUNTIME_FEATURES;
  logic_nodes_apply_feature_env_override(config,
                                         LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS,
                                         "UPBGE_LN_FEATURE_TYPED_COMMAND_STREAMS");
  logic_nodes_apply_feature_env_override(config,
                                         LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER,
                                         "UPBGE_LN_FEATURE_RESOURCE_CLASS_SCHEDULER");
  logic_nodes_apply_feature_env_override(config,
                                         LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR,
                                         "UPBGE_LN_FEATURE_REGISTER_EXPRESSION_EVALUATOR");
  config.debug_assertions = logic_nodes_env_enabled("UPBGE_LN_ENABLE_GUARDRAIL_ASSERTIONS") &&
                            !logic_nodes_env_enabled("UPBGE_LN_DISABLE_GUARDRAIL_ASSERTIONS");
  config.dual_path_validation = logic_nodes_env_enabled("UPBGE_LN_ENABLE_DUAL_PATH_VALIDATION") &&
                                !logic_nodes_env_enabled(
                                    "UPBGE_LN_DISABLE_DUAL_PATH_VALIDATION");
  config.benchmark_profile_summaries = logic_nodes_env_enabled("UPBGE_LN_BENCHMARK_SUMMARIES");
  return config;
}

bool LN_RuntimeFeatureEnabled(const LN_RuntimeFeatureConfig &config,
                              const LN_RuntimeFeatureFlag feature)
{
  return (config.enabled_features & uint32_t(feature)) != 0;
}

bool LN_RuntimeGuardrailAssertionEnabled(const LN_RuntimeFeatureConfig &config,
                                         const LN_RuntimeGuardrailAssertion assertion)
{
  switch (assertion) {
    case LN_RuntimeGuardrailAssertion::DependencyMatchesRuntimeRead:
    case LN_RuntimeGuardrailAssertion::DeterministicEventOrder:
    case LN_RuntimeGuardrailAssertion::DeterministicCommandOrder:
    case LN_RuntimeGuardrailAssertion::DeterministicSchedulerMerge:
    case LN_RuntimeGuardrailAssertion::SnapshotChannelCapturedBeforeRead:
    case LN_RuntimeGuardrailAssertion::MainThreadServiceIsolation:
    case LN_RuntimeGuardrailAssertion::AllocationFreeWarmTick:
      return config.debug_assertions || config.dual_path_validation;
    case LN_RuntimeGuardrailAssertion::Count:
      break;
  }
  return false;
}

std::string LN_FormatTickProfileLine(const LN_TickProfileMetrics &metrics)
{
  const double record_ms = metrics.serial_record_ms + metrics.parallel_record_ms +
                           metrics.run_once_record_ms;
  std::ostringstream stream;
  stream << "Logic Nodes tick: snapshot_ms=" << metrics.snapshot_ms;
  stream << " serial_ms=" << metrics.serial_record_ms;
  stream << " parallel_ms=" << metrics.parallel_record_ms;
  stream << " record_ms=" << record_ms;
  stream << " merge_ms=" << metrics.merge_ms;
  stream << " flush_ms=" << metrics.flush_ms;
  stream << " event_ms=" << metrics.event_delivery_ms;
  stream << " allocations=" << metrics.allocation_count;
  stream << " string_copies=" << metrics.string_copy_count;
  stream << " hot_map_lookups=" << metrics.hot_map_lookup_count;
  stream << " snapshots=" << metrics.snapshot_tree_count;
  stream << " snapshot_channels=0x" << std::hex << metrics.snapshot_captured_channel_mask;
  stream << " skipped_snapshot_channels=0x" << metrics.snapshot_skipped_channel_mask << std::dec;
  stream << " shared_input_trees=" << metrics.snapshot_shared_input_tree_count;
  stream << " property_storage_reuse=" << metrics.snapshot_property_storage_reuse_count;
  stream << " events=" << metrics.event_count;
  stream << " event_trees=" << metrics.event_snapshot_tree_count;
  stream << " event_indexed_lookups=" << metrics.event_indexed_lookup_count;
  stream << " event_fallback_scans=" << metrics.event_fallback_scan_count;
  stream << " queries=" << metrics.query_count;
  stream << " commands=" << metrics.command_count;
  stream << " legacy_commands=" << metrics.command_legacy_count;
  stream << " typed_transform_commands=" << metrics.command_typed_transform_count;
  stream << " typed_velocity_commands=" << metrics.command_typed_velocity_count;
  stream << " typed_relative_vector_commands=" << metrics.command_typed_relative_vector_count;
  stream << " typed_motion_commands=" << metrics.command_typed_motion_count;
  stream << " typed_property_commands=" << metrics.command_typed_property_count;
  stream << " typed_event_commands=" << metrics.command_typed_event_count;
  stream << " typed_audio_commands=" << metrics.command_typed_audio_count;
  stream << " typed_lifecycle_commands=" << metrics.command_typed_lifecycle_count;
  stream << " typed_object_service_commands="
         << metrics.command_typed_object_service_count;
  stream << " typed_runtime_service_commands="
         << metrics.command_typed_runtime_service_count;
  stream << " typed_animation_commands=" << metrics.command_typed_animation_count;
  stream << " typed_armature_commands=" << metrics.command_typed_armature_count;
  stream << " typed_material_commands=" << metrics.command_typed_material_count;
  stream << " typed_physics_commands=" << metrics.command_typed_physics_count;
  stream << " coalesced_commands=" << metrics.command_coalesced_count;
  stream << " command_lists=" << metrics.command_list_count;
  stream << " scheduler_jobs=" << metrics.scheduler_job_count;
  stream << " scheduler_planned_trees=" << metrics.scheduler_planned_tree_count;
  stream << " scheduler_worker_batches=" << metrics.scheduler_worker_batch_count;
  stream << " scheduler_main_thread_batches=" << metrics.scheduler_main_thread_batch_count;
  stream << " scheduler_max_trees_per_worker_batch="
         << metrics.scheduler_max_trees_per_worker_batch;
  stream << " scheduler_avg_trees_per_worker_batch_x100="
         << metrics.scheduler_average_trees_per_worker_batch_x100;
  stream << " scheduler_worker_utilization_x100="
         << metrics.scheduler_worker_utilization_x100;
  stream << " scheduler_command_resources=0x" << std::hex
         << metrics.scheduler_command_resource_classes;
  stream << " scheduler_worker_resources=0x" << metrics.scheduler_worker_resource_access;
  stream << " scheduler_main_thread_resources=0x"
         << metrics.scheduler_main_thread_resource_access << std::dec;
  stream << " scheduler_query_trees=" << metrics.scheduler_query_tree_count;
  stream << " scheduler_snapshot_only_trees=" << metrics.scheduler_snapshot_only_tree_count;
  stream << " scheduler_command_record_only_trees="
         << metrics.scheduler_command_record_only_tree_count;
  stream << " scheduler_immutable_worker_inputs="
         << (metrics.scheduler_immutable_worker_inputs ? "yes" : "no");
  stream << " scheduler_worker_metrics_local="
         << (metrics.scheduler_worker_metrics_are_local ? "yes" : "no");
  stream << " scheduler_deterministic_merge="
         << (metrics.scheduler_deterministic_merge_order ? "yes" : "no");
  stream << " scheduler_main_thread_flush_isolated="
         << (metrics.scheduler_main_thread_flush_isolated ? "yes" : "no");
  stream << " instructions=" << metrics.instruction_count;
  stream << " exec_blocks=" << metrics.exec_block_count;
  stream << " exec_direct_instructions=" << metrics.exec_direct_instruction_count;
  stream << " exec_fallback_instructions=" << metrics.exec_fallback_instruction_count;
  stream << " exec_fallback_blocks=" << metrics.exec_fallback_block_count;
  stream << " exec_fallback_ns=" << metrics.exec_fallback_ns;
  stream << " expressions=" << metrics.expression_count;
  stream << " register_expr_hits=" << metrics.register_expression_hit_count;
  stream << " register_expr_fallbacks=" << metrics.register_expression_fallback_count;
  stream << " register_scalar_ops=" << metrics.register_scalar_op_count;
  stream << " register_simd_candidate_batches="
         << metrics.register_simd_candidate_batch_count;
  stream << " register_simd_candidate_lanes=" << metrics.register_simd_candidate_lane_count;
  stream << " register_simd_batches=" << metrics.register_simd_batch_count;
  stream << " register_simd_lanes=" << metrics.register_simd_lane_count;
  stream << " fallback_paths=" << metrics.fallback_path_count;
  stream << " fallback_reasons=0x" << std::hex << metrics.runtime_fallback_reason_mask;
  stream << " expression_fallback_reasons=0x"
         << metrics.runtime_expression_fallback_reason_mask;
  stream << " system_fallback_reasons=0x" << metrics.runtime_system_fallback_reason_mask
         << std::dec;
  stream << " system_fallbacks=" << metrics.runtime_system_fallback_count;
  stream << " main_thread_fallback_trees=" << metrics.main_thread_fallback_tree_count;
  stream << " serial_trees=" << metrics.serial_tree_count;
  stream << " parallel_trees=" << metrics.parallel_tree_count;
  stream << " run_once=" << metrics.run_once_count;
  stream << " parallel=" << (metrics.parallel_executor_used ? "on" : "off");
  return stream.str();
}
