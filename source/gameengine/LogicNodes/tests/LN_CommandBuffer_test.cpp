/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <chrono>
#include <deque>
#include <set>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Vector3Value.h"
#include "KX_GameObject.h"
#include "LN_CommandBuffer.h"
#include "LN_CommandDiagnostics.h"
#include "LN_Program.h"
#include "LN_RuntimeSemantics.h"
#include "LN_RuntimeTree.h"
#include "LN_TestTrees.h"

using ln_test::NamedPropertyWriter;

namespace {

KX_GameObject *FakeGameObjectPointer(uint32_t value)
{
  return reinterpret_cast<KX_GameObject *>(uintptr_t(value));
}

LN_CommandBuffer::Command GamePropertyCommand(const std::string &name,
                                             float value,
                                             uint64_t sort_key,
                                             uint32_t source_ref_index)
{
  LN_CommandBuffer::Command command;
  command.type = LN_CommandBuffer::CommandType::SetGameProperty;
  command.property_name = name;
  command.property_value.type = LN_ValueType::Float;
  command.property_value.float_value = value;
  command.sort_key = sort_key;
  command.source_ref_index = source_ref_index;
  return command;
}

struct CommandTypeSubsystemCase {
  LN_CommandBuffer::CommandType type;
  LN_CommandBuffer::CommandSubsystem subsystem;
};

struct CommandTypeRuntimeFamilyCase {
  LN_CommandBuffer::CommandType type;
  LN_OpCode opcode;
  LN_RuntimeCommandFamily family;
};

static constexpr CommandTypeSubsystemCase kCommandTypeSubsystems[] = {
    {LN_CommandBuffer::CommandType::SetWorldPosition,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetLocalPosition,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetWorldOrientation,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetLocalOrientation,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetWorldScale,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetLocalScale,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetLinearVelocity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetLocalLinearVelocity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetAngularVelocity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetLocalAngularVelocity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::ApplyImpulse,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetVisibility,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::SetObjectColor,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::MakeLightUnique,
     LN_CommandBuffer::CommandSubsystem::Light},
    {LN_CommandBuffer::CommandType::SetLightColor,
     LN_CommandBuffer::CommandSubsystem::Light},
    {LN_CommandBuffer::CommandType::SetLightPower,
     LN_CommandBuffer::CommandSubsystem::Light},
    {LN_CommandBuffer::CommandType::SetLightShadow,
     LN_CommandBuffer::CommandSubsystem::Light},
    {LN_CommandBuffer::CommandType::ApplyMovement,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::ApplyRotation,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::ApplyForce,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::ApplyTorque,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetGameProperty,
     LN_CommandBuffer::CommandSubsystem::ObjectProperty},
    {LN_CommandBuffer::CommandType::AddObject,
     LN_CommandBuffer::CommandSubsystem::ObjectLifecycle},
    {LN_CommandBuffer::CommandType::AddObjectFromRef,
     LN_CommandBuffer::CommandSubsystem::ObjectLifecycle},
    {LN_CommandBuffer::CommandType::SetParent,
     LN_CommandBuffer::CommandSubsystem::Parenting},
    {LN_CommandBuffer::CommandType::SetParentFromRef,
     LN_CommandBuffer::CommandSubsystem::Parenting},
    {LN_CommandBuffer::CommandType::RemoveParent,
     LN_CommandBuffer::CommandSubsystem::Parenting},
    {LN_CommandBuffer::CommandType::RemoveObject,
     LN_CommandBuffer::CommandSubsystem::ObjectLifecycle},
    {LN_CommandBuffer::CommandType::SetGravity,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::SetTimeScale,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::SetGamePaused,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::SetActiveCamera,
     LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetCameraFov,
     LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetCameraOrthoScale,
     LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetCollisionGroup,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetPhysics,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetPhysicsPaused,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::SetDynamics,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::RebuildCollisionShape,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetRigidBodyAttribute,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::CharacterJump,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetCharacterGravity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetCharacterJumpSpeed,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetCharacterMaxJumps,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetCharacterWalkDirection,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetCharacterVelocity,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::VehicleControl,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::VehicleApplyEngineForce,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::VehicleApplyBraking,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::VehicleApplySteering,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetVehicleSuspensionCompression,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetVehicleSuspensionStiffness,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetVehicleSuspensionDamping,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetVehicleWheelFriction,
     LN_CommandBuffer::CommandSubsystem::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetTreeProperty,
     LN_CommandBuffer::CommandSubsystem::TreeState},
    {LN_CommandBuffer::CommandType::SetCursorVisibility,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetCursorPosition,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetGamepadVibration,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetWindowSize,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetFullscreen,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetVSync,
     LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetShowFramerate,
     LN_CommandBuffer::CommandSubsystem::Render},
    {LN_CommandBuffer::CommandType::SetShowProfile,
     LN_CommandBuffer::CommandSubsystem::Render},
    {LN_CommandBuffer::CommandType::SubsystemCommand,
     LN_CommandBuffer::CommandSubsystem::Render},
    {LN_CommandBuffer::CommandType::Print,
     LN_CommandBuffer::CommandSubsystem::Render},
    {LN_CommandBuffer::CommandType::QuitGame,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::RestartGame,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::LoadBlendFile,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::PlayAction,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::StopAction,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetActionFrame,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetShapeKeyValue,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::StopAllSounds,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::PlaySound,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::PlaySound3D,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::PauseSound,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::ResumeSound,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::StopSound,
     LN_CommandBuffer::CommandSubsystem::Sound},
    {LN_CommandBuffer::CommandType::SetLogicTreeEnabled,
     LN_CommandBuffer::CommandSubsystem::TreeState},
    {LN_CommandBuffer::CommandType::InstallLogicTree,
     LN_CommandBuffer::CommandSubsystem::TreeState},
    {LN_CommandBuffer::CommandType::SendEvent,
     LN_CommandBuffer::CommandSubsystem::Events},
    {LN_CommandBuffer::CommandType::MoveToward,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SlowFollow,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetOverlayCollection,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::RemoveOverlayCollection,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::OpenObjectMenu,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::CloseObjectMenu,
     LN_CommandBuffer::CommandSubsystem::SceneState},
    {LN_CommandBuffer::CommandType::SetUIFocus,
     LN_CommandBuffer::CommandSubsystem::TreeState},
    {LN_CommandBuffer::CommandType::LoadScene,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::SetScene,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::SaveGame,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::SetSaveExistence,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::LoadGame,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::AlignAxisToVector,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::RotateToward,
     LN_CommandBuffer::CommandSubsystem::ObjectTransform},
    {LN_CommandBuffer::CommandType::ReplaceMesh,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::CopyProperty,
     LN_CommandBuffer::CommandSubsystem::ObjectProperty},
    {LN_CommandBuffer::CommandType::SetBonePoseLocation,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBonePoseRotation,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBonePoseScale,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBonePoseTransform,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBoneAttribute,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBoneConstraintInfluence,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBoneConstraintTarget,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetBoneConstraintAttribute,
     LN_CommandBuffer::CommandSubsystem::Animation},
    {LN_CommandBuffer::CommandType::SetMaterialSlot,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::SetMaterialParameter,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::SetGeometryNodesInput,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::SetGeometryNodeSocketValue,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::SetCompositorNodeSocketValue,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::MakeNodeTreeUnique,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::SetNodeMute,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::EnableDisableModifier,
     LN_CommandBuffer::CommandSubsystem::Datablock},
    {LN_CommandBuffer::CommandType::ModifyGameProperty,
     LN_CommandBuffer::CommandSubsystem::ObjectProperty},
    {LN_CommandBuffer::CommandType::SetUIInteractable,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::ResetSmoothing,
     LN_CommandBuffer::CommandSubsystem::ObjectState},
    {LN_CommandBuffer::CommandType::SetPlayerCamera, LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetSplitScreen, LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetCameraViewport,
     LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetCameraVisibility,
     LN_CommandBuffer::CommandSubsystem::Camera},
    {LN_CommandBuffer::CommandType::SetPlayerGamepad, LN_CommandBuffer::CommandSubsystem::Window},
    {LN_CommandBuffer::CommandType::SetPlayerPointer,
     LN_CommandBuffer::CommandSubsystem::TreeState},
    {LN_CommandBuffer::CommandType::TakeUIControl, LN_CommandBuffer::CommandSubsystem::TreeState},
};

static_assert(sizeof(kCommandTypeSubsystems) / sizeof(kCommandTypeSubsystems[0]) ==
                  static_cast<size_t>(LN_CommandBuffer::CommandType::TakeUIControl) + 1,
              "Update command subsystem coverage when adding CommandType values");

static constexpr CommandTypeRuntimeFamilyCase kCommandRuntimeFamilies[] = {
    {LN_CommandBuffer::CommandType::SetWorldPosition,
     LN_OpCode::SetWorldPosition,
     LN_RuntimeCommandFamily::ObjectTransform},
    {LN_CommandBuffer::CommandType::SetLinearVelocity,
     LN_OpCode::SetLinearVelocity,
     LN_RuntimeCommandFamily::ObjectPhysics},
    {LN_CommandBuffer::CommandType::SetGameProperty,
     LN_OpCode::SetGameProperty,
     LN_RuntimeCommandFamily::ObjectProperty},
    {LN_CommandBuffer::CommandType::AddObject,
     LN_OpCode::AddObject,
     LN_RuntimeCommandFamily::ObjectLifecycle},
    {LN_CommandBuffer::CommandType::SetParent,
     LN_OpCode::SetParent,
     LN_RuntimeCommandFamily::Parenting},
    {LN_CommandBuffer::CommandType::SetGravity,
     LN_OpCode::SetGravity,
     LN_RuntimeCommandFamily::SceneState},
    {LN_CommandBuffer::CommandType::SetGamePaused,
     LN_OpCode::SetGamePaused,
     LN_RuntimeCommandFamily::SceneState},
    {LN_CommandBuffer::CommandType::SetPhysicsPaused,
     LN_OpCode::SetPhysicsPaused,
     LN_RuntimeCommandFamily::SceneState},
    {LN_CommandBuffer::CommandType::SetCameraFov,
     LN_OpCode::SetCameraFov,
     LN_RuntimeCommandFamily::Camera},
    {LN_CommandBuffer::CommandType::SetLightColor,
     LN_OpCode::SetLightColor,
     LN_RuntimeCommandFamily::Light},
    {LN_CommandBuffer::CommandType::SetWindowSize,
     LN_OpCode::SetWindowSize,
     LN_RuntimeCommandFamily::Window},
    {LN_CommandBuffer::CommandType::SetShowProfile,
     LN_OpCode::SetShowProfile,
     LN_RuntimeCommandFamily::Render},
    {LN_CommandBuffer::CommandType::PlayAction,
     LN_OpCode::PlayAction,
     LN_RuntimeCommandFamily::Animation},
    {LN_CommandBuffer::CommandType::SetShapeKeyValue,
     LN_OpCode::SetShapeKeyValue,
     LN_RuntimeCommandFamily::Animation},
    {LN_CommandBuffer::CommandType::SetBonePoseScale,
     LN_OpCode::SetBonePoseScale,
     LN_RuntimeCommandFamily::Animation},
    {LN_CommandBuffer::CommandType::PlaySound,
     LN_OpCode::PlaySound,
     LN_RuntimeCommandFamily::Audio},
    {LN_CommandBuffer::CommandType::SetLogicTreeEnabled,
     LN_OpCode::SetLogicTreeEnabled,
     LN_RuntimeCommandFamily::TreeState},
    {LN_CommandBuffer::CommandType::SendEvent,
     LN_OpCode::SendEvent,
     LN_RuntimeCommandFamily::Events},
    {LN_CommandBuffer::CommandType::SetOverlayCollection,
     LN_OpCode::SetOverlayCollection,
     LN_RuntimeCommandFamily::Collection},
    {LN_CommandBuffer::CommandType::RemoveOverlayCollection,
     LN_OpCode::RemoveOverlayCollection,
     LN_RuntimeCommandFamily::Collection},
    {LN_CommandBuffer::CommandType::LoadScene,
     LN_OpCode::LoadScene,
     LN_RuntimeCommandFamily::GameLifecycle},
    {LN_CommandBuffer::CommandType::RestartGame,
     LN_OpCode::RestartGame,
     LN_RuntimeCommandFamily::GameLifecycle},
    {LN_CommandBuffer::CommandType::SetMaterialParameter,
     LN_OpCode::SetMaterialParameter,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue,
     LN_OpCode::SetMaterialNodeSocketValue,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::SetGeometryNodesInput,
     LN_OpCode::SetGeometryNodesInput,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::SetGeometryNodeSocketValue,
     LN_OpCode::SetGeometryNodeSocketValue,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::SetCompositorNodeSocketValue,
     LN_OpCode::SetCompositorNodeSocketValue,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::MakeNodeTreeUnique,
     LN_OpCode::MakeNodeTreeUnique,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::EnableDisableModifier,
     LN_OpCode::EnableDisableModifier,
     LN_RuntimeCommandFamily::Datablock},
    {LN_CommandBuffer::CommandType::SetMaterialSlot,
     LN_OpCode::SetMaterialSlot,
     LN_RuntimeCommandFamily::ObjectState},
};

TEST(LN_CommandBuffer, SortsCommandsByDeterministicSortKey)
{
  std::vector<LN_CommandBuffer::Command> commands;
  commands.push_back(GamePropertyCommand("ordered", 30.0f, 30, 3));
  commands.push_back(GamePropertyCommand("ordered", 10.0f, 10, 1));
  commands.push_back(GamePropertyCommand("ordered", 20.0f, 20, 2));

  LN_CommandBuffer::SortCommands(commands);

  ASSERT_EQ(commands.size(), 3);
  EXPECT_EQ(commands[0].sort_key, 10);
  EXPECT_EQ(commands[1].sort_key, 20);
  EXPECT_EQ(commands[2].sort_key, 30);
  EXPECT_EQ(commands.back().property_value.float_value, 30.0f);
}

TEST(LN_CommandBuffer, CommandDiagnosticsIncludeFailureContext)
{
  LN_CommandBuffer::Command command;
  command.type = LN_CommandBuffer::CommandType::SetGameProperty;
  command.property_name = "Health";
  command.property_value.type = LN_ValueType::Float;
  command.property_value.float_value = 25.0f;
  command.sort_key = 42;

  const std::string summary = LN_DescribeCommandForDiagnostics(command);
  EXPECT_NE(summary.find("command=SetGameProperty"), std::string::npos);
  EXPECT_NE(summary.find("subsystem=ObjectProperty"), std::string::npos);
  EXPECT_NE(summary.find("target=\"<none>\""), std::string::npos);
  EXPECT_NE(summary.find("name=\"Health\""), std::string::npos);
  EXPECT_NE(summary.find("value_type=Float"), std::string::npos);
  EXPECT_NE(summary.find("sort_key=42"), std::string::npos);

  const std::string failure = LN_DescribeCommandFailure(command, "missing target object");
  EXPECT_NE(failure.find("Logic Nodes command failed: missing target object"),
            std::string::npos);
  EXPECT_NE(failure.find("command=SetGameProperty"), std::string::npos);
}

TEST(LN_CommandBuffer, PreservesRecordOrderForEqualSortKeys)
{
  std::vector<LN_CommandBuffer::Command> commands;
  commands.push_back(GamePropertyCommand("same_key", 1.0f, 42, 10));
  commands.push_back(GamePropertyCommand("same_key", 2.0f, 42, 11));
  commands.push_back(GamePropertyCommand("same_key", 3.0f, 42, 12));

  LN_CommandBuffer::SortCommands(commands);

  ASSERT_EQ(commands.size(), 3);
  EXPECT_EQ(commands[0].source_ref_index, 10);
  EXPECT_EQ(commands[1].source_ref_index, 11);
  EXPECT_EQ(commands[2].source_ref_index, 12);
  EXPECT_EQ(commands.back().property_value.float_value, 3.0f);
}

TEST(LN_CommandBuffer, SameRuntimeTreeKeyPreservesCrossStreamExecutionOrder)
{
  constexpr uint64_t runtime_tree_key = (uint64_t(7) << 32) | 3u;
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendPrint("before", runtime_tree_key, 1);
  command_buffer.AppendSetWorldPosition(
      FakeGameObjectPointer(1), MT_Vector3(1.0f, 2.0f, 3.0f), runtime_tree_key, 2);
  command_buffer.AppendPrint("after", runtime_tree_key, 3);
  command_buffer.EndRecording();

  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "before");
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[2].property_name, "after");
}

