/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_CommandDiagnostics.h"
#include "LN_RuntimeSemantics.h"

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
    {LN_CommandBuffer::CommandType::LoadScene,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::SetScene,
     LN_CommandBuffer::CommandSubsystem::GameLifecycle},
    {LN_CommandBuffer::CommandType::SaveGame,
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
};

static_assert(sizeof(kCommandTypeSubsystems) / sizeof(kCommandTypeSubsystems[0]) ==
                  static_cast<size_t>(
                      LN_CommandBuffer::CommandType::EnableDisableModifier) +
                      1,
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
    {LN_CommandBuffer::CommandType::LoadScene,
     LN_OpCode::LoadScene,
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
          LN_CommandBuffer::CommandType::MakeLightUnique),
        LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
    EXPECT_EQ(LN_CommandBuffer::GetCommandThreadPolicy(
                LN_CommandBuffer::CommandType::SetLightShadow),
            LN_CommandBuffer::CommandThreadPolicy::MainThreadFlush);
}

TEST(LN_CommandBuffer, AllowsOwnerlessSceneAndGlobalCommandsToFlush)
{
  using CommandType = LN_CommandBuffer::CommandType;

  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::LoadScene));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetScene));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::LoadBlendFile));
  EXPECT_TRUE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::StopAllSounds));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetWorldPosition));
  EXPECT_FALSE(LN_CommandBuffer::AllowsObjectlessFlush(CommandType::SetGameProperty));
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
      CommandType::SetCollisionGroup,
      CommandType::SetPhysics,
      CommandType::SetDynamics,
      CommandType::RebuildCollisionShape,
      CommandType::SetRigidBodyAttribute,
      CommandType::SetTreeProperty,
      CommandType::SetLogicTreeEnabled,
      CommandType::InstallLogicTree,
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

TEST(LN_CommandBuffer, MergeRecordedCommandListsUsesRuntimeTreeThenSortKeyOrder)
{
  LN_CommandBuffer command_buffer;

  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  LN_CommandBuffer::RecordedCommandList later_tree;
  later_tree.runtime_tree_index = 2;
  later_tree.commands.push_back(GamePropertyCommand("later_high", 30.0f, 30, 3));
  later_tree.commands.push_back(GamePropertyCommand("later_low", 10.0f, 10, 1));
  command_lists.push_back(std::move(later_tree));

  LN_CommandBuffer::RecordedCommandList earlier_tree;
  earlier_tree.runtime_tree_index = 1;
  earlier_tree.commands.push_back(GamePropertyCommand("earlier", 20.0f, 20, 2));
  command_lists.push_back(std::move(earlier_tree));

  LN_CommandBuffer::RecordedCommandList empty_tree;
  empty_tree.runtime_tree_index = 0;
  command_lists.push_back(std::move(empty_tree));

  command_buffer.MergeRecordedCommandLists(std::move(command_lists));

  ASSERT_EQ(command_buffer.Size(), 3u);
  const std::vector<LN_CommandBuffer::Command> &commands =
      command_buffer.GetCommandsForTests();
  EXPECT_EQ(commands[0].property_name, "earlier");
  EXPECT_EQ(commands[1].property_name, "later_low");
  EXPECT_EQ(commands[2].property_name, "later_high");
}

TEST(LN_CommandBuffer, FlushesAggregatedCommandsInSameTick)
{
  LN_CommandBuffer command_buffer;
  LN_CommandBuffer::Command command;
  command.type = LN_CommandBuffer::CommandType::SetShowProfile;
  command.bool_value = false;
  command.sort_key = 1;

  LN_CommandBuffer::RecordedCommandList command_list;
  command_list.runtime_tree_index = 0;
  command_list.commands.push_back(command);
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  command_lists.push_back(std::move(command_list));

  command_buffer.MergeRecordedCommandLists(std::move(command_lists));
  ASSERT_EQ(command_buffer.Size(), 1u);

  command_buffer.Flush();
  EXPECT_EQ(command_buffer.Size(), 0u);
}

