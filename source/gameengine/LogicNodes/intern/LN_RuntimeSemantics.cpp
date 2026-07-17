/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeSemantics.cpp
 *  \ingroup logicnodes
 */

#include "LN_RuntimeSemantics.h"

#include <algorithm>
#include <iterator>

namespace {

using Threading = LN_RuntimeSemanticThreading;
using Timing = LN_RuntimeSemanticTiming;
using Ordering = LN_RuntimeSemanticOrdering;
using Coalescing = LN_RuntimeSemanticCoalescing;

constexpr uint32_t READ_COMMAND_INPUTS = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE;
constexpr uint32_t READ_INPUT_COMMAND_INPUTS = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                               LN_RUNTIME_SEMANTIC_READ_INPUT;
constexpr uint32_t READ_EXPRESSION_INPUTS = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE;
constexpr uint32_t READ_SNAPSHOT_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                              LN_RUNTIME_SEMANTIC_READ_SNAPSHOT;
constexpr uint32_t READ_INPUT_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                           LN_RUNTIME_SEMANTIC_READ_INPUT;
constexpr uint32_t READ_EVENT_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                           LN_RUNTIME_SEMANTIC_READ_EVENT_BUS;
constexpr uint32_t READ_QUERY_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                           LN_RUNTIME_SEMANTIC_READ_SNAPSHOT |
                                           LN_RUNTIME_SEMANTIC_READ_QUERY_CACHE;
constexpr uint32_t READ_RUNTIME_STATE_EXPRESSION =
    LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE | LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE;
constexpr uint32_t READ_SCENE_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                           LN_RUNTIME_SEMANTIC_READ_SCENE;
constexpr uint32_t READ_GLOBAL_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                            LN_RUNTIME_SEMANTIC_READ_GLOBAL_STATE;
constexpr uint32_t READ_FILE_EXPRESSION = LN_RUNTIME_SEMANTIC_READ_PROGRAM_TABLE |
                                          LN_RUNTIME_SEMANTIC_READ_FILE;

constexpr uint32_t WRITE_COMMAND_OBJECT = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                          LN_RUNTIME_SEMANTIC_WRITE_OBJECT_STATE;
constexpr uint32_t WRITE_COMMAND_SCENE = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                         LN_RUNTIME_SEMANTIC_WRITE_SCENE;
constexpr uint32_t WRITE_COMMAND_AUDIO = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                         LN_RUNTIME_SEMANTIC_WRITE_AUDIO;
constexpr uint32_t WRITE_COMMAND_EVENT = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                         LN_RUNTIME_SEMANTIC_WRITE_EVENT_BUS;
constexpr uint32_t WRITE_COMMAND_FILE = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                        LN_RUNTIME_SEMANTIC_WRITE_FILE;
constexpr uint32_t WRITE_COMMAND_GLOBAL = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                          LN_RUNTIME_SEMANTIC_WRITE_GLOBAL_STATE;
constexpr uint32_t WRITE_COMMAND_RENDER = LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                                          LN_RUNTIME_SEMANTIC_WRITE_RENDER;
constexpr uint32_t WRITE_DIRECT_OBJECT = LN_RUNTIME_SEMANTIC_WRITE_OBJECT_STATE;
constexpr uint32_t WRITE_DIRECT_SCENE = LN_RUNTIME_SEMANTIC_WRITE_SCENE;
constexpr uint32_t WRITE_DIRECT_FILE = LN_RUNTIME_SEMANTIC_WRITE_FILE;
constexpr uint32_t WRITE_DIRECT_GLOBAL = LN_RUNTIME_SEMANTIC_WRITE_GLOBAL_STATE;
constexpr uint32_t WRITE_DIRECT_RENDER = LN_RUNTIME_SEMANTIC_WRITE_RENDER;

#define LN_SEM_WITH_FALLBACK(OP, READS, WRITES, TIME, THREAD, ORDER, COALESCE, FALLBACK) \
  {LN_OpCode::OP, #OP, READS, WRITES, TIME, THREAD, ORDER, COALESCE, FALLBACK}

#define LN_SEM(OP, READS, WRITES, TIME, THREAD, ORDER, COALESCE) \
  LN_SEM_WITH_FALLBACK(OP, READS, WRITES, TIME, THREAD, ORDER, COALESCE, \
                       LN_RUNTIME_FALLBACK_NONE)

#define LN_WORKER_COMMAND(OP, WRITES, ORDER) \
  LN_SEM(OP, READ_COMMAND_INPUTS, WRITES, Timing::DeferredCommandFlush, \
         Threading::WorkerSafeRecord, ORDER, Coalescing::Forbidden)

#define LN_WORKER_COMMAND_WITH_FALLBACK(OP, WRITES, ORDER, FALLBACK) \
  LN_SEM_WITH_FALLBACK(OP, READ_COMMAND_INPUTS, WRITES, Timing::DeferredCommandFlush, \
                       Threading::WorkerSafeRecord, ORDER, Coalescing::Forbidden, FALLBACK)

#define LN_MAIN_COMMAND(OP, WRITES, ORDER) \
  LN_SEM(OP, READ_COMMAND_INPUTS, WRITES, Timing::DeferredCommandFlush, \
         Threading::MainThreadRecord, ORDER, Coalescing::Forbidden)

#define LN_MAIN_DIRECT(OP, WRITES, ORDER) \
  LN_SEM(OP, READ_COMMAND_INPUTS, WRITES, Timing::ImmediateMainThreadRecord, \
         Threading::MainThreadRecord, ORDER, Coalescing::Forbidden)

#define LN_LAST_WRITE_COMMAND(OP, WRITES) \
  LN_SEM(OP, READ_COMMAND_INPUTS, WRITES, Timing::DeferredCommandFlush, \
         Threading::WorkerSafeRecord, Ordering::ObservablePerTarget, Coalescing::LastWriteWins)

static constexpr LN_RuntimeInstructionSemantics semantics_catalog[] = {
    LN_SEM(Nop,
           LN_RUNTIME_SEMANTIC_READ_NONE,
           LN_RUNTIME_SEMANTIC_WRITE_NONE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::NotObservable,
           Coalescing::Forbidden),

    LN_LAST_WRITE_COMMAND(SetWorldPosition, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLocalPosition, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetWorldOrientation, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLocalOrientation, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetWorldScale, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLocalScale, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLinearVelocity, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLocalLinearVelocity, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetAngularVelocity, WRITE_COMMAND_OBJECT),
    LN_LAST_WRITE_COMMAND(SetLocalAngularVelocity, WRITE_COMMAND_OBJECT),
    LN_WORKER_COMMAND(ApplyImpulse, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVisibility, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetObjectColor, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(MakeLightUnique, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetLightColor, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetLightPower, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetLightShadow, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ApplyMovement, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ApplyRotation, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ApplyForce, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_SEM(ApplyForceToTarget,
           READ_COMMAND_INPUTS | LN_RUNTIME_SEMANTIC_READ_SCENE,
           WRITE_COMMAND_OBJECT,
           Timing::DeferredCommandFlush,
           Threading::MainThreadRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_WORKER_COMMAND(ApplyTorque, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetGameProperty, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_MAIN_DIRECT(AddObject, WRITE_DIRECT_SCENE, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(SetParent, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(RemoveParent, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(RemoveObject, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetGravity, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetTimeScale, WRITE_COMMAND_GLOBAL, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetActiveCamera, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetCameraFov, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCameraOrthoScale, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetWindowSize, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetFullscreen, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetVSync, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetShowFramerate, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetShowProfile, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetCollisionGroup, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetPhysics, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetDynamics, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(RebuildCollisionShape, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetRigidBodyAttribute, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetTreeProperty,
                      LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                          LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCursorVisibility, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetCursorPosition, WRITE_COMMAND_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetGamepadVibration, WRITE_COMMAND_GLOBAL, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(GamepadLook, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_SEM(MouseLook,
           READ_INPUT_COMMAND_INPUTS,
           WRITE_COMMAND_OBJECT | LN_RUNTIME_SEMANTIC_WRITE_RENDER,
           Timing::DeferredCommandFlush,
           Threading::MainThreadRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_WORKER_COMMAND(CharacterJump, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCharacterGravity, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCharacterJumpSpeed, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCharacterMaxJumps, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCharacterWalkDirection,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCharacterVelocity, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(VehicleControl, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(VehicleApplyEngineForce,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(VehicleApplyBraking, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(VehicleApplySteering, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVehicleSuspensionCompression,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVehicleSuspensionStiffness,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVehicleSuspensionDamping,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVehicleWheelFriction,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(Print, WRITE_COMMAND_GLOBAL, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(QuitGame, WRITE_COMMAND_GLOBAL, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(RestartGame, WRITE_COMMAND_GLOBAL, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(LoadBlendFile, WRITE_COMMAND_FILE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetLogicTreeEnabled,
                      LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                          LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                      Ordering::ObservablePerTarget),
    LN_MAIN_DIRECT(RunLogicTreeOnce,
                   LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                   Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(InstallLogicTree,
                      LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM |
                          LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(PlayAction, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(StopAction, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetActionFrame, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(StopAllSounds, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(PlaySound, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(PlaySound3D, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(PauseSound, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(ResumeSound, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(StopSound, WRITE_COMMAND_AUDIO, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND_WITH_FALLBACK(SendEvent,
                                    WRITE_COMMAND_EVENT,
                                    Ordering::ObservableGlobal,
                                    LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP),
    LN_MAIN_DIRECT(SetGlobalProperty, WRITE_DIRECT_GLOBAL, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(SaveVariable, WRITE_DIRECT_FILE, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(SaveVariableDict, WRITE_DIRECT_FILE, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(ClearVariables, WRITE_DIRECT_FILE, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(RemoveVariable, WRITE_DIRECT_FILE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(Translate, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_SEM_WITH_FALLBACK(Navigate,
                         READ_COMMAND_INPUTS,
                         WRITE_COMMAND_RENDER,
                         Timing::DeferredCommandFlush,
                         Threading::MainThreadRecord,
                         Ordering::ObservablePerTarget,
                         Coalescing::Forbidden,
                         LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY),
    LN_SEM_WITH_FALLBACK(FollowPath,
                         READ_COMMAND_INPUTS,
                         WRITE_COMMAND_OBJECT,
                         Timing::DeferredCommandFlush,
                         Threading::MainThreadRecord,
                         Ordering::ObservablePerTarget,
                         Coalescing::Forbidden,
                         LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY),
    LN_MAIN_DIRECT(SetCollectionVisibility, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(SetOverlayCollection, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(RemoveOverlayCollection, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(MoveToward, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SlowFollow, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(LoadScene, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetScene, WRITE_COMMAND_SCENE, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(SaveGame, WRITE_COMMAND_FILE, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(LoadGame, WRITE_COMMAND_FILE, Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(AlignAxisToVector, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(RotateToward, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ReplaceMesh, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(CopyProperty, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBonePoseLocation, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBonePoseRotation, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBonePoseScale, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBonePoseTransform, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBoneAttribute, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBoneConstraintInfluence,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBoneConstraintTarget,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetBoneConstraintAttribute,
                      WRITE_COMMAND_OBJECT,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetMaterialSlot, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetMaterialParameter, WRITE_COMMAND_RENDER, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetMaterialNodeSocketValue,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetGeometryNodesInput,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetGeometryNodeSocketValue,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetCompositorNodeSocketValue,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(MakeNodeTreeUnique,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetNodeMute,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(EnableDisableModifier,
                      WRITE_COMMAND_RENDER,
                      Ordering::ObservablePerTarget),
    LN_MAIN_DIRECT(AssignGeometryNodesModifier,
                   WRITE_DIRECT_RENDER | LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                   Ordering::ObservablePerTarget),
    LN_MAIN_DIRECT(DrawLine, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(DrawArrow, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(DrawPath, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(DrawBox, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(DrawMesh, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(DrawAxis, WRITE_DIRECT_RENDER, Ordering::ObservableGlobal),
    LN_MAIN_COMMAND(AddPhysicsConstraint, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_MAIN_COMMAND(RemovePhysicsConstraint, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_MAIN_DIRECT(SpawnPoolCreate,
                   WRITE_DIRECT_SCENE | WRITE_DIRECT_OBJECT,
                   Ordering::ObservableGlobal),
    LN_MAIN_DIRECT(SpawnPoolSpawn,
                   WRITE_DIRECT_SCENE | WRITE_DIRECT_OBJECT,
                   Ordering::ObservableGlobal),
    LN_WORKER_COMMAND(SetTransformVector, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(SetVelocityVector, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ApplyTransformVector, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_WORKER_COMMAND(ApplyPhysicsVector, WRITE_COMMAND_OBJECT, Ordering::ObservablePerTarget),
    LN_SEM(ArmTimer,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(ArmDelay,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(UpdatePulsify,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(UpdateBarrier,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(TryOnce,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(ResetOnce,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(TryCooldown,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(ResetCooldown,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::RuntimeTreeState,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
    LN_SEM(BranchRoute,
           READ_COMMAND_INPUTS,
           LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
           Timing::BranchRoute,
           Threading::WorkerSafeRecord,
           Ordering::ObservablePerTarget,
           Coalescing::Forbidden),
};

constexpr LN_RuntimeExpressionSemantics Expr(const LN_RuntimeExpressionFamily family,
                                             const uint32_t kind,
                                             const char *name,
                                             const uint32_t reads,
                                             const uint32_t writes = LN_RUNTIME_SEMANTIC_WRITE_NONE,
                                             const Threading threading =
                                                 Threading::WorkerSafeRecord,
                                             const uint32_t fallback_requirements =
                                                 LN_RUNTIME_FALLBACK_NONE)
{
  return {family, kind, name, reads, writes, threading, true, fallback_requirements};
}

constexpr LN_RuntimeExpressionSemantics UnknownExpr(const LN_RuntimeExpressionFamily family,
                                                    const uint32_t kind)
{
  return {family,
          kind,
          "<invalid>",
          LN_RUNTIME_SEMANTIC_READ_NONE,
          LN_RUNTIME_SEMANTIC_WRITE_NONE,
          Threading::WorkerSafeRecord,
          false,
          LN_RUNTIME_FALLBACK_NONE};
}

#define LN_EXPR(FAMILY, KIND, READS) \
  return Expr(LN_RuntimeExpressionFamily::FAMILY, \
              uint32_t(kind), \
              #KIND, \
              READS)

#define LN_EXPR_WITH_FALLBACK(FAMILY, KIND, READS, FALLBACK) \
  return Expr(LN_RuntimeExpressionFamily::FAMILY, \
              uint32_t(kind), \
              #KIND, \
              READS, \
              LN_RUNTIME_SEMANTIC_WRITE_NONE, \
              Threading::WorkerSafeRecord, \
              FALLBACK)

#define LN_EXPR_STATE(FAMILY, KIND) \
  return Expr(LN_RuntimeExpressionFamily::FAMILY, \
              uint32_t(kind), \
              #KIND, \
              READ_RUNTIME_STATE_EXPRESSION, \
              LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE)

#define LN_EXPR_RUNTIME_READ(FAMILY, KIND) \
  return Expr(LN_RuntimeExpressionFamily::FAMILY, \
              uint32_t(kind), \
              #KIND, \
              READ_RUNTIME_STATE_EXPRESSION)

#define LN_EXPR_MAIN(FAMILY, KIND, READS) \
  return Expr(LN_RuntimeExpressionFamily::FAMILY, \
              uint32_t(kind), \
              #KIND, \
              READS, \
              LN_RUNTIME_SEMANTIC_WRITE_NONE, \
              Threading::MainThreadRecord, \
              LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ)

#undef LN_LAST_WRITE_COMMAND
#undef LN_MAIN_DIRECT
#undef LN_MAIN_COMMAND
#undef LN_WORKER_COMMAND_WITH_FALLBACK
#undef LN_WORKER_COMMAND
#undef LN_SEM
#undef LN_SEM_WITH_FALLBACK

}  // namespace

const LN_RuntimeInstructionSemantics *LN_GetRuntimeInstructionSemantics(const LN_OpCode opcode)
{
  const auto *begin = std::begin(semantics_catalog);
  const auto *end = std::end(semantics_catalog);
  const auto *iter = std::find_if(begin, end, [opcode](const LN_RuntimeInstructionSemantics &entry) {
    return entry.opcode == opcode;
  });
  return iter == end ? nullptr : iter;
}

const LN_RuntimeInstructionSemantics *LN_GetRuntimeInstructionSemanticsCatalog(size_t *r_count)
{
  if (r_count != nullptr) {
    *r_count = std::size(semantics_catalog);
  }
  return semantics_catalog;
}

bool LN_RuntimeInstructionSemanticsCatalogHasDuplicates()
{
  for (size_t i = 0; i < std::size(semantics_catalog); i++) {
    for (size_t j = i + 1; j < std::size(semantics_catalog); j++) {
      if (semantics_catalog[i].opcode == semantics_catalog[j].opcode) {
        return true;
      }
    }
  }
  return false;
}

LN_RuntimeCommandFamily LN_GetRuntimeCommandFamily(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::Nop:
      return LN_RuntimeCommandFamily::None;
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
      return LN_RuntimeCommandFamily::ObjectTransform;
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
    case LN_OpCode::ApplyImpulse:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyForceToTarget:
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
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyPhysicsVector:
      return LN_RuntimeCommandFamily::ObjectPhysics;
    case LN_OpCode::SetVisibility:
    case LN_OpCode::SetObjectColor:
    case LN_OpCode::ReplaceMesh:
    case LN_OpCode::SetMaterialSlot:
      return LN_RuntimeCommandFamily::ObjectState;
    case LN_OpCode::SetGameProperty:
    case LN_OpCode::CopyProperty:
      return LN_RuntimeCommandFamily::ObjectProperty;
    case LN_OpCode::AddObject:
    case LN_OpCode::RemoveObject:
      return LN_RuntimeCommandFamily::ObjectLifecycle;
    case LN_OpCode::SetGravity:
    case LN_OpCode::SetTimeScale:
      return LN_RuntimeCommandFamily::SceneState;
    case LN_OpCode::SetParent:
    case LN_OpCode::RemoveParent:
      return LN_RuntimeCommandFamily::Parenting;
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
      return LN_RuntimeCommandFamily::Animation;
    case LN_OpCode::StopAllSounds:
    case LN_OpCode::PlaySound:
    case LN_OpCode::PlaySound3D:
    case LN_OpCode::PauseSound:
    case LN_OpCode::ResumeSound:
    case LN_OpCode::StopSound:
      return LN_RuntimeCommandFamily::Audio;
    case LN_OpCode::SetActiveCamera:
    case LN_OpCode::SetCameraFov:
    case LN_OpCode::SetCameraOrthoScale:
      return LN_RuntimeCommandFamily::Camera;
    case LN_OpCode::MakeLightUnique:
    case LN_OpCode::SetLightColor:
    case LN_OpCode::SetLightPower:
    case LN_OpCode::SetLightShadow:
      return LN_RuntimeCommandFamily::Light;
    case LN_OpCode::SetShowFramerate:
    case LN_OpCode::SetShowProfile:
      return LN_RuntimeCommandFamily::Render;
    case LN_OpCode::SetWindowSize:
    case LN_OpCode::SetFullscreen:
    case LN_OpCode::SetVSync:
    case LN_OpCode::SetCursorVisibility:
    case LN_OpCode::SetCursorPosition:
    case LN_OpCode::SetGamepadVibration:
      return LN_RuntimeCommandFamily::Window;
    case LN_OpCode::SetTreeProperty:
    case LN_OpCode::SetLogicTreeEnabled:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::InstallLogicTree:
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
    case LN_OpCode::BranchRoute:
      return LN_RuntimeCommandFamily::TreeState;
    case LN_OpCode::QuitGame:
    case LN_OpCode::RestartGame:
    case LN_OpCode::LoadBlendFile:
    case LN_OpCode::LoadScene:
    case LN_OpCode::SetScene:
    case LN_OpCode::SaveGame:
    case LN_OpCode::LoadGame:
      return LN_RuntimeCommandFamily::GameLifecycle;
    case LN_OpCode::SendEvent:
      return LN_RuntimeCommandFamily::Events;
    case LN_OpCode::SetMaterialParameter:
    case LN_OpCode::SetMaterialNodeSocketValue:
    case LN_OpCode::SetGeometryNodesInput:
    case LN_OpCode::SetGeometryNodeSocketValue:
    case LN_OpCode::SetCompositorNodeSocketValue:
    case LN_OpCode::MakeNodeTreeUnique:
    case LN_OpCode::SetNodeMute:
    case LN_OpCode::EnableDisableModifier:
    case LN_OpCode::AssignGeometryNodesModifier:
      return LN_RuntimeCommandFamily::Datablock;
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
      return LN_RuntimeCommandFamily::Collection;
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
      return LN_RuntimeCommandFamily::Navigation;
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
      return LN_RuntimeCommandFamily::PhysicsConstraint;
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
      return LN_RuntimeCommandFamily::SpawnPool;
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
      return LN_RuntimeCommandFamily::File;
    case LN_OpCode::SetGlobalProperty:
      return LN_RuntimeCommandFamily::GlobalState;
    case LN_OpCode::Print:
      return LN_RuntimeCommandFamily::Render;
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
      return LN_RuntimeCommandFamily::DebugDraw;
    case LN_OpCode::GamepadLook:
    case LN_OpCode::MouseLook:
      return LN_RuntimeCommandFamily::ObjectTransform;
    case LN_OpCode::Count:
      break;
  }
  return LN_RuntimeCommandFamily::Unknown;
}

LN_RuntimeSideEffectDelivery LN_GetRuntimeSideEffectDelivery(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::Nop:
      return LN_RuntimeSideEffectDelivery::None;
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
    case LN_OpCode::BranchRoute:
      return LN_RuntimeSideEffectDelivery::RuntimeTreeState;
    case LN_OpCode::SetGlobalProperty:
    case LN_OpCode::SaveVariable:
    case LN_OpCode::SaveVariableDict:
    case LN_OpCode::ClearVariables:
    case LN_OpCode::RemoveVariable:
    case LN_OpCode::AddObject:
    case LN_OpCode::RunLogicTreeOnce:
    case LN_OpCode::SpawnPoolCreate:
    case LN_OpCode::SpawnPoolSpawn:
    case LN_OpCode::SetCollectionVisibility:
    case LN_OpCode::SetOverlayCollection:
    case LN_OpCode::RemoveOverlayCollection:
    case LN_OpCode::DrawLine:
    case LN_OpCode::DrawArrow:
    case LN_OpCode::DrawPath:
    case LN_OpCode::DrawBox:
    case LN_OpCode::DrawMesh:
    case LN_OpCode::DrawAxis:
    case LN_OpCode::AssignGeometryNodesModifier:
      return LN_RuntimeSideEffectDelivery::ImmediateMainThread;
    case LN_OpCode::AddPhysicsConstraint:
    case LN_OpCode::RemovePhysicsConstraint:
    case LN_OpCode::Navigate:
      return LN_RuntimeSideEffectDelivery::ImmediateAndDeferred;
    case LN_OpCode::Count:
      break;
    default:
      return LN_RuntimeSideEffectDelivery::DeferredCommandBuffer;
  }
  return LN_RuntimeSideEffectDelivery::Unknown;
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_BoolExpressionKind kind)
{
  switch (kind) {
    case LN_BoolExpressionKind::SnapshotGameProperty:
    case LN_BoolExpressionKind::SnapshotVisibility:
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
    case LN_BoolExpressionKind::SnapshotCharacterOnGround:
      LN_EXPR(Bool, SnapshotGameProperty, READ_SNAPSHOT_EXPRESSION);
    case LN_BoolExpressionKind::RuntimeTreeProperty:
    case LN_BoolExpressionKind::LogicTreeRunning:
    case LN_BoolExpressionKind::LogicTreeStopped:
    case LN_BoolExpressionKind::ActionDone:
    case LN_BoolExpressionKind::AnimationPlaying:
    case LN_BoolExpressionKind::LoopActive:
    case LN_BoolExpressionKind::StoreValueDone:
    case LN_BoolExpressionKind::InstructionExecuted:
    case LN_BoolExpressionKind::InstructionReached:
      LN_EXPR(Bool, RuntimeTreeProperty, READ_RUNTIME_STATE_EXPRESSION);
    case LN_BoolExpressionKind::OnNextTick:
    case LN_BoolExpressionKind::Once:
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
      LN_EXPR_STATE(Bool, OnNextTick);
    case LN_BoolExpressionKind::BooleanEdge:
      LN_EXPR_STATE(Bool, BooleanEdge);
    case LN_BoolExpressionKind::BooleanEdgeFalling:
      LN_EXPR_STATE(Bool, BooleanEdgeFalling);
    case LN_BoolExpressionKind::CooldownAccepted:
      LN_EXPR_RUNTIME_READ(Bool, CooldownAccepted);
    case LN_BoolExpressionKind::CooldownBlocked:
      LN_EXPR_RUNTIME_READ(Bool, CooldownBlocked);
    case LN_BoolExpressionKind::CooldownCompleted:
      LN_EXPR_RUNTIME_READ(Bool, CooldownCompleted);
    case LN_BoolExpressionKind::CooldownReady:
      LN_EXPR_RUNTIME_READ(Bool, CooldownReady);
    case LN_BoolExpressionKind::InputStatus:
    case LN_BoolExpressionKind::KeyboardActive:
    case LN_BoolExpressionKind::MouseMoved:
    case LN_BoolExpressionKind::MouseWheelMoved:
    case LN_BoolExpressionKind::WindowFullscreen:
    case LN_BoolExpressionKind::GamepadActive:
    case LN_BoolExpressionKind::GamepadButton:
    case LN_BoolExpressionKind::KeyLoggerPressed:
      LN_EXPR(Bool, InputStatus, READ_INPUT_EXPRESSION);
    case LN_BoolExpressionKind::EventReceived:
      LN_EXPR(Bool, EventReceived, READ_EVENT_EXPRESSION);
    case LN_BoolExpressionKind::PhysicsQueryDone:
    case LN_BoolExpressionKind::PhysicsQueryHit:
    case LN_BoolExpressionKind::PhysicsQueryBlocked:
    case LN_BoolExpressionKind::PhysicsQueryHasUV:
    case LN_BoolExpressionKind::PhysicsQueryStartedOverlapping:
    case LN_BoolExpressionKind::MouseOverEnter:
    case LN_BoolExpressionKind::MouseOverOver:
    case LN_BoolExpressionKind::MouseOverExit:
      LN_EXPR(Bool, PhysicsQueryHit, READ_QUERY_EXPRESSION);
    case LN_BoolExpressionKind::ObjectsColliding:
    case LN_BoolExpressionKind::CollisionDetected:
      LN_EXPR_MAIN(Bool, ObjectsColliding, READ_QUERY_EXPRESSION);
    case LN_BoolExpressionKind::RigidBodyAttribute:
      LN_EXPR_MAIN(Bool, RigidBodyAttribute, READ_SCENE_EXPRESSION);
    case LN_BoolExpressionKind::RigidBodyConstraintFound:
      return Expr(LN_RuntimeExpressionFamily::Bool,
                  uint32_t(kind),
                  "RigidBodyConstraintFound",
                  READ_SCENE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit:
      return Expr(LN_RuntimeExpressionFamily::Bool,
                  uint32_t(kind),
                  "CollisionEvent",
                  READ_QUERY_EXPRESSION | LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE,
                  LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ);
    case LN_BoolExpressionKind::Constant:
    case LN_BoolExpressionKind::Not:
    case LN_BoolExpressionKind::And:
    case LN_BoolExpressionKind::Or:
    case LN_BoolExpressionKind::FloatCompare:
    case LN_BoolExpressionKind::StringContains:
    case LN_BoolExpressionKind::StringStartsWith:
    case LN_BoolExpressionKind::StringEndsWith:
    case LN_BoolExpressionKind::ValueIsNone:
    case LN_BoolExpressionKind::DictHasKey:
    case LN_BoolExpressionKind::ListContains:
    case LN_BoolExpressionKind::ValueCompare:
      LN_EXPR(Bool, Constant, READ_EXPRESSION_INPUTS);
    case LN_BoolExpressionKind::MaterialSlotFound:
    case LN_BoolExpressionKind::MaterialNodeValueFound:
    case LN_BoolExpressionKind::EditorNodeValueFound:
      return Expr(LN_RuntimeExpressionFamily::Bool,
                  uint32_t(kind),
                  "MaterialNodeValueFound",
                  READ_EXPRESSION_INPUTS,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_BoolExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(Bool,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_BoolExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Bool, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_FloatExpressionKind kind)
{
  switch (kind) {
    case LN_FloatExpressionKind::SnapshotGameProperty:
    case LN_FloatExpressionKind::SnapshotTimeScale:
    case LN_FloatExpressionKind::SnapshotLightPower:
    case LN_FloatExpressionKind::SnapshotElapsedTime:
    case LN_FloatExpressionKind::SnapshotFrameDelta:
    case LN_FloatExpressionKind::SnapshotFPS:
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
    case LN_FloatExpressionKind::AnimationFrame:
    case LN_FloatExpressionKind::BoneLength:
      LN_EXPR(Float, SnapshotGameProperty, READ_SNAPSHOT_EXPRESSION);
    case LN_FloatExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(Float, RuntimeTreeProperty);
    case LN_FloatExpressionKind::StoreValue:
    case LN_FloatExpressionKind::TweenFactor:
    case LN_FloatExpressionKind::TweenFloatResult:
      LN_EXPR_STATE(Float, StoreValue);
    case LN_FloatExpressionKind::GamepadButtonStrength:
      LN_EXPR(Float, GamepadButtonStrength, READ_INPUT_EXPRESSION);
    case LN_FloatExpressionKind::ObjectDistance:
      return Expr(LN_RuntimeExpressionFamily::Float,
                  uint32_t(kind),
                  "ObjectDistance",
                  READ_SCENE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_FloatExpressionKind::PhysicsQueryDistance:
    case LN_FloatExpressionKind::PhysicsQueryFraction:
    case LN_FloatExpressionKind::PhysicsQueryPenetrationDepth:
      LN_EXPR(Float, PhysicsQueryDistance, READ_QUERY_EXPRESSION);
    case LN_FloatExpressionKind::RigidBodyAttribute:
      LN_EXPR_MAIN(Float, RigidBodyAttribute, READ_SCENE_EXPRESSION);
    case LN_FloatExpressionKind::CooldownRemaining:
      LN_EXPR_RUNTIME_READ(Float, CooldownRemaining);
    case LN_FloatExpressionKind::CooldownProgress:
      LN_EXPR_RUNTIME_READ(Float, CooldownProgress);
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
    case LN_FloatExpressionKind::Random:
    case LN_FloatExpressionKind::Select:
    case LN_FloatExpressionKind::Formula:
      LN_EXPR(Float, Constant, READ_EXPRESSION_INPUTS);
    case LN_FloatExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(Float,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_FloatExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Float, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_IntExpressionKind kind)
{
  switch (kind) {
    case LN_IntExpressionKind::SnapshotGameProperty:
    case LN_IntExpressionKind::SnapshotCollisionGroup:
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
    case LN_IntExpressionKind::SnapshotCharacterJumpCount:
      LN_EXPR(Int, SnapshotGameProperty, READ_SNAPSHOT_EXPRESSION);
    case LN_IntExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(Int, RuntimeTreeProperty);
    case LN_IntExpressionKind::LoopIndex:
      LN_EXPR(Int, LoopIndex, READ_RUNTIME_STATE_EXPRESSION);
    case LN_IntExpressionKind::MouseWheelDelta:
      LN_EXPR(Int, MouseWheelDelta, READ_INPUT_EXPRESSION);
    case LN_IntExpressionKind::Constant:
    case LN_IntExpressionKind::StringCount:
    case LN_IntExpressionKind::ListLength:
    case LN_IntExpressionKind::DictLength:
    case LN_IntExpressionKind::Random:
      LN_EXPR(Int, Constant, READ_EXPRESSION_INPUTS);
    case LN_IntExpressionKind::MaterialSlotCount:
      return Expr(LN_RuntimeExpressionFamily::Int,
                  uint32_t(kind),
                  "MaterialSlotCount",
                  READ_EXPRESSION_INPUTS,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_IntExpressionKind::CollisionContactCount:
      return Expr(LN_RuntimeExpressionFamily::Int,
                  uint32_t(kind),
                  "CollisionContactCount",
                  READ_QUERY_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ);
    case LN_IntExpressionKind::PhysicsQueryFaceIndex:
    case LN_IntExpressionKind::PhysicsQueryHitCount:
      return Expr(LN_RuntimeExpressionFamily::Int,
                  uint32_t(kind),
                  "PhysicsQueryInt",
                  READ_QUERY_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ);
    case LN_IntExpressionKind::WindowResolutionWidth:
      LN_EXPR(Int, WindowResolutionWidth, READ_INPUT_EXPRESSION);
    case LN_IntExpressionKind::WindowResolutionHeight:
      LN_EXPR(Int, WindowResolutionHeight, READ_INPUT_EXPRESSION);
    case LN_IntExpressionKind::WindowVSyncMode:
      LN_EXPR(Int, WindowVSyncMode, READ_INPUT_EXPRESSION);
    case LN_IntExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(Int,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_IntExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Int, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_StringExpressionKind kind)
{
  switch (kind) {
    case LN_StringExpressionKind::SnapshotGameProperty:
    case LN_StringExpressionKind::AnimationActionName:
      LN_EXPR(String, SnapshotGameProperty, READ_SNAPSHOT_EXPRESSION);
    case LN_StringExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(String, RuntimeTreeProperty);
    case LN_StringExpressionKind::KeyLoggerCharacter:
    case LN_StringExpressionKind::KeyLoggerKeycode:
      LN_EXPR(String, KeyLoggerCharacter, READ_INPUT_EXPRESSION);
    case LN_StringExpressionKind::Constant:
    case LN_StringExpressionKind::Join:
    case LN_StringExpressionKind::Replace:
    case LN_StringExpressionKind::ToUppercase:
    case LN_StringExpressionKind::ToLowercase:
    case LN_StringExpressionKind::ZeroFill:
    case LN_StringExpressionKind::Format:
    case LN_StringExpressionKind::MasterFolder:
    case LN_StringExpressionKind::ObjectID:
      LN_EXPR(String, Constant, READ_EXPRESSION_INPUTS);
    case LN_StringExpressionKind::MaterialName:
      return Expr(LN_RuntimeExpressionFamily::String,
                  uint32_t(kind),
                  "MaterialName",
                  READ_EXPRESSION_INPUTS,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_StringExpressionKind::RigidBodyConstraintName:
      return Expr(LN_RuntimeExpressionFamily::String,
                  uint32_t(kind),
                  "RigidBodyConstraintName",
                  READ_SCENE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_StringExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(String,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_StringExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::String, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_VectorExpressionKind kind)
{
  switch (kind) {
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
      LN_EXPR(Vector, SnapshotWorldPosition, READ_SNAPSHOT_EXPRESSION);
    case LN_VectorExpressionKind::WindowResolution:
      LN_EXPR(Vector, WindowResolution, READ_INPUT_EXPRESSION);
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(Vector, RuntimeTreeProperty);
    case LN_VectorExpressionKind::CursorPosition:
    case LN_VectorExpressionKind::CursorMovement:
    case LN_VectorExpressionKind::GamepadStick:
      LN_EXPR(Vector, CursorPosition, READ_INPUT_EXPRESSION);
    case LN_VectorExpressionKind::PhysicsQueryPoint:
    case LN_VectorExpressionKind::PhysicsQueryNormal:
    case LN_VectorExpressionKind::PhysicsQueryCastPosition:
    case LN_VectorExpressionKind::PhysicsQueryDirection:
    case LN_VectorExpressionKind::PhysicsQueryEndPoint:
    case LN_VectorExpressionKind::PhysicsQueryUV:
      LN_EXPR(Vector, PhysicsQueryPoint, READ_QUERY_EXPRESSION);
    case LN_VectorExpressionKind::CollisionHitPoint:
    case LN_VectorExpressionKind::CollisionHitNormal:
    case LN_VectorExpressionKind::SpawnPoolHitPoint:
    case LN_VectorExpressionKind::SpawnPoolHitNormal:
    case LN_VectorExpressionKind::SpawnPoolHitDirection:
      LN_EXPR_MAIN(Vector, CollisionHitPoint, READ_QUERY_EXPRESSION);
    case LN_VectorExpressionKind::GroupCenterPosition:
      return Expr(LN_RuntimeExpressionFamily::Vector,
                  uint32_t(kind),
                  "GroupCenterPosition",
                  READ_SCENE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_VectorExpressionKind::RigidBodyAttribute:
      LN_EXPR_MAIN(Vector, RigidBodyAttribute, READ_SCENE_EXPRESSION);
    case LN_VectorExpressionKind::InstructionNextPoint:
    case LN_VectorExpressionKind::TweenVectorResult:
    case LN_VectorExpressionKind::TweenRotationResult:
      LN_EXPR(Vector, InstructionNextPoint, READ_RUNTIME_STATE_EXPRESSION);
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
    case LN_VectorExpressionKind::RotateAroundAxis:
    case LN_VectorExpressionKind::VectorToRotation:
    case LN_VectorExpressionKind::AxisVector:
    case LN_VectorExpressionKind::Combine:
    case LN_VectorExpressionKind::EvaluateCurveAtFactor:
    case LN_VectorExpressionKind::Random:
      LN_EXPR(Vector, Constant, READ_EXPRESSION_INPUTS);
    case LN_VectorExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(Vector,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_VectorExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Vector, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_ColorExpressionKind kind)
{
  switch (kind) {
    case LN_ColorExpressionKind::SnapshotObjectColor:
    case LN_ColorExpressionKind::SnapshotLightColor:
      LN_EXPR(Color, SnapshotObjectColor, READ_SNAPSHOT_EXPRESSION);
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(Color, RuntimeTreeProperty);
    case LN_ColorExpressionKind::Constant:
    case LN_ColorExpressionKind::Combine:
      LN_EXPR(Color, Constant, READ_EXPRESSION_INPUTS);
    case LN_ColorExpressionKind::FromGenericValue:
      LN_EXPR_WITH_FALLBACK(Color,
                            FromGenericValue,
                            READ_EXPRESSION_INPUTS,
                            LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION);
    case LN_ColorExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Color, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_ValueExpressionKind kind)
{
  switch (kind) {
    case LN_ValueExpressionKind::SnapshotGameProperty:
    case LN_ValueExpressionKind::ActiveCamera:
    case LN_ValueExpressionKind::OwnerObject:
    case LN_ValueExpressionKind::ObjectParent:
    case LN_ValueExpressionKind::ObjectChild:
    case LN_ValueExpressionKind::ObjectGameProperty:
      LN_EXPR(Value, SnapshotGameProperty, READ_SNAPSHOT_EXPRESSION);
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      LN_EXPR_RUNTIME_READ(Value, RuntimeTreeProperty);
    case LN_ValueExpressionKind::ObjectChildByName:
    case LN_ValueExpressionKind::ObjectAttribute:
    case LN_ValueExpressionKind::BoneAttribute:
    case LN_ValueExpressionKind::BonePoseRotation:
      LN_EXPR_WITH_FALLBACK(Value,
                            ObjectChildByName,
                            READ_SNAPSHOT_EXPRESSION,
                            LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::ObjectByName:
      LN_EXPR_WITH_FALLBACK(Value,
                            ObjectByName,
                            READ_SCENE_EXPRESSION,
                            LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::CurrentScene:
      LN_EXPR(Value, CurrentScene, READ_SCENE_EXPRESSION);
    case LN_ValueExpressionKind::CollectionObjects:
    case LN_ValueExpressionKind::CollectionObjectNames:
      LN_EXPR_WITH_FALLBACK(Value,
                            CollectionObjects,
                            READ_SCENE_EXPRESSION,
                            LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::RigidBodyConstraintNames:
      return Expr(LN_RuntimeExpressionFamily::Value,
                  uint32_t(kind),
                  "RigidBodyConstraintNames",
                  READ_SCENE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
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
      LN_EXPR(Value, PhysicsQueryObject, READ_QUERY_EXPRESSION);
    case LN_ValueExpressionKind::CollisionHitObject:
    case LN_ValueExpressionKind::CollisionHitObjects:
    case LN_ValueExpressionKind::CollisionHitPoints:
    case LN_ValueExpressionKind::CollisionHitNormals:
    case LN_ValueExpressionKind::SpawnPoolHitObject:
      LN_EXPR_MAIN(Value, CollisionHitObject, READ_QUERY_EXPRESSION);
    case LN_ValueExpressionKind::EventContent:
    case LN_ValueExpressionKind::EventMessenger:
      LN_EXPR(Value, EventContent, READ_EVENT_EXPRESSION);
    case LN_ValueExpressionKind::LoopCurrentValue:
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew:
      LN_EXPR(Value, LoopCurrentValue, READ_RUNTIME_STATE_EXPRESSION);
    case LN_ValueExpressionKind::ListAppend:
    case LN_ValueExpressionKind::ListRemoveIndex:
    case LN_ValueExpressionKind::ListRemoveValue:
    case LN_ValueExpressionKind::ListSetIndex:
    case LN_ValueExpressionKind::DictSetKey:
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue:
      LN_EXPR_STATE(Value, ListAppend);
    case LN_ValueExpressionKind::GetGlobalProperty:
      return Expr(LN_RuntimeExpressionFamily::Value,
                  uint32_t(kind),
                  "GetGlobalProperty",
                  READ_GLOBAL_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::ListGlobalProperties:
      LN_EXPR_MAIN(Value, GetGlobalProperty, READ_GLOBAL_EXPRESSION);
    case LN_ValueExpressionKind::LoadVariable:
    case LN_ValueExpressionKind::LoadVariableDict:
      return Expr(LN_RuntimeExpressionFamily::Value,
                  uint32_t(kind),
                  "LoadVariable",
                  READ_FILE_EXPRESSION,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::ListSavedVariables:
      LN_EXPR_MAIN(Value, LoadVariable, READ_FILE_EXPRESSION);
    case LN_ValueExpressionKind::Constant:
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
    case LN_ValueExpressionKind::ListElement:
    case LN_ValueExpressionKind::MakeList:
    case LN_ValueExpressionKind::ListExtend:
    case LN_ValueExpressionKind::DictGetKey:
    case LN_ValueExpressionKind::MakeDict:
    case LN_ValueExpressionKind::DictGetKeys:
    case LN_ValueExpressionKind::EmptyList:
    case LN_ValueExpressionKind::EmptyDict:
    case LN_ValueExpressionKind::ListDuplicate:
    case LN_ValueExpressionKind::DictMerge:
    case LN_ValueExpressionKind::ListFromItems:
    case LN_ValueExpressionKind::ListRandomItem:
    case LN_ValueExpressionKind::ValueSwitchList:
    case LN_ValueExpressionKind::ValueSwitchListCompare:
      LN_EXPR(Value, Constant, READ_EXPRESSION_INPUTS);
    case LN_ValueExpressionKind::MaterialSlot:
    case LN_ValueExpressionKind::MaterialNodeValue:
    case LN_ValueExpressionKind::EditorNodeValue:
      return Expr(LN_RuntimeExpressionFamily::Value,
                  uint32_t(kind),
                  "MaterialNodeValue",
                  READ_EXPRESSION_INPUTS,
                  LN_RUNTIME_SEMANTIC_WRITE_NONE,
                  Threading::MainThreadRecord,
                  LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ |
                      LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP);
    case LN_ValueExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Value, uint32_t(kind));
}

LN_RuntimeExpressionSemantics LN_GetRuntimeExpressionSemantics(
    const LN_QueryExpressionKind kind)
{
  switch (kind) {
    case LN_QueryExpressionKind::Raycast:
    case LN_QueryExpressionKind::RaycastAll:
    case LN_QueryExpressionKind::ShapeCast:
    case LN_QueryExpressionKind::ShapeCastAll:
    case LN_QueryExpressionKind::MouseOver:
    case LN_QueryExpressionKind::MouseRay:
    case LN_QueryExpressionKind::CameraRay:
    case LN_QueryExpressionKind::ProjectileRay:
      LN_EXPR(Query, Raycast, READ_QUERY_EXPRESSION);
    case LN_QueryExpressionKind::Count:
      break;
  }
  return UnknownExpr(LN_RuntimeExpressionFamily::Query, uint32_t(kind));
}

#undef LN_EXPR_MAIN
#undef LN_EXPR_RUNTIME_READ
#undef LN_EXPR_STATE
#undef LN_EXPR_WITH_FALLBACK
#undef LN_EXPR