TEST(LN_CommandBuffer, RemovesCommandsForDeletedGameObject)
{
  KX_GameObject *removed_object = FakeGameObjectPointer(1);
  KX_GameObject *live_object = FakeGameObjectPointer(2);
  LN_CommandBuffer command_buffer;

  command_buffer.BeginRecording();
  command_buffer.AppendSetWorldPosition(removed_object,
                                        MT_Vector3(1.0f, 0.0f, 0.0f),
                                        1,
                                        0);
  command_buffer.AppendSetWorldPosition(live_object,
                                        MT_Vector3(2.0f, 0.0f, 0.0f),
                                        2,
                                        0);
  command_buffer.AppendSetLinearVelocity(removed_object,
                                        MT_Vector3(0.0f, 1.0f, 0.0f),
                                        3,
                                        0);
  command_buffer.RemoveCommandsForGameObject(removed_object);
  command_buffer.EndRecording();

  ASSERT_EQ(command_buffer.Size(), 1);
  EXPECT_EQ(command_buffer.GetCommandsForTests().front().object, live_object);
}

TEST(LN_CommandBuffer, ClassifiesCommandSubsystems)
{
  for (const CommandTypeSubsystemCase &test_case : kCommandTypeSubsystems) {
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystem(test_case.type), test_case.subsystem)
    << int(test_case.type);
  }

  LN_CommandBuffer::Command subsystem_command;
  subsystem_command.type = LN_CommandBuffer::CommandType::SubsystemCommand;
  subsystem_command.subsystem = LN_CommandBuffer::CommandSubsystem::Camera;
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystem(subsystem_command),
      LN_CommandBuffer::CommandSubsystem::Camera);
}

TEST(LN_CommandBuffer, CommandSubsystemsAgreeWithRuntimeCommandFamilies)
{
  for (const CommandTypeRuntimeFamilyCase &test_case : kCommandRuntimeFamilies) {
    EXPECT_EQ(LN_GetRuntimeCommandFamily(test_case.opcode), test_case.family)
        << int(test_case.type);
    EXPECT_NE(LN_GetRuntimeSideEffectDelivery(test_case.opcode),
              LN_RuntimeSideEffectDelivery::Unknown)
        << int(test_case.type);
  }
}

TEST(LN_CommandBuffer, ExistingCommandsFlushOnMainThread)
{
  EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetWorldPosition),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
  EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetTimeScale),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
  EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetGamePaused),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
  EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetPhysicsPaused),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
  EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
          LN_CommandBuffer::CommandType::MakeLightUnique),
        LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
    EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetLightShadow),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
}

TEST(LN_CommandBuffer, SceneTransitionsRequireOwnersWhileGlobalCommandsDoNot)
{
  using CommandType = LN_CommandBuffer::CommandType;

  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::LoadScene));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetScene));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetOverlayCollection));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::RemoveOverlayCollection));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::RestartGame));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::LoadBlendFile));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::StopAllSounds));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetGamePaused));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetPhysicsPaused));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetWorldPosition));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetGameProperty));
}

TEST(LN_CommandBuffer, OverlayMutationsRetainOwningSceneCameraAndCollectionUntilFlush)
{
  KX_GameObject *owner = FakeGameObjectPointer(71);
  KX_GameObject *camera = FakeGameObjectPointer(72);
  blender::Collection *collection = reinterpret_cast<blender::Collection *>(uintptr_t(73));
  LN_CommandBuffer command_buffer;

  command_buffer.BeginRecording();
  command_buffer.AppendSetOverlayCollection(
      nullptr, owner, camera, collection, LN_INVALID_INDEX, 10, 1);
  command_buffer.AppendSetOverlayCollection(
      nullptr, owner, nullptr, collection, LN_INVALID_INDEX, 15, 3);
  command_buffer.AppendRemoveOverlayCollection(
      nullptr, owner, collection, LN_INVALID_INDEX, 20, 2);
  command_buffer.EndRecording();

  const std::vector<LN_CommandBuffer::Command> commands =
      command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetOverlayCollection);
  EXPECT_EQ(commands[0].object, owner);
  EXPECT_EQ(commands[0].event_target_object, camera);
  EXPECT_EQ(commands[0].collection, collection);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetOverlayCollection);
  EXPECT_EQ(commands[1].event_target_object, nullptr);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::RemoveOverlayCollection);
  EXPECT_EQ(commands[2].object, owner);
  EXPECT_EQ(commands[2].collection, collection);
}

TEST(LN_CommandBuffer, RemovingOverlayCameraCancelsPendingOverlayRequest)
{
  KX_GameObject *owner = FakeGameObjectPointer(71);
  KX_GameObject *camera = FakeGameObjectPointer(72);
  blender::Collection *collection = reinterpret_cast<blender::Collection *>(uintptr_t(73));
  LN_CommandBuffer command_buffer;

  command_buffer.BeginRecording();
  command_buffer.AppendSetOverlayCollection(
      nullptr, owner, camera, collection, LN_INVALID_INDEX, 10, 1);
  command_buffer.AppendRemoveOverlayCollection(
      nullptr, owner, collection, LN_INVALID_INDEX, 20, 2);
  command_buffer.RemoveCommandsForGameObject(camera);
  command_buffer.EndRecording();

  const std::vector<LN_CommandBuffer::Command> commands =
      command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::RemoveOverlayCollection);
  EXPECT_EQ(commands[0].object, owner);
}

TEST(LN_CommandBuffer, ProvidesSubsystemPoliciesForImplementedAndDeferredFamilies)
{
  using CommandSubsystem = LN_CommandBuffer::CommandSubsystem;
  using CommandThreadPolicy = LN_CommandBuffer::CommandThreadPolicy;

  const CommandSubsystem known_subsystems[] = {
      CommandSubsystem::ObjectTransform,
      CommandSubsystem::ObjectPhysics,
      CommandSubsystem::ObjectState,
      CommandSubsystem::ObjectProperty,
      CommandSubsystem::ObjectLifecycle,
      CommandSubsystem::SceneState,
      CommandSubsystem::Parenting,
      CommandSubsystem::Animation,
      CommandSubsystem::Sound,
      CommandSubsystem::Camera,
      CommandSubsystem::Light,
      CommandSubsystem::Render,
      CommandSubsystem::Window,
      CommandSubsystem::Datablock,
      CommandSubsystem::Collection,
      CommandSubsystem::Geometry,
      CommandSubsystem::Group,
      CommandSubsystem::Input,
      CommandSubsystem::TreeState,
        CommandSubsystem::GameLifecycle,
        CommandSubsystem::Events,
  };

  for (const CommandSubsystem subsystem : known_subsystems) {
    const LN_CommandBuffer::CommandSubsystemPolicy policy =
        LN_CommandBuffer::GetCommandSubsystemPolicy(subsystem);
    EXPECT_NE(policy.runtime_service, nullptr);
    EXPECT_NE(policy.notes, nullptr);
  }

  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystemPolicy(CommandSubsystem::ObjectTransform)
                .read_policy,
            CommandThreadPolicy::SnapshotReadOnly);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystemPolicy(CommandSubsystem::Camera).write_policy,
            CommandThreadPolicy::MainThreadFlush);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystemPolicy(CommandSubsystem::Datablock)
                .write_policy,
            CommandThreadPolicy::Unsupported);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystemPolicy(CommandSubsystem::Group).read_policy,
            CommandThreadPolicy::Unsupported);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystemPolicy(CommandSubsystem::Input).read_policy,
            CommandThreadPolicy::SnapshotReadOnly);
}