TEST(LN_CommandBuffer, MergeRecordedCommandsPreservesOrder)
{
  LN_CommandBuffer command_buffer;
  command_buffer.SetMainThreadId(std::this_thread::get_id());
  command_buffer.BeginRecording();
  command_buffer.AppendSetWorldPosition(
      FakeGameObjectPointer(1), MT_Vector3(1.0f, 0.0f, 0.0f), 10, 0);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> worker_commands;
  worker_commands.push_back(GamePropertyCommand("merged", 7.0f, 20, 1));

  command_buffer.MergeRecordedCommands(std::move(worker_commands));

  ASSERT_EQ(command_buffer.Size(), 2u);
  EXPECT_EQ(command_buffer.GetCommandsForTests().front().type,
            LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(command_buffer.GetCommandsForTests().back().property_name, "merged");
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
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetActiveCamera),
            LN_CommandBuffer::CommandClass::ObjectServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::ApplyImpulse),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::SetCharacterVelocity),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandClass(CommandType::VehicleApplyEngineForce),
            LN_CommandBuffer::CommandClass::PhysicsServiceMutation);

  EXPECT_EQ(LN_CommandBuffer::GetCommandBarrier(CommandType::ApplyForce),
            LN_CommandBuffer::CommandBarrier::RelativeMutation);
  EXPECT_EQ(LN_CommandBuffer::GetCommandBarrier(CommandType::RemoveObject),
            LN_CommandBuffer::CommandBarrier::ObjectLifecycle);
  EXPECT_EQ(LN_CommandBuffer::GetCommandBarrier(CommandType::SendEvent),
            LN_CommandBuffer::CommandBarrier::EventMessage);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::SetWorldPosition),
            LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::SetMaterialParameter),
            LN_CommandBuffer::CoalescingPolicy::LastWriteWinsWithinBarrierSegment);
  EXPECT_EQ(LN_CommandBuffer::GetCoalescingPolicy(CommandType::PlaySound),
            LN_CommandBuffer::CoalescingPolicy::Forbidden);
}

TEST(LN_CommandBuffer, TypedStreamsMaterializeInDeterministicRecordOrder)
{
  LN_CommandBuffer command_buffer;
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
  command_buffer.AppendSetGameProperty(FakeGameObjectPointer(1), "Speed", property_value, 20, 3);
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

TEST(LN_CommandBuffer, OutOfOrderTypedRelativeVectorFlushAvoidsDirectOrderedFastPath)
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

  command_buffer.Flush();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 2u);
  EXPECT_EQ(stats.direct_ordered_stream_count, 0u);
  EXPECT_EQ(stats.direct_ordered_command_count, 0u);
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

  command_buffer.BeginRecording();
  command_buffer.AppendAddObject(owner, "Cube", 0.0f, false, 30, 1);
  command_buffer.AppendSetParent(owner, "Parent", true, false, 10, 2);
  command_buffer.AppendReplaceMesh(owner, target, 20, 3);
  command_buffer.AppendAddObjectFromRef(runtime_tree, owner, object_ref, 2.5f, true, 6, 35, 7);
  command_buffer.AppendCopyProperty(owner, target, "speed", 40, 4);
  command_buffer.AppendRemoveObject(owner, 50, 5);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeFlushPlannedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 6u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetParent);
  EXPECT_EQ(commands[0].property_name, "Parent");
  EXPECT_TRUE(commands[0].bool_value);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::ReplaceMesh);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(commands[1].runtime_tree), target);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::AddObject);
  EXPECT_EQ(commands[2].property_name, "Cube");
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
  command_buffer.AppendSetGamepadVibration(1, 0.25f, 0.75f, 500, 40, 4);
  command_buffer.AppendSetFullscreen(true, 50, 5);
  command_buffer.AppendSetVSync(2, 60, 6);
  command_buffer.AppendSetShowFramerate(true, 70, 7);
  command_buffer.AppendSetShowProfile(false, 80, 8);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 8u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetCursorVisibility);
  EXPECT_FALSE(commands[0].bool_value);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SetCursorPosition);
  EXPECT_EQ(commands[1].int_value, 100);
  EXPECT_EQ(commands[1].secondary_int_value, 200);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetWindowSize);
  EXPECT_EQ(commands[2].int_value, 1280);
  EXPECT_EQ(commands[2].secondary_int_value, 720);
  EXPECT_EQ(commands[3].type, LN_CommandBuffer::CommandType::SetGamepadVibration);
  EXPECT_EQ(commands[3].int_value, 1);
  EXPECT_EQ(commands[3].scalar_value, 0.25f);
  EXPECT_EQ(commands[3].vector_value.x(), 0.75f);
  EXPECT_EQ(commands[3].property_ref_index, 500u);
  EXPECT_EQ(commands[4].type, LN_CommandBuffer::CommandType::SetFullscreen);
  EXPECT_TRUE(commands[4].bool_value);
  EXPECT_EQ(commands[5].type, LN_CommandBuffer::CommandType::SetVSync);
  EXPECT_EQ(commands[5].int_value, 2);
  EXPECT_EQ(commands[6].type, LN_CommandBuffer::CommandType::SetShowFramerate);
  EXPECT_TRUE(commands[6].bool_value);
  EXPECT_EQ(commands[7].type, LN_CommandBuffer::CommandType::SetShowProfile);
  EXPECT_FALSE(commands[7].bool_value);

  EXPECT_EQ(stats.legacy_command_count, 0u);
  EXPECT_EQ(stats.typed_runtime_service_count, 8u);
}