TEST(LN_CommandBuffer, CommandThreadPoliciesAgreeWithSubsystemWritePolicies)
{
  using CommandType = LN_CommandBuffer::CommandType;

  const CommandType command_types[] = {
      CommandType::SetWorldPosition,
      CommandType::SetLocalPosition,
      CommandType::SetWorldOrientation,
      CommandType::SetLocalOrientation,
      CommandType::SetWorldScale,
      CommandType::SetLocalScale,
      CommandType::SetLinearVelocity,
      CommandType::SetLocalLinearVelocity,
      CommandType::SetAngularVelocity,
      CommandType::SetLocalAngularVelocity,
      CommandType::ApplyImpulse,
      CommandType::SetVisibility,
      CommandType::SetObjectColor,
      CommandType::MakeLightUnique,
      CommandType::SetLightColor,
      CommandType::SetLightPower,
      CommandType::SetLightShadow,
      CommandType::ApplyMovement,
      CommandType::ApplyRotation,
      CommandType::ApplyForce,
      CommandType::ApplyTorque,
      CommandType::SetGameProperty,
      CommandType::AddObject,
      CommandType::AddObjectFromRef,
      CommandType::SetParent,
      CommandType::SetParentFromRef,
      CommandType::RemoveParent,
      CommandType::RemoveObject,
      CommandType::SetGravity,
      CommandType::SetTimeScale,
      CommandType::SetGamePaused,
      CommandType::SetCollisionGroup,
      CommandType::SetPhysics,
      CommandType::SetPhysicsPaused,
      CommandType::SetDynamics,
      CommandType::RebuildCollisionShape,
      CommandType::SetRigidBodyAttribute,
      CommandType::SetTreeProperty,
      CommandType::SetLogicTreeEnabled,
      CommandType::InstallLogicTree,
      CommandType::SetOverlayCollection,
      CommandType::RemoveOverlayCollection,
      CommandType::SetCursorVisibility,
      CommandType::SetCursorPosition,
      CommandType::SetGamepadVibration,
      CommandType::SetWindowSize,
      CommandType::SetFullscreen,
      CommandType::SetVSync,
      CommandType::SetShowFramerate,
      CommandType::SetShowProfile,
      CommandType::SubsystemCommand,
  };

  for (const CommandType command_type : command_types) {
    const LN_CommandBuffer::CommandSubsystem subsystem =
        LN_CommandBuffer::GetCommandSubsystem(command_type);
    EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(command_type),
              LN_CommandBuffer::GetCommandSubsystemPolicy(subsystem).write_policy);
  }
}

TEST(LN_CommandBuffer, RecordsDeferredSubsystemCommandsForMainThreadFamilies)
{
  LN_CommandBuffer command_buffer;
  LN_Value payload;
  payload.type = LN_ValueType::Float;
  payload.float_value = 45.0f;

  command_buffer.BeginRecording();
  command_buffer.AppendSubsystemCommand(LN_CommandBuffer::CommandSubsystem::Camera,
                                        "set_lens",
                                        nullptr,
                                        LN_RuntimeRef(),
                                        payload,
                                        20,
                                        1);
  command_buffer.AppendSubsystemCommand(LN_CommandBuffer::CommandSubsystem::Datablock,
                                        "mutate_id",
                                        nullptr,
                                        LN_RuntimeRef(),
                                        payload,
                                        30,
                                        2);
  command_buffer.EndRecording();

  ASSERT_EQ(command_buffer.Size(), 1u);
  const LN_CommandBuffer::Command &command = command_buffer.GetCommandsForTests().front();
  EXPECT_EQ(command.type, LN_CommandBuffer::CommandType::SubsystemCommand);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystem(command),
            LN_CommandBuffer::CommandSubsystem::Camera);
  EXPECT_EQ(command.property_name, "set_lens");
  EXPECT_EQ(command.property_value.float_value, 45.0f);
}

TEST(LN_CommandBuffer, TakesRecordedCommandsForProductionTransfer)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendSetWorldPosition(
      FakeGameObjectPointer(1), MT_Vector3(1.0f, 2.0f, 3.0f), 10, 0);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands.front().type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(command_buffer.Size(), 0u);
  EXPECT_TRUE(command_buffer.TakeRecordedCommands().empty());
}

TEST(LN_CommandBuffer, FlushesAggregatedCommandsInSameTick)
{
  LN_CommandBuffer command_buffer;
  LN_CommandBuffer::Command command;
  command.type = LN_CommandBuffer::CommandType::SetShowProfile;
  command.bool_value = false;
  command.sort_key = 1;

  command_buffer.AppendLegacyCommandsForTests({command});
  ASSERT_EQ(command_buffer.Size(), 1u);

  command_buffer.Flush();
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, ClassifiesTypedCommandStreamPolicies)
{
  using CommandType = LN_CommandBuffer::CommandType;

  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetWorldPosition),
            LN_CommandBuffer::CommandClass::AbsoluteObjectTransform);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetLinearVelocity),
            LN_CommandBuffer::CommandClass::AbsoluteObjectVelocity);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyMovement),
            LN_CommandBuffer::CommandClass::RelativeObjectVector);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyForce),
            LN_CommandBuffer::CommandClass::RelativeObjectVector);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetGameProperty),
            LN_CommandBuffer::CommandClass::GamePropertyWrite);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetTreeProperty),
            LN_CommandBuffer::CommandClass::TreePropertyWrite);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SendEvent),
            LN_CommandBuffer::CommandClass::EventSend);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::PlaySound),
            LN_CommandBuffer::CommandClass::SimpleAudioSideEffect);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::AddObject),
            LN_CommandBuffer::CommandClass::ObjectLifecycleMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetObjectColor),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetLightPower),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetGravity),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetTimeScale),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetGamePaused),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetPhysicsPaused),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetActiveCamera),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyImpulse),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetCharacterVelocity),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::VehicleApplyEngineForce),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);

  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::SetWorldPosition),
            LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::SetMaterialParameter),
            LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::SetShapeKeyValue),
            LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::PlaySound),
            LN_CommandBuffer::CoalescingPolicy::Forbidden);
}

/* Phase A6: the class and the merge policy of every command type are read from one table made at
 * build time. The last-write-wins set below is the old switch's list, written out. */
TEST(LN_CommandBuffer, TheTypeTableGivesTheOldClassesAndTheOldMergeSet)
{
  using CommandType = LN_CommandBuffer::CommandType;
  const std::set<CommandType> last_write_wins = {CommandType::SetWorldPosition,
                                                 CommandType::SetLocalPosition,
                                                 CommandType::SetWorldOrientation,
                                                 CommandType::SetLocalOrientation,
                                                 CommandType::SetWorldScale,
                                                 CommandType::SetLocalScale,
                                                 CommandType::SetLinearVelocity,
                                                 CommandType::SetLocalLinearVelocity,
                                                 CommandType::SetAngularVelocity,
                                                 CommandType::SetLocalAngularVelocity,
                                                 CommandType::SetGameProperty,
                                                 CommandType::SetTreeProperty,
                                                 CommandType::SetMaterialParameter,
                                                 CommandType::SetShapeKeyValue,
                                                 CommandType::SetGeometryNodesInput,
                                                 CommandType::SetGeometryNodeSocketValue,
                                                 CommandType::SetCompositorNodeSocketValue,
                                                 CommandType::SetNodeMute};
  /* Every value of the enum, up to the last one (Set Volume). */
  for (int value = 0; value <= int(CommandType::SetSoundVolume); value++) {
    const CommandType type = CommandType(value);
    SCOPED_TRACE(value);
    EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(type) ==
                  LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment,
              last_write_wins.count(type) == 1);
  }
  /* The vector commands go to their stream by class: absolute setters, velocities, relative. */
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyMovement),
            LN_CommandBuffer::CommandClass::RelativeObjectVector);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyRotation),
            LN_CommandBuffer::CommandClass::RelativeObjectVector);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetLinearVelocity),
            LN_CommandBuffer::CommandClass::AbsoluteObjectVelocity);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetWorldScale),
            LN_CommandBuffer::CommandClass::AbsoluteObjectTransform);
}

/* A micro benchmark of recording, not a check (DISABLED_: run with --gtest_also_run_disabled_tests
 * --gtest_filter=*RecordCostBench). One step of 10,000 NPCs: a move, a turn and five number
 * writes each, recorded, ended and cleared; the best of 15 steps. */
TEST(LN_CommandBuffer, DISABLED_RecordCostBench)
{
  std::vector<std::string> names = {"p0", "p1", "p2", "p3", "p4"};
  NamedPropertyWriter writer(names);
  constexpr uint32_t npcs = 10000;
  std::deque<ln_test::BatchObject> objects;
  for (uint32_t index = 0; index < npcs; index++) {
    objects.emplace_back(MT_Vector3(0.0f, 0.0f, 0.0f));
  }
  LN_PropertyNumber number;
  number.type = LN_ValueType::Float;
  number.float_value = 1.5f;
  LN_CommandBuffer buffer;
  /* [0] everything, [1] moves only, [2] turns only, [3] number writes only. */
  double best[4] = {1.0e9, 1.0e9, 1.0e9, 1.0e9};
  double order_best = 1.0e9, merge_best = 1.0e9;
  for (int round = 0; round < 15; round++) {
    for (int kind = 0; kind < 4; kind++) {
      const auto start = std::chrono::steady_clock::now();
      buffer.BeginRecording();
      for (uint32_t index = 0; index < npcs; index++) {
        KX_GameObject *object = &objects[index].object;
        if (kind == 0 || kind == 1) {
          buffer.AppendApplyMovement(object, MT_Vector3(0.0f, 0.002f, 0.0f), true, index, 1);
        }
        if (kind == 0 || kind == 2) {
          buffer.AppendApplyRotation(object, MT_Vector3(0.0f, 0.0f, 0.002f), true, index, 2);
        }
        if (kind == 0 || kind == 3) {
          for (uint32_t property = 0; property < 5; property++) {
            writer.WriteNumber(buffer, object, names[property], number, index, 3 + property);
          }
        }
      }
      buffer.EndRecording();
      const double recorded = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
      if (kind == 0) {
        /* The flush's order of all of it, without applying anything. */
        const auto order_start = std::chrono::steady_clock::now();
        buffer.DirectFlushOrderForTests(false);
        order_best = std::min(order_best,
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - order_start)
                                  .count());
        const auto merge_start = std::chrono::steady_clock::now();
        buffer.DirectFlushOrderForTests(true);
        merge_best = std::min(merge_best,
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - merge_start)
                                  .count());
      }
      buffer.Clear();
      best[kind] = std::min(best[kind], recorded);
    }
  }
  std::printf("RECORD_BENCH all %.3f ms, moves %.3f, turns %.3f, 5 number writes %.3f (%u NPCs); flush order %.3f, with merge marks %.3f; sizes %s\n",
              best[0],
              best[1],
              best[2],
              best[3],
              npcs,
              order_best,
              merge_best,
              LN_CommandBuffer::StreamEntrySizesForTests().c_str());
}

/* A micro benchmark of the flush (DISABLED_): 10,000 objects with five float properties, five
 * number writes each, applied by name or through checked handles; best of 15. */
TEST(LN_CommandBuffer, DISABLED_FlushCostBench)
{
  std::vector<std::string> names = {"p0", "p1", "p2", "p3", "p4"};
  NamedPropertyWriter writer(names);
  constexpr uint32_t npcs = 10000;
  std::deque<ln_test::BatchObject> objects;
  for (uint32_t index = 0; index < npcs; index++) {
    objects.emplace_back(MT_Vector3(0.0f, 0.0f, 0.0f));
    for (const std::string &name : names) {
      EXP_Value *property = new EXP_FloatValue(0.0f);
      objects.back().object.SetProperty(name, property);
      property->Release();
    }
  }
  LN_PropertyNumber number;
  number.type = LN_ValueType::Float;
  number.float_value = 1.5f;
  LN_CommandBuffer buffer;
  for (const int mode : {0, 1}) {
    LN_CommandBuffer::SetLeanFlushModeForTests(mode);
    double best = 1.0e9;
    for (int round = 0; round < 15; round++) {
      buffer.BeginRecording();
      for (uint32_t index = 0; index < npcs; index++) {
        KX_GameObject *object = &objects[index].object;
        const uint64_t stamp = object->GetPropertyMapStamp();
        for (uint32_t property = 0; property < 5; property++) {
          buffer.AppendSetGamePropertyNumberRef(writer.Tree(),
                                                object,
                                                property,
                                                number,
                                                index,
                                                3 + property,
                                                object->GetProperty(names[property]),
                                                stamp);
        }
      }
      buffer.EndRecording();
      const auto start = std::chrono::steady_clock::now();
      buffer.Flush();
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count());
    }
    std::printf("FLUSH_BENCH mode %d: %.3f ms for %u x 5 number writes\n", mode, best, npcs);
  }
  LN_CommandBuffer::SetLeanFlushModeForTests(-1);
}

TEST(LN_CommandBuffer, TypedStreamsMaterializeInDeterministicRecordOrder)
{
  LN_CommandBuffer command_buffer;
  NamedPropertyWriter speed_writer({"Speed"});
  command_buffer.BeginRecording();
  command_buffer.AppendSetWorldPosition(
      FakeGameObjectPointer(1), MT_Vector3(1.0f, 0.0f, 0.0f), 30, 1);
  command_buffer.AppendSetLinearVelocity(
      FakeGameObjectPointer(1), MT_Vector3(0.0f, 2.0f, 0.0f), 10, 2);
  command_buffer.AppendApplyMovement(
      FakeGameObjectPointer(1), MT_Vector3(3.0f, 0.0f, 0.0f), true, 25, 6);
  command_buffer.AppendApplyForce(
      FakeGameObjectPointer(1), MT_Vector3(0.0f, 0.0f, 7.0f), false, 35, 7);
  LN_Value property_value;
  property_value.type = LN_ValueType::Float;
  property_value.float_value = 4.0f;
  speed_writer.Write(command_buffer, FakeGameObjectPointer(1), "Speed", property_value, 20, 3);
  command_buffer.AppendSendEvent("Started", property_value, FakeGameObjectPointer(1), nullptr, 40, 4);
  command_buffer.AppendStopAllSounds(45, 8);
  command_buffer.AppendPlaySound("Ping", 0.5f, 1.25f, true, 50, 5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 8u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetLinearVelocity);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetGameProperty);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::ApplyMovement);
  EXPECT_EQ(commands[2].vector_value.x(), 3.0f);
  EXPECT_TRUE(commands[2].bool_value);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::ApplyForce);
  EXPECT_EQ(commands[4].vector_value.z(), 7.0f);
  EXPECT_FALSE(commands[4].bool_value);
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::SendEvent);
  EXPECT_EQ(commands[6].type, LN_CommandBuffer::CommandType::StopAllSounds);
  EXPECT_EQ(commands[7].type, LN_CommandBuffer::CommandType::PlaySound);
  EXPECT_EQ(commands[7].vector_value.x(), 0.5f);
  EXPECT_EQ(commands[7].vector_value.y(), 1.25f);
  EXPECT_TRUE(commands[7].bool_value);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_transform_count, 1u);
  EXPECT_EQ(stats.typed_velocity_count, 1u);
  EXPECT_EQ(stats.typed_relative_vector_count, 2u);
  EXPECT_EQ(stats.typed_property_count, 1u);
  EXPECT_EQ(stats.typed_event_count, 1u);
  EXPECT_EQ(stats.typed_audio_count, 2u);
}

TEST(LN_CommandBuffer, DirectTypedVectorFlushHandlesMissingTarget)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendApplyMovement(nullptr, MT_Vector3(1.0f, 0.0f, 0.0f), false, 10, 1);
  command_buffer.EndRecording();

  command_buffer.Flush();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 1u);
  EXPECT_EQ(stats.coalesced_command_count, 0u);
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, DirectTypedRelativeVectorFlushUsesSingleStreamFastPath)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  for (uint32_t index = 0; index < 4096; index++) {
    if ((index & 1u) == 0u) {
      command_buffer.AppendApplyMovement(nullptr,
                                         MT_Vector3(1.0f, 0.0f, 0.0f),
                                         false,
                                         10,
                                         1);
    }
    else {
      command_buffer.AppendApplyRotation(nullptr,
                                         MT_Vector3(0.0f, 1.0f, 0.0f),
                                         true,
                                         10,
                                         1);
    }
  }
  command_buffer.EndRecording();

  command_buffer.Flush();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 4096u);
  EXPECT_EQ(stats.coalesced_command_count, 0u);
  EXPECT_EQ(stats.direct_ordered_stream_count, 1u);
  EXPECT_EQ(stats.direct_ordered_command_count, 4096u);
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, OutOfOrderTypedRelativeVectorStreamUsesPlannerSort)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendApplyMovement(nullptr,
                                     MT_Vector3(2.0f, 0.0f, 0.0f),
                                     false,
                                     20,
                                     1);
  command_buffer.AppendApplyMovement(nullptr,
                                     MT_Vector3(1.0f, 0.0f, 0.0f),
                                     false,
                                     10,
                                     2);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> planned = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(planned.size(), 2u);
  EXPECT_EQ(planned[0].sort_key, 10u);
  EXPECT_EQ(planned[0].vector_value.x(), 1.0f);
  EXPECT_EQ(planned[1].sort_key, 20u);
  EXPECT_EQ(planned[1].vector_value.x(), 2.0f);
  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 2u);
  EXPECT_EQ(stats.direct_ordered_stream_count, 0u);
  EXPECT_EQ(stats.direct_ordered_command_count, 0u);
}

/* Streams out of order flush in place too (since 2026-10-31), in sort key order: the planned
 * flush's order, without its copy of every command. */
TEST(LN_CommandBuffer, OutOfOrderTypedStreamsFlushInPlaceInSortOrder)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendApplyMovement(nullptr,
                                     MT_Vector3(2.0f, 0.0f, 0.0f),
                                     false,
                                     20,
                                     1);
  command_buffer.AppendApplyMovement(nullptr,
                                     MT_Vector3(1.0f, 0.0f, 0.0f),
                                     false,
                                     10,
                                     2);
  command_buffer.EndRecording();
  ASSERT_FALSE(command_buffer.FlushDecisionForTests().first);
  const std::vector<std::pair<uint64_t, uint64_t>> order = {{10, 2}, {20, 1}};
  EXPECT_EQ(command_buffer.DirectFlushOrderForTests(false), order);

  command_buffer.Flush();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 2u);
  EXPECT_EQ(stats.direct_ordered_stream_count, 1u);
  EXPECT_EQ(stats.direct_ordered_command_count, 2u);
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, DirectFlushHandlesMixedLegacyAndTypedStreams)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  LN_Value payload;
  command_buffer.AppendSubsystemCommand(LN_CommandBuffer::CommandSubsystem::Camera,
                                        "noop",
                                        nullptr,
                                        LN_RuntimeRef{},
                                        payload,
                                        10,
                                        1);
  command_buffer.AppendApplyMovement(nullptr, MT_Vector3(1.0f, 0.0f, 0.0f), false, 20, 2);
  command_buffer.EndRecording();

  command_buffer.Flush();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  EXPECT_EQ(stats.legacy_command_count, 1u);
  EXPECT_EQ(stats.typed_relative_vector_count, 1u);
  EXPECT_EQ(stats.coalesced_command_count, 0u);
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, TypedLifecycleStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *owner = FakeGameObjectPointer(1);
  KX_GameObject *target = FakeGameObjectPointer(2);
  LN_RuntimeTree *runtime_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_RuntimeRef object_ref;
  object_ref.kind = LN_RuntimeRefKind::Object;
  object_ref.slot = 4;
  object_ref.generation = 5;
  object_ref.debug_name = "CubeTemplate";
  LN_RuntimeRef cube_ref = object_ref;
  cube_ref.debug_name = "Cube";
  LN_RuntimeRef parent_ref = object_ref;
  parent_ref.debug_name = "Parent";

  command_buffer.BeginRecording();
  command_buffer.AppendAddObjectFromRef(
      runtime_tree, owner, cube_ref, 0.0f, false, LN_INVALID_INDEX, 30, 1);
  command_buffer.AppendSetParentFromRef(runtime_tree, owner, parent_ref, true, false, 10, 2);
  command_buffer.AppendReplaceMesh(owner, target, 20, 3);
  command_buffer.AppendAddObjectFromRef(runtime_tree, owner, object_ref, 2.5f, true, 6, 35, 7);
  command_buffer.AppendCopyProperty(owner, target, "speed", 40, 4);
  command_buffer.AppendRemoveObject(owner, 50, 5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 6u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetParentFromRef);
  EXPECT_EQ(commands[0].runtime_ref.debug_name, "Parent");
  EXPECT_TRUE(commands[0].bool_value);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::ReplaceMesh);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(commands[1].runtime_tree), target);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::AddObjectFromRef);
  EXPECT_EQ(commands[2].runtime_ref.debug_name, "Cube");
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::AddObjectFromRef);
  EXPECT_EQ(commands[3].runtime_tree, runtime_tree);
  EXPECT_EQ(commands[3].runtime_ref.kind, LN_RuntimeRefKind::Object);
  EXPECT_EQ(commands[3].runtime_ref.slot, 4u);
  EXPECT_EQ(commands[3].runtime_ref.generation, 5u);
  EXPECT_EQ(commands[3].runtime_ref.debug_name, "CubeTemplate");
  EXPECT_EQ(commands[3].scalar_value, 2.5f);
  EXPECT_TRUE(commands[3].bool_value);
  EXPECT_EQ(commands[3].property_ref_index, 6u);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::CopyProperty);
  EXPECT_EQ(commands[4].property_name, "speed");
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::RemoveObject);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_lifecycle_count, 6u);
}

TEST(LN_CommandBuffer, TypedObjectServiceStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendSetObjectColor(object, MT_Vector4(0.1f, 0.2f, 0.3f, 1.0f), 30, 1);
  command_buffer.AppendSetVisibility(object, true, false, 10, 2);
  command_buffer.AppendSetLightPower(object, 3.5f, 20, 3);
  command_buffer.AppendSetGravity(object, MT_Vector3(0.0f, 0.0f, -9.8f), 25, 6);
  command_buffer.AppendSetCameraFov(object, 60.0f, 40, 4);
  command_buffer.AppendMakeLightUnique(object, 50, 5);
  command_buffer.AppendSetTimeScale(object, 0.5f, 60, 7);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 7u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_TRUE(commands[0].bool_value);
  EXPECT_FALSE(commands[0].secondary_bool_value);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetLightPower);
  EXPECT_EQ(commands[1].scalar_value, 3.5f);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetGravity);
  EXPECT_EQ(commands[2].vector_value.z(), -9.8f);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SetObjectColor);
  EXPECT_EQ(commands[3].color_value.x(), 0.1f);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::SetCameraFov);
  EXPECT_EQ(commands[4].scalar_value, 60.0f);
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::MakeLightUnique);
  EXPECT_EQ(commands[6].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(commands[6].scalar_value, 0.5f);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_object_service_count, 7u);
}

TEST(LN_CommandBuffer, TypedRuntimeServiceStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;

  command_buffer.BeginRecording();
  command_buffer.AppendSetWindowSize(1280, 720, 30, 1);
  command_buffer.AppendSetCursorVisibility(false, 10, 2);
  command_buffer.AppendSetCursorPosition(100, 200, 20, 3);
  command_buffer.AppendSetGamePaused(true, 25, 9);
  command_buffer.AppendSetGamepadVibration(1, 0.25f, 0.75f, 500, 40, 4);
  command_buffer.AppendSetPhysicsPaused(false, 45, 10);
  command_buffer.AppendSetFullscreen(true, 50, 5);
  command_buffer.AppendSetVSync(2, 60, 6);
  command_buffer.AppendSetShowFramerate(true, 70, 7);
  command_buffer.AppendSetShowProfile(false, 80, 8);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 10u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetCursorVisibility);
  EXPECT_FALSE(commands[0].bool_value);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetCursorPosition);
  EXPECT_EQ(commands[1].int_value, 100);
  EXPECT_EQ(commands[1].secondary_int_value, 200);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetGamePaused);
  EXPECT_TRUE(commands[2].bool_value);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SetWindowSize);
  EXPECT_EQ(commands[3].int_value, 1280);
  EXPECT_EQ(commands[3].secondary_int_value, 720);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::SetGamepadVibration);
  EXPECT_EQ(commands[4].int_value, 1);
  EXPECT_EQ(commands[4].scalar_value, 0.25f);
  EXPECT_EQ(commands[4].vector_value.x(), 0.75f);
  EXPECT_EQ(commands[4].property_ref_index, 500u);
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::SetPhysicsPaused);
  EXPECT_FALSE(commands[5].bool_value);
  EXPECT_EQ(commands[6].type, LN_CommandBuffer::CommandType::SetFullscreen);
  EXPECT_TRUE(commands[6].bool_value);
  EXPECT_EQ(commands[7].type, LN_CommandBuffer::CommandType::SetVSync);
  EXPECT_EQ(commands[7].int_value, 2);
  EXPECT_EQ(commands[8].type, LN_CommandBuffer::CommandType::SetShowFramerate);
  EXPECT_TRUE(commands[8].bool_value);
  EXPECT_EQ(commands[9].type, LN_CommandBuffer::CommandType::SetShowProfile);
  EXPECT_FALSE(commands[9].bool_value);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_runtime_service_count, 10u);
}