TEST(LN_CommandBuffer, TypedAnimationStreamsMaterializeInDeterministicOrder)
{
  LN_CommandBuffer command_buffer;
  KX_GameObject *object = FakeGameObjectPointer(1);

  command_buffer.BeginRecording();
  command_buffer.AppendStopAction(object, 2, 30, 1);
  command_buffer.AppendPlayAction(object,
                                  "Walk",
                                  1.0f,
                                  24.0f,
                                  3,
                                  4,
                                  0.25f,
                                  0.8f,
                                  0x0010203u,
                                  1.5f,
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
  EXPECT_EQ(commands[1].vector_value.y(), 0.2f);
  EXPECT_EQ(commands[1].secondary_int_value, 1);
  EXPECT_EQ(commands[2].type, LN_CommandBuffer::CommandType::SetBonePoseTransform);
  EXPECT_EQ(commands[2].vector_value.z(), 6.0f);
  EXPECT_EQ(commands[2].secondary_vector_value.y(), 0.5f);
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
  command_buffer.AppendRotateToward(object, MT_Vector3(5.0f, 6.0f, 7.0f), 0.5f, 1, 4, 20, 3);
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

TEST(LN_CommandBuffer, TypedCommandStreamsCanBeDisabledForLegacyABTesting)
{
  LN_CommandBuffer command_buffer;
  command_buffer.SetTypedCommandStreamsEnabled(false);
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
  command_buffer.AppendSetGameProperty(FakeGameObjectPointer(1), "Speed", property_value, 20, 3);
  command_buffer.AppendSendEvent("Started", property_value, FakeGameObjectPointer(1), nullptr, 40, 4);
  command_buffer.AppendSetVisibility(FakeGameObjectPointer(1), true, false, 45, 8);
  command_buffer.AppendSetGravity(
      FakeGameObjectPointer(1), MT_Vector3(0.0f, 0.0f, -9.8f), 46, 14);
  command_buffer.AppendApplyImpulse(FakeGameObjectPointer(1),
                                    MT_Vector3(0.1f, 0.2f, 0.3f),
                                    MT_Vector3(4.0f, 5.0f, 6.0f),
                                    47,
                                    9);
  command_buffer.AppendStopAllSounds(48, 14);
  command_buffer.AppendSetTimeScale(FakeGameObjectPointer(1), 0.5f, 49, 15);
  command_buffer.AppendPlaySound("Ping", 0.5f, 1.25f, true, 50, 5);
  command_buffer.AppendSetVSync(1, 55, 10);
  command_buffer.AppendStopAction(FakeGameObjectPointer(1), 2, 60, 11);
  LN_Value material_value;
  material_value.type = LN_ValueType::DatablockRef;
  material_value.exists = true;
  material_value.reference_name = "Mat";
  command_buffer.AppendSetMaterialNodeSocketValue(
      nullptr, material_value, "Node", "Input", property_value, 65, 12);
  command_buffer.AppendSetBonePoseLocation(
      FakeGameObjectPointer(1), "Bone", MT_Vector3(1.0f, 2.0f, 3.0f), 70, 13);
  command_buffer.EndRecording();

  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  const LN_CommandBuffer::CommandStreamStats &stats =
      command_buffer.GetLastCommandStreamStats();

  ASSERT_EQ(commands.size(), 16u);
  EXPECT_EQ(stats.legacy_command_count, 16u);
  EXPECT_EQ(stats.typed_transform_count, 0u);
  EXPECT_EQ(stats.typed_velocity_count, 0u);
  EXPECT_EQ(stats.typed_relative_vector_count, 0u);
  EXPECT_EQ(stats.typed_motion_count, 0u);
  EXPECT_EQ(stats.typed_property_count, 0u);
  EXPECT_EQ(stats.typed_event_count, 0u);
  EXPECT_EQ(stats.typed_audio_count, 0u);
  EXPECT_EQ(stats.typed_object_service_count, 0u);
  EXPECT_EQ(stats.typed_runtime_service_count, 0u);
  EXPECT_EQ(stats.typed_animation_count, 0u);
  EXPECT_EQ(stats.typed_armature_count, 0u);
  EXPECT_EQ(stats.typed_material_count, 0u);
  EXPECT_EQ(stats.typed_physics_count, 0u);
  EXPECT_EQ(commands[7].type, LN_CommandBuffer::CommandType::SetGravity);
  EXPECT_EQ(commands[7].vector_value.z(), -9.8f);
  EXPECT_EQ(commands[8].type, LN_CommandBuffer::CommandType::ApplyImpulse);
  EXPECT_EQ(commands[9].type, LN_CommandBuffer::CommandType::StopAllSounds);
  EXPECT_EQ(commands[10].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(commands[10].scalar_value, 0.5f);
  EXPECT_EQ(commands[11].type, LN_CommandBuffer::CommandType::PlaySound);
  EXPECT_EQ(commands[11].vector_value.x(), 0.5f);
  EXPECT_EQ(commands[11].vector_value.y(), 1.25f);
  EXPECT_TRUE(commands[11].bool_value);
  EXPECT_EQ(commands[12].type, LN_CommandBuffer::CommandType::SetVSync);
  EXPECT_EQ(commands[12].int_value, 1);
  EXPECT_EQ(commands[13].type, LN_CommandBuffer::CommandType::StopAction);
  EXPECT_EQ(commands[13].int_value, 2);
  EXPECT_EQ(commands[14].type, LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue);
  EXPECT_EQ(commands[14].property_name, "Mat");
  EXPECT_EQ(commands[14].tertiary_property_name, "Input");
  EXPECT_EQ(commands[15].type, LN_CommandBuffer::CommandType::SetBonePoseLocation);
  EXPECT_EQ(commands[15].property_name, "Bone");
}

TEST(LN_CommandBuffer, TypedCommandStreamDisableBypassesFlushCoalescing)
{
  LN_CommandBuffer command_buffer;
  command_buffer.SetTypedCommandStreamsEnabled(false);

  std::vector<LN_CommandBuffer::Command> commands;
  LN_CommandBuffer::Command first;
  first.type = LN_CommandBuffer::CommandType::SetTreeProperty;
  first.property_ref_index = 7;
  first.sort_key = 10;
  commands.push_back(first);

  LN_CommandBuffer::Command second = first;
  second.sort_key = 20;
  commands.push_back(second);

  command_buffer.MergeRecordedCommands(std::move(commands));
  const std::vector<LN_CommandBuffer::Command> planned =
      command_buffer.TakeFlushPlannedCommands();

  EXPECT_EQ(planned.size(), 2u);
  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 0u);
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

  command_buffer.MergeRecordedCommands(std::move(commands));
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

  command_buffer.MergeRecordedCommands(std::move(commands));
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

  command_buffer.MergeRecordedCommands(std::move(commands));
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

  command_buffer.MergeRecordedCommands(std::move(commands));
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

  command_buffer.MergeRecordedCommands(std::move(commands));
  command_buffer.Flush();

  EXPECT_EQ(command_buffer.GetLastCommandStreamStats().coalesced_command_count, 0u);
}

}  // namespace