TEST(LN_CommandBuffer, TypedAnimationStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendStopAction(object, 2, 30, 1);
  command_buffer.AppendPlayAction(object,
                                  nullptr,
                                  "Walk",
                                  1.0f,
                                  24.0f,
                                  3,
                                  4,
                                  0.25f,
                                  0.8f,
                                  0x0010203u,
                                  1.5f,
                                  LN_INVALID_INDEX,
                                  LN_ACTION_OUTPUT_NONE,
                                  10,
                                  2);
  command_buffer.AppendSetActionFrame(object, 5, 12.5f, 20, 3);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::PlayAction);
  EXPECT_EQ(commands[0].property_name, "Walk");
  EXPECT_EQ(commands[0].vector_value.x(), 1.0f);
  EXPECT_EQ(commands[0].vector_value.y(), 24.0f);
  EXPECT_EQ(commands[0].vector_value.z(), 0.25f);
  EXPECT_EQ(commands[0].secondary_vector_value.x(), 0.8f);
  EXPECT_EQ(commands[0].secondary_vector_value.y(), 1.5f);
  EXPECT_EQ(commands[0].int_value, 3);
  EXPECT_EQ(commands[0].secondary_int_value, 4);
  EXPECT_EQ(commands[0].animation_flags, 0x0010203u);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetActionFrame);
  EXPECT_EQ(commands[1].int_value, 5);
  EXPECT_EQ(commands[1].scalar_value, 12.5f);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::StopAction);
  EXPECT_EQ(commands[2].int_value, 2);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_animation_count, 3u);
}

TEST(LN_CommandBuffer, ShapeKeyWritesUseTypedStreamAndCoalesceByObjectAndName)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendSetShapeKeyValue(object, "Smile", 0.25f, 10, 1);
  command_buffer.AppendSetShapeKeyValue(object, "Smile", 0.75f, 20, 2);
  command_buffer.AppendSetShapeKeyValue(object, "Blink", 0.5f, 30, 3);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands =
      command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().legacy_command_count, 0u);
  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().typed_animation_count, 3u);

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  const std::vector<LN_CommandBuffer::Command> planned =
      command_buffer.TakeFlushPlannedCommands();
  ASSERT_EQ(planned.size(), 2u);
  EXPECT_EQ(planned[0].type, LN_CommandBuffer::CommandType::SetShapeKeyValue);
  EXPECT_EQ(planned[0].object, object);
  EXPECT_EQ(planned[0].property_name, "Smile");
  EXPECT_EQ(planned[0].scalar_value, 0.75f);
  EXPECT_EQ(planned[1].type, LN_CommandBuffer::CommandType::SetShapeKeyValue);
  EXPECT_EQ(planned[1].property_name, "Blink");
  EXPECT_EQ(planned[1].scalar_value, 0.5f);
  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 1u);
}

TEST(LN_CommandBuffer, ShapeKeyWriteClampsToAuthoredRange)
{
  blender::Mesh mesh = {};
  mesh.verts_num = 3;
  /* Keep this fixture independent of KX_Scene. The production CPU path differs only by
   * enqueueing the same geometry update used by BL_Action. */
  mesh.is_running_gpu_animation_playback = true;

  blender::Key key = {};
  key.type = blender::KEY_RELATIVE;
  mesh.key = &key;

  blender::KeyBlock basis = {};
  basis.totelem = mesh.verts_num;
  std::strncpy(basis.name, "Basis", sizeof(basis.name) - 1);
  blender::KeyBlock smile = {};
  smile.totelem = mesh.verts_num;
  smile.slidermin = -0.25f;
  smile.slidermax = 0.75f;
  std::strncpy(smile.name, "Smile", sizeof(smile.name) - 1);
  basis.next = &smile;
  smile.prev = &basis;
  key.block.first = &basis;
  key.block.last = &smile;
  key.refkey = &basis;

  blender::Object blender_object = {};
  blender_object.type = blender::OB_MESH;
  blender_object.data = &mesh.id;
  KX_GameObject game_object;
  game_object.SetBlenderObject(&blender_object);

  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendSetShapeKeyValue(&game_object, "Smile", 2.0f, 10, 1);
  command_buffer.EndRecording();
  command_buffer.Flush();

  EXPECT_EQ(smile.curval, 0.75f);
}

TEST(LN_CommandBuffer, TypedMaterialStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  LN_Value material_value;
  material_value.type = LN_ValueType::DatablockRef;
  material_value.exists = true;
  material_value.reference_name = "MatRef";

  LN_Value socket_value;
  socket_value.type = LN_ValueType::Float;
  socket_value.float_value = 0.75f;

  command_buffer.BeginRecording();
  command_buffer.AppendAssignMaterialToSlot(nullptr, object, material_value, 2, 30, 1);
  command_buffer.AppendSetMaterialNodeSocketValue(
      nullptr, material_value, "Principled", "Roughness", socket_value, 10, 2);
  command_buffer.AppendSetMaterialNodeSocketValue(
      nullptr, material_value, "Node", "Base Color", socket_value, 40, 4);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue);
  EXPECT_EQ(commands[0].property_name, "MatRef");
  EXPECT_EQ(commands[0].secondary_property_name, "Principled");
  EXPECT_EQ(commands[0].tertiary_property_name, "Roughness");
  EXPECT_EQ(commands[0].property_value.float_value, 0.75f);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetMaterialSlot);
  EXPECT_EQ(commands[1].object, object);
  EXPECT_EQ(commands[1].property_name, "MatRef");
  EXPECT_EQ(commands[1].int_value, 2);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue);
  EXPECT_EQ(commands[2].property_name, "MatRef");
  EXPECT_EQ(commands[2].secondary_property_name, "Node");
  EXPECT_EQ(commands[2].tertiary_property_name, "Base Color");

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_material_count, 3u);
}

TEST(LN_CommandBuffer, GeometryNodeWritesUseTypedStreamAndCoalesceByExactTarget)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  LN_Value first_value;
  first_value.type = LN_ValueType::Float;
  first_value.exists = true;
  first_value.float_value = 1.0f;

  LN_Value final_value = first_value;
  final_value.float_value = 2.0f;

  command_buffer.BeginRecording();
  command_buffer.AppendSetGeometryNodesInput(
      nullptr, object, "GeometryNodes", "Socket_2", first_value, 10, 1);
  command_buffer.AppendSetGeometryNodesInput(
      nullptr, object, "GeometryNodes", "Socket_2", final_value, 20, 2);
  command_buffer.AppendSetGeometryNodesInput(
      nullptr, object, "GeometryNodes", "Socket_3", first_value, 30, 3);
  command_buffer.AppendSetGeometryNodeSocketValue(
      nullptr, object, "GeometryNodes", "Set Position", "Offset", first_value, 40, 4);
  command_buffer.AppendSetGeometryNodeSocketValue(
      nullptr, object, "GeometryNodes", "Set Position", "Offset", final_value, 50, 5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetGeometryNodesInput);
  EXPECT_EQ(commands[0].object, object);
  EXPECT_EQ(commands[0].property_name, "GeometryNodes");
  EXPECT_EQ(commands[0].secondary_property_name, "Socket_2");
  EXPECT_EQ(commands[0].property_value.float_value, 2.0f);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetGeometryNodesInput);
  EXPECT_EQ(commands[1].secondary_property_name, "Socket_3");
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetGeometryNodeSocketValue);
  EXPECT_EQ(commands[2].property_name, "GeometryNodes");
  EXPECT_EQ(commands[2].secondary_property_name, "Set Position");
  EXPECT_EQ(commands[2].tertiary_property_name, "Offset");
  EXPECT_EQ(commands[2].property_value.float_value, 2.0f);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_material_count, 5u);
  EXPECT_EQ(stats.coalesced_command_count, 2u);
}

TEST(LN_CommandBuffer, ModifierEnableCommandsUseTypedStreamAndPreserveOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendEnableDisableModifier(object,
                                             LN_CommandBuffer::ModifierTarget::Last,
                                             std::string(),
                                             0,
                                             false,
                                             10,
                                             1);
  command_buffer.AppendEnableDisableModifier(object,
                                             LN_CommandBuffer::ModifierTarget::Last,
                                             std::string(),
                                             0,
                                             true,
                                             20,
                                             2);
  command_buffer.AppendEnableDisableModifier(object,
                                             LN_CommandBuffer::ModifierTarget::Name,
                                             std::string(),
                                             0,
                                             false,
                                             30,
                                             3);
  command_buffer.AppendEnableDisableModifier(object,
                                             LN_CommandBuffer::ModifierTarget::Index,
                                             std::string(),
                                             -1,
                                             false,
                                             40,
                                             4);
  command_buffer.AppendEnableDisableModifier(object,
                                             LN_CommandBuffer::ModifierTarget::PersistentId,
                                             std::string(),
                                             12345,
                                             true,
                                             50,
                                             5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats = command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 5u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::EnableDisableModifier);
  EXPECT_EQ(commands[0].object, object);
  EXPECT_EQ(commands[0].property_value.type, LN_ValueType::Int);
  EXPECT_EQ(commands[0].property_value.int_value,
            int32_t(LN_CommandBuffer::ModifierTarget::Last));
  EXPECT_FLOAT_EQ(commands[0].scalar_value, 0.0f);
  EXPECT_FLOAT_EQ(commands[1].scalar_value, 1.0f);
  EXPECT_EQ(commands[2].property_value.int_value,
            int32_t(LN_CommandBuffer::ModifierTarget::Name));
  EXPECT_TRUE(commands[2].property_name.empty());
  EXPECT_EQ(commands[3].property_value.int_value,
            int32_t(LN_CommandBuffer::ModifierTarget::Index));
  EXPECT_EQ(commands[3].int_value, -1);
  EXPECT_EQ(commands[4].property_value.int_value,
            int32_t(LN_CommandBuffer::ModifierTarget::PersistentId));
  EXPECT_EQ(commands[4].int_value, 12345);
  EXPECT_FLOAT_EQ(commands[4].scalar_value, 1.0f);
  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_material_count, 5u);
  EXPECT_EQ(stats.coalesced_command_count, 0u);
}

TEST(LN_CommandBuffer, SetMaterialParameterCoalescesSharedMaterialWrites)
{
  LN_CommandBuffer command_buffer;

  LN_Value material_value;
  material_value.type = LN_ValueType::DatablockRef;
  material_value.exists = true;
  material_value.reference_name = "SharedMat";

  LN_Value red_value;
  red_value.type = LN_ValueType::Color;
  red_value.exists = true;
  red_value.color_value = MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f);

  LN_Value green_value;
  green_value.type = LN_ValueType::Color;
  green_value.exists = true;
  green_value.color_value = MT_Vector4(0.0f, 1.0f, 0.0f, 1.0f);

  command_buffer.BeginRecording();
  command_buffer.AppendSetMaterialParameter(
      nullptr, material_value, "Principled", "Base Color", red_value, 10, 1);
  command_buffer.AppendSetMaterialParameter(
      nullptr, material_value, "Principled", "Base Color", green_value, 20, 2);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetMaterialParameter);
  EXPECT_EQ(commands[0].property_name, "SharedMat");
  EXPECT_EQ(commands[0].secondary_property_name, "Principled");
  EXPECT_EQ(commands[0].tertiary_property_name, "Base Color");
  EXPECT_EQ(commands[0].property_value.type, LN_ValueType::Color);
  EXPECT_NEAR(commands[0].property_value.color_value[0], 0.0f, 0.0001f);
  EXPECT_NEAR(commands[0].property_value.color_value[1], 1.0f, 0.0001f);
  EXPECT_NEAR(commands[0].property_value.color_value[2], 0.0f, 0.0001f);
  EXPECT_NEAR(commands[0].property_value.color_value[3], 1.0f, 0.0001f);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_material_count, 2u);
  EXPECT_EQ(stats.coalesced_command_count, 1u);
}

TEST(LN_CommandBuffer, SetMaterialParameterKeepsDifferentTargetsSeparate)
{
  LN_CommandBuffer command_buffer;

  LN_Value material_value;
  material_value.type = LN_ValueType::DatablockRef;
  material_value.exists = true;
  material_value.reference_name = "SharedMat";

  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  value.float_value = 0.5f;

  command_buffer.BeginRecording();
  command_buffer.AppendSetMaterialParameter(
      nullptr, material_value, "Principled", "Roughness", value, 10, 1);
  command_buffer.AppendSetMaterialParameter(
      nullptr, material_value, "Principled", "Metallic", value, 20, 2);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetMaterialParameter);
  EXPECT_EQ(commands[0].tertiary_property_name, "Roughness");
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetMaterialParameter);
  EXPECT_EQ(commands[1].tertiary_property_name, "Metallic");
  EXPECT_EQ(stats.coalesced_command_count, 0u);
}

TEST(LN_CommandBuffer, TypedArmatureStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *armature = FakeGameObjectPointer(1);

  LN_Value value;
  value.type = LN_ValueType::Float;
  value.float_value = 0.5f;

  command_buffer.BeginRecording();
  command_buffer.AppendSetBonePoseLocation(
      armature,
      "Bone",
      MT_Vector3(1.0f, 2.0f, 3.0f),
      30,
      1,
      int(LN_BonePoseLocationSpace::World),
      true);
  command_buffer.AppendSetBonePoseRotation(
      armature, "Bone", MT_Vector3(0.1f, 0.2f, 0.3f), 20, 2, true);
  command_buffer.AppendSetBonePoseTransform(armature,
                                            "Bone",
                                            MT_Vector3(4.0f, 5.0f, 6.0f),
                                            MT_Vector3(0.4f, 0.5f, 0.6f),
                                            25,
                                            7,
                                            int(LN_BonePoseLocationSpace::World),
                                            true);
  command_buffer.AppendSetBoneAttribute(armature, "Bone", 13, 0, value, 50, 3);
  command_buffer.AppendSetBoneConstraintInfluence(armature, "Bone", "CopyLoc", 0.75f, 10, 4);
  command_buffer.AppendSetBoneConstraintTarget(nullptr, armature, "Bone", "Track", value, 40, 5);
  command_buffer.AppendSetBoneConstraintAttribute(
      nullptr, armature, "Bone", "Limit", "influence", value, 60, 6);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 7u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetBoneConstraintInfluence);
  EXPECT_EQ(commands[0].property_name, "Bone");
  EXPECT_EQ(commands[0].secondary_property_name, "CopyLoc");
  EXPECT_EQ(commands[0].scalar_value, 0.75f);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetBonePoseRotation);
  EXPECT_LE(commands[1].rotation_value.AngleTo(
                LN_Rotation::FromEulerXYZ(MT_Vector3(0.1f, 0.2f, 0.3f))),
            0.0001f);
  EXPECT_EQ(commands[1].secondary_int_value, 1);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetBonePoseTransform);
  EXPECT_EQ(commands[2].vector_value.z(), 6.0f);
  EXPECT_LE(commands[2].rotation_value.AngleTo(
                LN_Rotation::FromEulerXYZ(MT_Vector3(0.4f, 0.5f, 0.6f))),
            0.0001f);
  EXPECT_EQ(commands[2].int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(commands[2].secondary_int_value, 1);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SetBonePoseLocation);
  EXPECT_EQ(commands[3].vector_value.z(), 3.0f);
  EXPECT_EQ(commands[3].int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(commands[3].secondary_int_value, 1);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::SetBoneConstraintTarget);
  EXPECT_EQ(commands[4].secondary_property_name, "Track");
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::SetBoneAttribute);
  EXPECT_EQ(commands[5].int_value, 13);
  EXPECT_EQ(commands[5].secondary_int_value, 0);
  EXPECT_EQ(commands[6].type, LN_CommandBuffer::CommandType::SetBoneConstraintAttribute);
  EXPECT_EQ(commands[6].secondary_property_name, "Limit");
  EXPECT_EQ(commands[6].tertiary_property_name, "influence");

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_armature_count, 7u);
}

TEST(LN_CommandBuffer, TypedPhysicsStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendSetCollisionGroup(object, 3, 30, 1);
  command_buffer.AppendApplyImpulse(
      object, MT_Vector3(0.1f, 0.2f, 0.3f), MT_Vector3(4.0f, 5.0f, 6.0f), 10, 2);
  command_buffer.AppendSetCharacterVelocity(
      object, MT_Vector3(7.0f, 8.0f, 9.0f), 0.5f, true, 20, 3);
  command_buffer.AppendVehicleApplyEngineForce(object, 2.0f, 4, 1, 40, 4);
  command_buffer.AppendSetVehicleWheelFriction(object, 1.2f, 2, 0, 50, 5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 5u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyImpulse);
  EXPECT_EQ(commands[0].vector_value.x(), 4.0f);
  EXPECT_EQ(commands[0].secondary_vector_value.y(), 0.2f);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetCharacterVelocity);
  EXPECT_EQ(commands[1].vector_value.z(), 9.0f);
  EXPECT_EQ(commands[1].scalar_value, 0.5f);
  EXPECT_TRUE(commands[1].bool_value);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(commands[2].int_value, 3);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::VehicleApplyEngineForce);
  EXPECT_EQ(commands[3].scalar_value, 2.0f);
  EXPECT_EQ(commands[3].int_value, 4);
  EXPECT_EQ(commands[3].secondary_int_value, 1);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::SetVehicleWheelFriction);
  EXPECT_EQ(commands[4].scalar_value, 1.2f);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_physics_count, 5u);
}

TEST(LN_CommandBuffer, TypedMotionStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);
  KX_GameObject *target = FakeGameObjectPointer(2);

  command_buffer.BeginRecording();
  command_buffer.AppendMoveToward(
      object, MT_Vector3(1.0f, 2.0f, 3.0f), 4.0f, 0.5f, false, true, 0.016f, 30, 1);
  command_buffer.AppendAlignAxisToVector(object, MT_Vector3(0.0f, 1.0f, 0.0f), 2, 0.25f, 10, 2);
  command_buffer.AppendRotateToward(
      object, MT_Vector3(5.0f, 6.0f, 7.0f), 0.5f, 1, 4, false, 20, 3);
  command_buffer.AppendSlowFollow(object, target, 0.75f, 1, 40, 4);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 4u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::AlignAxisToVector);
  EXPECT_EQ(commands[0].vector_value.y(), 1.0f);
  EXPECT_EQ(commands[0].int_value, 2);
  EXPECT_EQ(commands[0].scalar_value, 0.25f);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::RotateToward);
  EXPECT_EQ(commands[1].vector_value.z(), 7.0f);
  EXPECT_EQ(commands[1].int_value, 1);
  EXPECT_EQ(commands[1].secondary_int_value, 4);
  EXPECT_FALSE(commands[1].bool_value);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::MoveToward);
  EXPECT_EQ(commands[2].vector_value.x(), 1.0f);
  EXPECT_EQ(commands[2].secondary_vector_value.x(), 0.5f);
  EXPECT_EQ(commands[2].secondary_vector_value.y(), 0.016f);
  EXPECT_TRUE(commands[2].secondary_bool_value);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SlowFollow);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(commands[3].runtime_tree), target);
  EXPECT_EQ(commands[3].scalar_value, 0.75f);
  EXPECT_EQ(commands[3].int_value, 1);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_motion_count, 4u);
}

TEST(LN_CommandBuffer, FlushPlannerCoalescesLastWriteWinsTreePropertyCommands)
{
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::Command> commands;
  LN_CommandBuffer::Command first;
  first.type = LN_CommandBuffer::CommandType::SetTreeProperty;
  first.property_ref_index = 7;
  first.sort_key = 10;
  commands.push_back(first);

  LN_CommandBuffer::Command second = first;
  second.sort_key = 20;
  commands.push_back(second);

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  command_buffer.Flush();

  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 1u);
}

TEST(LN_CommandBuffer, RuntimeFlushSkipsTinyCoalescibleMinority)
{
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::Command> commands;
  LN_CommandBuffer::Command first;
  first.type = LN_CommandBuffer::CommandType::SetTreeProperty;
  first.property_ref_index = 7;
  first.sort_key = 10;
  commands.push_back(first);

  LN_CommandBuffer::Command second = first;
  second.sort_key = 20;
  commands.push_back(second);

  for (uint64_t index = 0; index < 64; index++) {
    LN_CommandBuffer::Command barrier;
    barrier.type = LN_CommandBuffer::CommandType::SubsystemCommand;
    barrier.subsystem = LN_CommandBuffer::CommandSubsystem::Render;
    barrier.sort_key = 30 + index;
    commands.push_back(barrier);
  }

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  command_buffer.Flush();

  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 0u);
}

TEST(LN_CommandBuffer, RemovedCoalescibleCommandsDoNotForcePlannerFlush)
{
  KX_GameObject *removed_object = FakeGameObjectPointer(1);
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::Command> commands;
  for (uint64_t index = 0; index < 64; index++) {
    LN_CommandBuffer::Command command;
    command.type = LN_CommandBuffer::CommandType::SetGameProperty;
    command.object = removed_object;
    command.property_name = "removed";
    command.sort_key = index;
    commands.push_back(std::move(command));
  }

  for (uint64_t index = 0; index < 2; index++) {
    LN_CommandBuffer::Command command;
    command.type = LN_CommandBuffer::CommandType::SubsystemCommand;
    command.subsystem = LN_CommandBuffer::CommandSubsystem::Render;
    command.sort_key = 100 + index;
    commands.push_back(std::move(command));
  }

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  command_buffer.RemoveCommandsForGameObject(removed_object);
  command_buffer.Flush();

  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();
  EXPECT_EQ(stats.coalesced_command_count, 0u);
  EXPECT_EQ(stats.direct_ordered_stream_count, 1u);
  EXPECT_EQ(stats.direct_ordered_command_count, 2u);
}

TEST(LN_CommandBuffer, FlushPlanningCanBeMeasuredWithoutApplyingCommands)
{
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::Command> commands;
  LN_CommandBuffer::Command first;
  first.type = LN_CommandBuffer::CommandType::SetTreeProperty;
  first.property_ref_index = 7;
  first.sort_key = 10;
  commands.push_back(first);

  LN_CommandBuffer::Command second = first;
  second.sort_key = 20;
  commands.push_back(second);

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  std::vector<LN_CommandBuffer::Command> planned_commands =
      command_buffer.TakeFlushPlannedCommands();

  ASSERT_EQ(planned_commands.size(), 1u);
  EXPECT_EQ(planned_commands.front().type, LN_CommandBuffer::CommandType::SetTreeProperty);
  EXPECT_EQ(planned_commands.front().sort_key, 20u);
  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 1u);
  EXPECT_TRUE(command_buffer.TakeFlushPlannedCommands().empty());
}

TEST(LN_CommandBuffer, FlushPlannerDoesNotCoalesceAcrossBarrierCommands)
{
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::Command> commands;
  LN_CommandBuffer::Command first;
  first.type = LN_CommandBuffer::CommandType::SetTreeProperty;
  first.property_ref_index = 7;
  first.sort_key = 10;
  commands.push_back(first);

  LN_CommandBuffer::Command barrier;
  barrier.type = LN_CommandBuffer::CommandType::Print;
  barrier.property_name = "visible diagnostic barrier";
  barrier.sort_key = 20;
  commands.push_back(barrier);

  LN_CommandBuffer::Command second = first;
  second.sort_key = 30;
  commands.push_back(second);

  command_buffer.AppendLegacyCommandsForTests(std::move(commands));
  command_buffer.Flush();

  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 0u);
}

/** Records the same random step into two buffers: every last-write-wins kind in each stream
 * that records one (transforms, velocities, properties, shape keys, Geometry Nodes, node mute,
 * legacy tree properties), run-ending commands (moves, sounds), repeated targets and equal sort
 * keys. Nothing is applied. */
void RecordRandomFlushStep(LN_CommandBuffer &command_buffer,
                           const uint32_t seed,
                           const bool out_of_order = false)
{
  /* Left to live for the whole run: the buffers keep pointers to its program's property names. */
  static NamedPropertyWriter &random_step_writer = *new NamedPropertyWriter({"a", "b"});
  uint32_t state = seed * 2654435761u + 1u;
  const auto next = [&state](const uint32_t range) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % range;
  };
  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  const char *names[] = {"a", "b"};
  uint64_t sort_key = 1;
  const uint32_t count = next(90);

  command_buffer.BeginRecording();
  for (uint32_t index = 0; index < count; index++) {
    /* Equal keys now and then: the record sequence orders them. Out of order: now and then a
     * lower key after a higher one (a tree spawned after removals, a Run Logic Tree pass). */
    if (out_of_order && next(8) == 0) {
      sort_key -= std::min<uint64_t>(sort_key - 1, 1 + next(6));
    }
    sort_key += next(3);
    KX_GameObject *object = FakeGameObjectPointer(1 + next(3));
    const char *name = names[next(2)];
    value.float_value = float(index);
    switch (next(9)) {
      case 0:
        command_buffer.AppendSetWorldPosition(object, MT_Vector3(float(index), 0, 0), sort_key, 1);
        break;
      case 1:
        command_buffer.AppendSetLinearVelocity(object, MT_Vector3(0, float(index), 0), sort_key, 1);
        break;
      case 2:
        command_buffer.AppendApplyMovement(object, MT_Vector3(0, 0, 1), false, sort_key, 1);
        break;
      case 3:
      case 4:
        random_step_writer.Write(command_buffer, object, name, value, sort_key, 1);
        break;
      case 5:
        command_buffer.AppendSetShapeKeyValue(object, name, float(index), sort_key, 1);
        break;
      case 6:
        command_buffer.AppendSetGeometryNodesInput(
            nullptr, object, "GeometryNodes", name, value, sort_key, 1);
        break;
      case 7:
        command_buffer.AppendSetNodeMute(nullptr, 0, "Material", name, index % 2 == 0, sort_key, 1);
        break;
      case 8:
        command_buffer.AppendPlaySound("Sound", 1.0f, 1.0f, false, sort_key, 1);
        break;
    }
  }
  command_buffer.EndRecording();

  /* Legacy commands (worker trees' merged lists use this stream), after the typed ones. */
  std::vector<LN_CommandBuffer::Command> legacy;
  const uint32_t legacy_count = next(6);
  for (uint32_t index = 0; index < legacy_count; index++) {
    LN_CommandBuffer::Command command;
    command.type = next(3) == 0 ? LN_CommandBuffer::CommandType::Print :
                                  LN_CommandBuffer::CommandType::SetTreeProperty;
    command.property_ref_index = next(2);
    sort_key += next(3);
    command.sort_key = sort_key;
    legacy.push_back(std::move(command));
  }
  command_buffer.AppendLegacyCommandsForTests(std::move(legacy));
}

TEST(LN_CommandBuffer, DirectFlushAppliesAndCoalescesExactlyAsThePlannedFlush)
{
  size_t recorded_count = 0;
  size_t coalesced_count = 0;
  size_t unsorted_count = 0;
  /* Streams out of order too: since 2026-10-31 they flush in place as well (one sort). */
  for (const bool out_of_order : {false, true}) {
    for (uint32_t seed = 0; seed < 2000; seed++) {
      for (const bool coalesce : {false, true}) {
        LN_CommandBuffer planned_buffer;
        RecordRandomFlushStep(planned_buffer, seed, out_of_order);
        const size_t size = planned_buffer.Size();
        std::vector<LN_CommandBuffer::Command> planned =
            coalesce ? planned_buffer.TakeFlushPlannedCommands() :
                       planned_buffer.TakeRecordedCommands();
        if (coalesce) {
          recorded_count += size;
          coalesced_count += size - planned.size();
        }
        std::vector<std::pair<uint64_t, uint64_t>> planned_order;
        for (const LN_CommandBuffer::Command &command : planned) {
          planned_order.emplace_back(command.sort_key, command.record_sequence);
        }

        LN_CommandBuffer direct_buffer;
        RecordRandomFlushStep(direct_buffer, seed, out_of_order);
        unsorted_count += direct_buffer.FlushDecisionForTests().first ? 0 : 1;
        const std::vector<std::pair<uint64_t, uint64_t>> direct_order =
            direct_buffer.DirectFlushOrderForTests(coalesce);

        ASSERT_EQ(direct_order, planned_order)
            << "seed " << seed << " coalesce " << coalesce << " out of order " << out_of_order;
      }
    }
  }
  /* The recordings must give the merge real work: about a tenth of the writes are merged away
   * (9,278 of 93,720 sorted), and most out-of-order recordings must really be out of order. */
  EXPECT_GT(coalesced_count, recorded_count / 20);
  EXPECT_GT(unsorted_count, 2000u);
}

/* The planned flush (typed streams off, tests) keeps Play Action's tree, completion and outputs:
 * it lost them, so the action played and Started / Done never fired (fixed 2026-10-31). */
TEST(LN_CommandBuffer, PlannedFlushKeepsPlayActionOutputs)
{
  LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(0x40));
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendPlayAction(FakeGameObjectPointer(1),
                                  tree,
                                  "Attack",
                                  1.0f,
                                  20.0f,
                                  0,
                                  0,
                                  0.0f,
                                  1.0f,
                                  0,
                                  1.0f,
                                  7,
                                  LN_ACTION_OUTPUT_COMPLETED,
                                  10,
                                  1);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> planned = command_buffer.TakeFlushPlannedCommands();
  ASSERT_EQ(planned.size(), 1u);
  EXPECT_EQ(planned.front().runtime_tree, tree);
  EXPECT_EQ(planned.front().property_ref_index, 7u);
  EXPECT_EQ(planned.front().action_output_mask, LN_ACTION_OUTPUT_COMPLETED);
}

/* Tree batches (LN_TreeBatch.h): a step recorded partly in chunk buffers, appended in order, ends
 * exactly as the same step recorded in one buffer: the same commands in the same flush order
 * with the same merges, values and sizes, also when the buffer's pending move index was made
 * before the chunks came (it then finds their moves). */
TEST(LN_CommandBuffer, ChunksAppendedInOrderEndAsOneRecording)
{
  struct Op {
    uint32_t kind;
    uint32_t object;
    uint32_t name;
    uint64_t sort_key;
    float number;
  };
  const char *names[] = {"a", "b", "c"};
  NamedPropertyWriter writer({"a", "b", "c"});
  const auto record = [&](LN_CommandBuffer &buffer, const Op &op) {
    KX_GameObject *object = FakeGameObjectPointer(1 + op.object);
    LN_Value value;
    value.type = LN_ValueType::Float;
    value.exists = true;
    value.float_value = op.number;
    switch (op.kind) {
      case 0:
        buffer.AppendSetWorldPosition(object, MT_Vector3(op.number, 0, 0), op.sort_key, 1);
        break;
      case 1:
        buffer.AppendSetLinearVelocity(object, MT_Vector3(0, op.number, 0), op.sort_key, 1);
        break;
      case 2:
        buffer.AppendApplyMovement(object, MT_Vector3(0, 0, op.number), true, op.sort_key, 1);
        break;
      case 3:
      case 4:
        writer.Write(buffer, object, names[op.name], value, op.sort_key, 1);
        break;
      case 5:
        buffer.AppendSetShapeKeyValue(object, names[op.name], op.number, op.sort_key, 1);
        break;
      case 6:
        buffer.AppendSetVisibility(object, op.name == 0, false, op.sort_key, 1);
        break;
      case 7:
        buffer.AppendApplyRotation(
            object, MT_Vector3(0, 0, op.number), false, op.sort_key, 1);
        break;
      case 9: {
        LN_PropertyModify modify;
        modify.op = LN_PropertyModifyOp::Add;
        modify.operand = op.number;
        writer.Modify(buffer, object, names[op.name], modify, op.sort_key, 1);
        break;
      }
      case 10:
        buffer.AppendSetWorldOrientation(object, MT_Vector3(0, op.number * 0.01f, 0), op.sort_key, 1);
        break;
      default:
        buffer.AppendPrint(names[op.name], op.sort_key, 1);
        break;
    }
  };
  const auto planned_text = [](std::vector<LN_CommandBuffer::Command> commands) {
    std::string text;
    for (const LN_CommandBuffer::Command &command : commands) {
      text += std::to_string(int(command.type)) + "/" +
              std::to_string(uintptr_t(command.object)) + "/" +
              std::to_string(command.sort_key) + "/" + std::to_string(command.record_sequence) +
              "/" + std::to_string(command.vector_value.x()) + "," +
              std::to_string(command.vector_value.y()) + "," +
              std::to_string(command.vector_value.z()) + "/" + command.property_name + "/" +
              std::to_string(command.property_value.float_value) + "/" +
              std::to_string(command.modify.operand) + "/" +
              std::to_string(command.rotation_value.ToMatrix().getColumn(0).x()) + "," +
              std::to_string(command.rotation_value.ToMatrix().getColumn(0).y()) + ";";
    }
    return text;
  };

  size_t compared = 0;
  for (uint32_t seed = 0; seed < 1500; seed++) {
    uint32_t state = seed * 2654435761u + 7u;
    const auto next = [&state](const uint32_t range) {
      state = state * 1664525u + 1013904223u;
      return (state >> 8) % range;
    };
    std::vector<Op> ops(next(140));
    uint64_t sort_key = 1;
    for (uint32_t index = 0; index < ops.size(); index++) {
      /* Sort keys mostly grow (trees in order), now and then go back (unsorted streams). */
      sort_key = next(12) == 0 ? sort_key - std::min<uint64_t>(sort_key - 1, next(4)) :
                                 sort_key + next(3);
      ops[index] = Op{next(11), next(4), next(3), sort_key, float(index)};
    }
    /* Ops before the chunks, the chunks' ops (in up to 5 chunks), ops after them. */
    const size_t before = ops.empty() ? 0 : next(uint32_t(ops.size()) + 1);
    const size_t after = before + (ops.size() == before ? 0 : next(uint32_t(ops.size() - before) + 1));
    const uint32_t chunk_count = 1 + next(5);
    const bool index_first = next(2) == 0;
    for (const bool coalesce : {false, true}) {
      LN_CommandBuffer one;
      one.BeginRecording();
      for (const Op &op : ops) {
        record(one, op);
      }
      one.EndRecording();

      LN_CommandBuffer joined;
      joined.BeginRecording();
      for (size_t index = 0; index < before; index++) {
        record(joined, ops[index]);
      }
      if (index_first) {
        /* Makes the pending move index now: the chunks' moves must go into it. */
        joined.HasPendingTransforms(FakeGameObjectPointer(1), false);
      }
      std::vector<LN_CommandBuffer> chunks(chunk_count);
      for (uint32_t chunk = 0; chunk < chunk_count; chunk++) {
        chunks[chunk].BeginChunkRecording();
        const size_t first = before + (after - before) * chunk / chunk_count;
        const size_t last = before + (after - before) * (chunk + 1) / chunk_count;
        for (size_t index = first; index < last; index++) {
          record(chunks[chunk], ops[index]);
        }
        chunks[chunk].EndChunkRecording();
        EXPECT_FALSE(chunks[chunk].ChunkReadPendingMove());
      }
      for (LN_CommandBuffer &chunk : chunks) {
        joined.AppendRecordedChunk(chunk);
        EXPECT_EQ(chunk.Size(), 0u);
      }
      for (size_t index = after; index < ops.size(); index++) {
        record(joined, ops[index]);
      }
      /* What a read would find pending: the same objects. */
      for (uint32_t object = 1; object <= 4; object++) {
        ASSERT_EQ(joined.HasPendingTransforms(FakeGameObjectPointer(object), false),
                  one.HasPendingTransforms(FakeGameObjectPointer(object), false))
            << "seed " << seed << " object " << object;
      }
      joined.EndRecording();

      ASSERT_EQ(joined.Size(), one.Size()) << "seed " << seed;
      /* The flush's own choices: direct or planned, merging or not. */
      ASSERT_EQ(joined.FlushDecisionForTests(), one.FlushDecisionForTests()) << "seed " << seed;
      ASSERT_EQ(joined.DirectFlushOrderForTests(coalesce), one.DirectFlushOrderForTests(coalesce))
          << "seed " << seed << " coalesce " << coalesce;
      ASSERT_EQ(planned_text(coalesce ? joined.TakeFlushPlannedCommands() :
                                        joined.TakeRecordedCommands()),
                planned_text(coalesce ? one.TakeFlushPlannedCommands() :
                                        one.TakeRecordedCommands()))
          << "seed " << seed << " coalesce " << coalesce;
      compared++;
    }
  }
  EXPECT_EQ(compared, 3000u);
}

/* A chunk never changes the scene: a read of one of its own pending moves applies nothing (the
 * object stays where it is) and is noted for the manager. */
TEST(LN_CommandBuffer, AChunkReadOfAPendingMoveAppliesNothingAndIsNoted)
{
  KX_GameObject *object = FakeGameObjectPointer(1);
  LN_CommandBuffer chunk;
  chunk.BeginChunkRecording();
  /* (Fake objects have no scene node: reads without parents.) */
  chunk.FlushPendingObjectTransformCommands(object);
  EXPECT_FALSE(chunk.ChunkReadPendingMove());
  chunk.AppendApplyMovement(FakeGameObjectPointer(2), MT_Vector3(1, 0, 0), false, 1, 1);
  chunk.FlushPendingObjectTransformCommands(object);
  EXPECT_FALSE(chunk.ChunkReadPendingMove());
  chunk.AppendApplyMovement(object, MT_Vector3(1, 0, 0), false, 1, 1);
  /* Applying would dereference the fake object: nothing is applied. */
  chunk.FlushPendingObjectTransformCommands(object);
  EXPECT_TRUE(chunk.ChunkReadPendingMove());
  chunk.EndChunkRecording();
  EXPECT_EQ(chunk.Size(), 2u);
  /* A new chunk recording starts over. */
  chunk.BeginChunkRecording();
  EXPECT_FALSE(chunk.ChunkReadPendingMove());
  EXPECT_EQ(chunk.Size(), 0u);
  chunk.EndChunkRecording();
}

TEST(LN_CommandBuffer, ManyPropertyWritesFlushInPlaceAndKeepTheLastValue)
{
  KX_GameObject object;
  NamedPropertyWriter writer({"a", "b"});
  LN_CommandBuffer command_buffer;
  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;

  /* 80 writes of two properties: enough last-write-wins writes for the flush to coalesce, which
   * used to copy every command into a planned list. */
  command_buffer.BeginRecording();
  for (uint32_t index = 0; index < 40; index++) {
    value.float_value = float(index);
    writer.Write(command_buffer, &object, "a", value, 2 * index, 1);
    value.float_value = float(100 + index);
    writer.Write(command_buffer, &object, "b", value, 2 * index + 1, 1);
  }
  command_buffer.EndRecording();
  command_buffer.Flush();

  const LN_CommandBuffer::CommandStreamStats &stats = command_buffer.GetLastCommandStreamStats();
  EXPECT_EQ(stats.direct_ordered_stream_count, 1u);
  EXPECT_EQ(stats.direct_ordered_command_count, 80u);
  EXPECT_EQ(stats.coalesced_command_count, 78u);
  ASSERT_NE(object.GetProperty("a"), nullptr);
  ASSERT_NE(object.GetProperty("b"), nullptr);
  EXPECT_FLOAT_EQ(float(object.GetProperty("a")->GetNumber()), 39.0f);
  EXPECT_FLOAT_EQ(float(object.GetProperty("b")->GetNumber()), 139.0f);
  EXPECT_EQ(command_buffer.Size(), 0u);
}

/** A property of kind `kind` on `object` ("missing" adds none): the kinds a number write meets. */
void AddPropertyOfKind(KX_GameObject &object, const std::string &kind)
{
  EXP_Value *property = nullptr;
  if (kind == "bool") {
    property = new EXP_BoolValue(true);
  }
  else if (kind == "int") {
    property = new EXP_IntValue(-4);
  }
  else if (kind == "float") {
    property = new EXP_FloatValue(9.25f);
  }
  else if (kind == "text") {
    property = new EXP_StringValue("words", "p");
  }
  else if (kind == "vector") {
    property = new EXP_Vector3Value(1.0f, 2.0f, 3.0f);
  }
  if (property != nullptr) {
    object.SetProperty("p", property);
    property->Release();
  }
}

/* Set Game Property Bool/Int/Float records only the number (2026-10-31) and writes it in place
 * when the property holds a number: the property must end exactly as when the write carried the
 * whole value those nodes made, whatever the property held, and the planned flush must see that
 * value. */
TEST(LN_CommandBuffer, NumberPropertyWritesEndAsTheWholeValueWouldEnd)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_GamePropertyRef property_ref;
  property_ref.name = "p";
  property_ref.value_type = LN_ValueType::Float;
  program->AddGamePropertyRef(property_ref);

  std::vector<LN_PropertyNumber> numbers;
  for (const bool value : {false, true}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Bool;
    number.bool_value = value;
    numbers.push_back(number);
  }
  for (const int32_t value : {0, 7, -3, 2147483647}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Int;
    number.int_value = value;
    numbers.push_back(number);
  }
  for (const float value : {0.0f, -0.0f, 2.5f, -0.75f, 1.0e9f, 0.1f}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Float;
    number.float_value = value;
    numbers.push_back(number);
  }

  for (const char *kind : {"missing", "bool", "int", "float", "text", "vector"}) {
    for (const LN_PropertyNumber &number : numbers) {
      KX_GameObject compact_object;
      KX_GameObject whole_object;
      AddPropertyOfKind(compact_object, kind);
      AddPropertyOfKind(whole_object, kind);
      LN_RuntimeTree compact_tree(program, &compact_object, 1, 0);
      LN_RuntimeTree whole_tree(program, &whole_object, 2, 0);

      LN_CommandBuffer compact_buffer;
      compact_buffer.BeginRecording();
      compact_buffer.AppendSetGamePropertyNumberRef(&compact_tree, &compact_object, 0, number, 1, 0);
      compact_buffer.EndRecording();
      compact_buffer.Flush();

      LN_CommandBuffer whole_buffer;
      whole_buffer.BeginRecording();
      whole_buffer.AppendSetGamePropertyRef(
          &whole_tree, &whole_object, 0, number.ToValue(), 1, 0);
      whole_buffer.EndRecording();
      whole_buffer.Flush();

      const std::string where = std::string(kind) + " type " +
                                std::to_string(int(number.type)) + " " +
                                std::to_string(number.int_value) + " " +
                                std::to_string(number.float_value);
      EXP_Value *compact = compact_object.GetProperty("p");
      EXP_Value *whole = whole_object.GetProperty("p");
      ASSERT_EQ(compact != nullptr, whole != nullptr) << where;
      if (compact != nullptr) {
        EXPECT_EQ(compact->GetValueType(), whole->GetValueType()) << where;
        EXPECT_EQ(compact->GetNumber(), whole->GetNumber()) << where;
        EXPECT_EQ(compact->GetText(), whole->GetText()) << where;
      }

      /* The planned flush (and its warnings) see the whole value. */
      LN_CommandBuffer planned_buffer;
      planned_buffer.BeginRecording();
      planned_buffer.AppendSetGamePropertyNumberRef(&compact_tree, &compact_object, 0, number, 1, 0);
      planned_buffer.EndRecording();
      const std::vector<LN_CommandBuffer::Command> planned =
          planned_buffer.TakeFlushPlannedCommands();
      ASSERT_EQ(planned.size(), 1u) << where;
      const LN_Value expected = number.ToValue();
      const LN_Value &value = planned[0].property_value;
      EXPECT_EQ(value.type, expected.type) << where;
      EXPECT_TRUE(value.exists) << where;
      EXPECT_EQ(value.bool_value, expected.bool_value) << where;
      EXPECT_EQ(value.int_value, expected.int_value) << where;
      EXPECT_EQ(value.float_value, expected.float_value) << where;
      EXPECT_TRUE(value.string_value.empty()) << where;
    }
  }
}

/* Plan A7: a number write may carry a checked handle (the property the tree's snapshot found and
 * the target's property list stamp of that moment). Whatever the property held and whatever
 * happened between the capture and the flush, the property must end exactly as when the write is
 * found by name: the same type, number and text. The cases: a fresh handle; an earlier write in
 * the same flush that changes the property (the old property object is kept alive here, so a
 * write through it would show); a widget replacing the property between capture and record; a
 * handle that belongs to another object; no handle. The modes: handles on, by name only, verify
 * (it must find no difference). */
TEST(LN_CommandBuffer, NumberWritesThroughCheckedHandlesEndAsTheNamePathEnds)
{
  struct ModeGuard {
    ~ModeGuard()
    {
      LN_CommandBuffer::SetLeanFlushModeForTests(-1);
    }
  } mode_guard;
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_GamePropertyRef property_ref;
  property_ref.name = "p";
  property_ref.value_type = LN_ValueType::Float;
  program->AddGamePropertyRef(property_ref);

  std::vector<LN_PropertyNumber> numbers;
  for (const bool value : {false, true}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Bool;
    number.bool_value = value;
    numbers.push_back(number);
  }
  for (const int32_t value : {0, -3, 2147483647}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Int;
    number.int_value = value;
    numbers.push_back(number);
  }
  for (const float value : {0.0f, -0.0f, 2.5f, 1.0e9f}) {
    LN_PropertyNumber number;
    number.type = LN_ValueType::Float;
    number.float_value = value;
    numbers.push_back(number);
  }

  enum class Case { Fresh, EarlierWriteChangesIt, WidgetReplacesIt, OtherObjectsHandle, NoHandle };
  const auto describe = [](KX_GameObject &object) {
    EXP_Value *property = object.GetProperty("p");
    if (property == nullptr) {
      return std::string("none");
    }
    return std::to_string(int(property->GetValueType())) + "/" +
           std::to_string(property->GetNumber()) + "/" + property->GetText();
  };
  /* One run of a case: the final description of the object's property. `with_handles`: false
   * records no handle (the reference); the case's other events happen in both. */
  const auto run = [&](const char *kind,
                       const LN_PropertyNumber &number,
                       const Case which,
                       const bool with_handles) {
    KX_GameObject object;
    KX_GameObject other;
    AddPropertyOfKind(object, kind);
    AddPropertyOfKind(other, "float");
    LN_RuntimeTree tree(program, &object, 1, 0);
    EXP_Value *captured = object.GetProperty("p");
    uint64_t stamp = object.GetPropertyMapStamp();
    EXP_Value *kept_alive = nullptr;
    if (which == Case::OtherObjectsHandle) {
      captured = other.GetProperty("p");
      stamp = other.GetPropertyMapStamp();
    }
    if (captured != nullptr) {
      captured->AddRef(); /* the test may hold it past a replacement */
      kept_alive = captured;
    }
    if (which == Case::WidgetReplacesIt) {
      EXP_Value *replacement = new EXP_FloatValue(123.0f);
      object.SetProperty("p", replacement);
      replacement->Release();
    }
    LN_CommandBuffer buffer;
    buffer.BeginRecording();
    if (which == Case::EarlierWriteChangesIt) {
      /* A text write earlier in the flush: it replaces a number property by a text one or
       * converts, either way the property list changes. */
      LN_Value text;
      text.exists = true;
      text.type = LN_ValueType::String;
      text.string_value = "earlier";
      buffer.AppendSetGamePropertyRef(&tree, &object, 0, text, 0, 0);
    }
    buffer.AppendSetGamePropertyNumberRef(&tree,
                                          &object,
                                          0,
                                          number,
                                          1,
                                          1,
                                          with_handles && which != Case::NoHandle ? captured :
                                                                                    nullptr,
                                          with_handles && which != Case::NoHandle ? stamp : 0);
    buffer.EndRecording();
    buffer.Flush();
    const std::string result = describe(object);
    if (kept_alive != nullptr) {
      kept_alive->Release();
    }
    return result;
  };

  size_t compared = 0;
  const uint32_t differences_before = LN_CommandBuffer::LeanFlushVerifyDifferencesForTests();
  for (const char *kind : {"missing", "bool", "int", "float", "text", "vector"}) {
    for (const LN_PropertyNumber &number : numbers) {
      for (const Case which : {Case::Fresh,
                               Case::EarlierWriteChangesIt,
                               Case::WidgetReplacesIt,
                               Case::OtherObjectsHandle,
                               Case::NoHandle})
      {
        LN_CommandBuffer::SetLeanFlushModeForTests(0);
        const std::string reference = run(kind, number, which, false);
        for (const int mode : {0, 1, 2}) {
          LN_CommandBuffer::SetLeanFlushModeForTests(mode);
          EXPECT_EQ(run(kind, number, which, true), reference)
              << kind << " number type " << int(number.type) << " case " << int(which)
              << " mode " << mode;
          compared++;
        }
      }
    }
  }
  EXPECT_EQ(LN_CommandBuffer::LeanFlushVerifyDifferencesForTests(), differences_before);
  EXPECT_EQ(compared, 6u * numbers.size() * 5u * 3u);
}

/* A write's value that is not a number waits in the buffer's side list; many of them in one step,
 * with numbers between, each reach their own property. */
TEST(LN_CommandBuffer, PropertyWritesKeepTheirOwnValuesAmongMany)
{
  KX_GameObject object;
  std::vector<std::string> names;
  for (uint32_t index = 0; index < 200; index++) {
    names.push_back("s" + std::to_string(index));
  }
  NamedPropertyWriter writer(names);
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  for (uint32_t index = 0; index < 200; index++) {
    LN_Value value;
    value.exists = true;
    value.type = LN_ValueType::String;
    value.string_value = "text " + std::to_string(index);
    writer.Write(command_buffer, &object, names[index], value, index, 1);
  }
  command_buffer.EndRecording();
  command_buffer.Flush();
  for (uint32_t index = 0; index < 200; index++) {
    EXP_Value *property = object.GetProperty("s" + std::to_string(index));
    ASSERT_NE(property, nullptr) << index;
    EXPECT_EQ(property->GetText(), "text " + std::to_string(index)) << index;
  }
}

}  // namespace
