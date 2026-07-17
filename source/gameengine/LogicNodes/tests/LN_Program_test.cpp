/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>

#include "LN_CommandBuffer.h"
#include "LN_DenseIds.h"
#include "LN_Program.h"
#include "LN_RuntimeSemantics.h"
#include "LN_RuntimeTree.h"
#include "LN_Types.h"

namespace {

bool HasResource(const LN_SchedulerSummary &summary, const uint32_t resource_access)
{
  return (summary.resource_access & resource_access) == resource_access;
}

bool HasDependencyMask(const uint32_t mask, const uint32_t bits)
{
  return (mask & bits) == bits;
}

bool HasRead(const LN_RuntimeExpressionSemantics &semantics, const uint32_t read)
{
  return (semantics.reads & read) == read;
}

bool HasWrite(const LN_RuntimeExpressionSemantics &semantics, const uint32_t write)
{
  return (semantics.writes & write) == write;
}

bool HasFallback(const LN_RuntimeExpressionSemantics &semantics, const uint32_t fallback)
{
  return (semantics.fallback_requirements & fallback) == fallback;
}

bool HasFallback(const LN_RuntimeInstructionSemantics &semantics, const uint32_t fallback)
{
  return (semantics.fallback_requirements & fallback) == fallback;
}

bool ContainsError(const std::vector<std::string> &errors, const std::string &needle)
{
  for (const std::string &error : errors) {
    if (error.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

KX_GameObject *FakeGameObjectPointer(uint32_t value)
{
  return reinterpret_cast<KX_GameObject *>(uintptr_t(value));
}

LN_Value MakeFloatValue(const float value)
{
  LN_Value result;
  result.type = LN_ValueType::Float;
  result.exists = true;
  result.float_value = value;
  return result;
}

LN_Value MakeBoolValue(const bool value)
{
  LN_Value result;
  result.type = LN_ValueType::Bool;
  result.exists = true;
  result.bool_value = value;
  return result;
}

LN_Value MakeIntValue(const int32_t value)
{
  LN_Value result;
  result.type = LN_ValueType::Int;
  result.exists = true;
  result.int_value = value;
  return result;
}

LN_Value MakeVectorValue(const MT_Vector3 &value)
{
  LN_Value result;
  result.type = LN_ValueType::Vector;
  result.exists = true;
  result.vector_value = value;
  return result;
}

LN_Value MakeColorValue(const MT_Vector4 &value)
{
  LN_Value result;
  result.type = LN_ValueType::Color;
  result.exists = true;
  result.color_value = value;
  return result;
}

LN_MainThreadOnlyReason ReasonForOpcode(const LN_OpCode opcode)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = opcode;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  EXPECT_FALSE(program.IsParallelEligible());
  return program.GetMainThreadOnlyReason();
}

LN_MainThreadOnlyReason ReasonForValueExpression(const LN_ValueExpressionKind kind)
{
  LN_Program program;
  LN_ValueExpression expression;
  expression.kind = kind;
  program.AddValueExpression(expression);
  EXPECT_FALSE(program.IsParallelEligible());
  return program.GetMainThreadOnlyReason();
}

}  // namespace

TEST(LN_Program, IsParallelEligibleByDefault)
{
  LN_Program program;
  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::None);
  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::Pure);
  EXPECT_EQ(summary.required_phase, LN_SchedulerRequiredPhase::None);
  EXPECT_EQ(summary.resource_access, LN_SCHEDULER_RESOURCE_NONE);
  EXPECT_EQ(summary.estimated_work_class, LN_EstimatedWorkClass::Trivial);
  EXPECT_FALSE(summary.emits_commands);
  EXPECT_FALSE(summary.uses_jolt_queries_or_contacts);
  EXPECT_TRUE(summary.reads_snapshot_only);
  EXPECT_TRUE(summary.worker_lane_eligible);
  EXPECT_EQ(summary.main_thread_only_reason, LN_MainThreadOnlyReason::None);
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForSupportedHotOps)
{
  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));

  const LN_ExecBlockProgram &ir = program->GetExecBlockProgram(LN_Event::OnInit);
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.program_version, LN_PROGRAM_VERSION);
  EXPECT_EQ(ir.schema_version, LN_PROGRAM_SCHEMA_VERSION);
  EXPECT_EQ(ir.cache_generation, LN_EXEC_BLOCK_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.feature_mask, LN_EXEC_BLOCK_IR_FEATURE_MASK);
  EXPECT_EQ(ir.direct_instruction_count, 1u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.blocks.size(), 1u);
  ASSERT_EQ(ir.ops.size(), 1u);
  EXPECT_EQ(ir.ops[0].kind, LN_ExecOpKind::VectorCommand);
  EXPECT_EQ(ir.ops[0].opcode, LN_OpCode::SetTransformVector);
  EXPECT_TRUE(program->ValidateExecBlockIR());
  EXPECT_TRUE(program->ValidateInstructionPayloads());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForAdditionalCommandFamilies)
{
  LN_Program program;

  LN_Instruction set_object_color;
  set_object_color.opcode = LN_OpCode::SetObjectColor;
  set_object_color.color_value = MT_Vector4(0.1f, 0.2f, 0.3f, 1.0f);
  program.AddInstruction(LN_Event::OnFixedUpdate, set_object_color);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_value = true;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_visibility);

  const LN_OpCode physics_opcodes[] = {
      LN_OpCode::ApplyMovement,
      LN_OpCode::ApplyRotation,
      LN_OpCode::ApplyForce,
      LN_OpCode::ApplyTorque,
      LN_OpCode::ApplyImpulse,
      LN_OpCode::Translate,
      LN_OpCode::MoveToward,
      LN_OpCode::SlowFollow,
      LN_OpCode::AlignAxisToVector,
      LN_OpCode::RotateToward,
      LN_OpCode::SetCollisionGroup,
      LN_OpCode::SetPhysics,
      LN_OpCode::SetDynamics,
      LN_OpCode::RebuildCollisionShape,
      LN_OpCode::SetRigidBodyAttribute,
      LN_OpCode::SetGravity,
      LN_OpCode::CharacterJump,
      LN_OpCode::SetCharacterGravity,
      LN_OpCode::SetCharacterJumpSpeed,
      LN_OpCode::SetCharacterMaxJumps,
      LN_OpCode::SetCharacterWalkDirection,
      LN_OpCode::SetCharacterVelocity,
      LN_OpCode::VehicleControl,
      LN_OpCode::VehicleApplyEngineForce,
      LN_OpCode::VehicleApplyBraking,
      LN_OpCode::VehicleApplySteering,
      LN_OpCode::SetVehicleSuspensionCompression,
      LN_OpCode::SetVehicleSuspensionStiffness,
      LN_OpCode::SetVehicleSuspensionDamping,
      LN_OpCode::SetVehicleWheelFriction,
  };
  for (const LN_OpCode opcode : physics_opcodes) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  LN_Instruction set_camera_fov;
  set_camera_fov.opcode = LN_OpCode::SetCameraFov;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_camera_fov);

  LN_Instruction set_camera_ortho_scale;
  set_camera_ortho_scale.opcode = LN_OpCode::SetCameraOrthoScale;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_camera_ortho_scale);

  LN_Instruction set_active_camera;
  set_active_camera.opcode = LN_OpCode::SetActiveCamera;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_active_camera);

  LN_Instruction make_light_unique;
  make_light_unique.opcode = LN_OpCode::MakeLightUnique;
  program.AddInstruction(LN_Event::OnFixedUpdate, make_light_unique);

  LN_Instruction set_light_color;
  set_light_color.opcode = LN_OpCode::SetLightColor;
  set_light_color.color_value = MT_Vector4(0.7f, 0.6f, 0.5f, 1.0f);
  program.AddInstruction(LN_Event::OnFixedUpdate, set_light_color);

  LN_Instruction set_light_power;
  set_light_power.opcode = LN_OpCode::SetLightPower;
  set_light_power.vector_value = MT_Vector3(3.5f, 0.0f, 0.0f);
  program.AddInstruction(LN_Event::OnFixedUpdate, set_light_power);

  LN_Instruction set_light_shadow;
  set_light_shadow.opcode = LN_OpCode::SetLightShadow;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_light_shadow);

  LN_Instruction set_window_size;
  set_window_size.opcode = LN_OpCode::SetWindowSize;
  set_window_size.int_value = 1024;
  set_window_size.secondary_int_value = 768;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_window_size);

  LN_Instruction set_fullscreen;
  set_fullscreen.opcode = LN_OpCode::SetFullscreen;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_fullscreen);

  LN_Instruction set_vsync;
  set_vsync.opcode = LN_OpCode::SetVSync;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_vsync);

  LN_Instruction set_show_framerate;
  set_show_framerate.opcode = LN_OpCode::SetShowFramerate;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_show_framerate);

  LN_Instruction set_show_profile;
  set_show_profile.opcode = LN_OpCode::SetShowProfile;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_show_profile);

  LN_Instruction set_cursor_visibility;
  set_cursor_visibility.opcode = LN_OpCode::SetCursorVisibility;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_cursor_visibility);

  LN_Instruction set_cursor_position;
  set_cursor_position.opcode = LN_OpCode::SetCursorPosition;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_cursor_position);

  LN_Instruction set_gamepad_vibration;
  set_gamepad_vibration.opcode = LN_OpCode::SetGamepadVibration;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_gamepad_vibration);

  const LN_OpCode audio_opcodes[] = {
      LN_OpCode::StopAllSounds,
      LN_OpCode::PlaySound,
      LN_OpCode::PlaySound3D,
      LN_OpCode::PauseSound,
      LN_OpCode::ResumeSound,
      LN_OpCode::StopSound,
  };
  for (const LN_OpCode opcode : audio_opcodes) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  LN_Instruction play_action;
  play_action.opcode = LN_OpCode::PlayAction;
  program.AddInstruction(LN_Event::OnFixedUpdate, play_action);

  LN_Instruction stop_action;
  stop_action.opcode = LN_OpCode::StopAction;
  program.AddInstruction(LN_Event::OnFixedUpdate, stop_action);

  LN_Instruction set_action_frame;
  set_action_frame.opcode = LN_OpCode::SetActionFrame;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_action_frame);

  const LN_OpCode bone_opcodes[] = {
      LN_OpCode::SetBonePoseLocation,
      LN_OpCode::SetBonePoseRotation,
      LN_OpCode::SetBonePoseScale,
      LN_OpCode::SetBonePoseTransform,
      LN_OpCode::SetBoneAttribute,
      LN_OpCode::SetBoneConstraintInfluence,
      LN_OpCode::SetBoneConstraintTarget,
      LN_OpCode::SetBoneConstraintAttribute,
  };
  for (const LN_OpCode opcode : bone_opcodes) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  const LN_OpCode material_opcodes[] = {
      LN_OpCode::SetMaterialSlot,
      LN_OpCode::SetMaterialParameter,
      LN_OpCode::SetMaterialNodeSocketValue,
      LN_OpCode::SetGeometryNodesInput,
      LN_OpCode::SetGeometryNodeSocketValue,
      LN_OpCode::SetCompositorNodeSocketValue,
      LN_OpCode::MakeNodeTreeUnique,
      LN_OpCode::EnableDisableModifier,
      LN_OpCode::AssignGeometryNodesModifier,
  };
  for (const LN_OpCode opcode : material_opcodes) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  program.AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_Instruction quit_game;
  quit_game.opcode = LN_OpCode::QuitGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, quit_game);

  LN_Instruction restart_game;
  restart_game.opcode = LN_OpCode::RestartGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, restart_game);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.vector_value = MT_Vector3(0.5f, 0.0f, 0.0f);
  program.AddInstruction(LN_Event::OnFixedUpdate, set_time_scale);

  LN_Instruction load_blend_file;
  load_blend_file.opcode = LN_OpCode::LoadBlendFile;
  program.AddInstruction(LN_Event::OnFixedUpdate, load_blend_file);

  LN_Instruction save_game;
  save_game.opcode = LN_OpCode::SaveGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, save_game);

  LN_Instruction load_game;
  load_game.opcode = LN_OpCode::LoadGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, load_game);

  LN_Instruction load_scene;
  load_scene.opcode = LN_OpCode::LoadScene;
  program.AddInstruction(LN_Event::OnFixedUpdate, load_scene);

  LN_Instruction set_scene;
  set_scene.opcode = LN_OpCode::SetScene;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_scene);

  LN_Instruction remove_parent;
  remove_parent.opcode = LN_OpCode::RemoveParent;
  program.AddInstruction(LN_Event::OnFixedUpdate, remove_parent);

  LN_Instruction remove_object;
  remove_object.opcode = LN_OpCode::RemoveObject;
  program.AddInstruction(LN_Event::OnFixedUpdate, remove_object);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  const uint32_t expected_direct_instruction_count = uint32_t(
      2u + std::size(physics_opcodes) + 3u + 4u + 8u + std::size(audio_opcodes) + 3u +
      std::size(bone_opcodes) + std::size(material_opcodes) + 4u + 3u + 2u + 2u);

  EXPECT_EQ(ir.direct_instruction_count, expected_direct_instruction_count);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.blocks.size(), 1u);
  ASSERT_EQ(ir.ops.size(), expected_direct_instruction_count);

  uint32_t op_index = 0;
  const auto expect_kind = [&](const LN_ExecOpKind kind) {
    EXPECT_EQ(ir.ops[op_index].kind, kind);
    op_index++;
  };
  const auto expect_kind_range = [&](const uint32_t count, const LN_ExecOpKind kind) {
    for (uint32_t range_index = 0; range_index < count; range_index++) {
      expect_kind(kind);
    }
  };

  expect_kind(LN_ExecOpKind::ObjectColorCommand);
  expect_kind(LN_ExecOpKind::ObjectStateCommand);
  expect_kind_range(uint32_t(std::size(physics_opcodes)), LN_ExecOpKind::PhysicsCommand);
  expect_kind_range(3u, LN_ExecOpKind::CameraCommand);
  expect_kind_range(4u, LN_ExecOpKind::LightCommand);
  expect_kind_range(8u, LN_ExecOpKind::WindowCommand);
  expect_kind_range(uint32_t(std::size(audio_opcodes)), LN_ExecOpKind::AudioCommand);
  expect_kind_range(3u, LN_ExecOpKind::ActionCommand);
  expect_kind_range(uint32_t(std::size(bone_opcodes)), LN_ExecOpKind::BoneCommand);
  expect_kind_range(uint32_t(std::size(material_opcodes)), LN_ExecOpKind::MaterialCommand);
  expect_kind_range(4u, LN_ExecOpKind::GlobalCommand);
  expect_kind_range(3u, LN_ExecOpKind::FileCommand);
  expect_kind_range(2u, LN_ExecOpKind::SceneCommand);
  expect_kind_range(2u, LN_ExecOpKind::LifecycleCommand);
  EXPECT_EQ(op_index, expected_direct_instruction_count);
  EXPECT_TRUE(program.ValidateExecBlockIR());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForTimeFlowControl)
{
  LN_Program program;
  const uint32_t timer_state_index = program.AddTimerState();
  const LN_OpCode time_flow_opcodes[] = {
      LN_OpCode::ArmTimer,
      LN_OpCode::ArmDelay,
      LN_OpCode::UpdatePulsify,
      LN_OpCode::UpdateBarrier,
  };

  for (const LN_OpCode opcode : time_flow_opcodes) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.int_value = int32_t(timer_state_index);
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 4u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.ops.size(), 4u);
  for (const LN_ExecOp &op : ir.ops) {
    EXPECT_EQ(op.kind, LN_ExecOpKind::TimeFlowControl);
  }
  EXPECT_TRUE(program.ValidateExecBlockIR());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForObjectReferenceCommands)
{
  LN_Program program;
  LN_Instruction replace_mesh;
  replace_mesh.opcode = LN_OpCode::ReplaceMesh;
  program.AddInstruction(LN_Event::OnFixedUpdate, replace_mesh);

  LN_Instruction set_parent;
  set_parent.opcode = LN_OpCode::SetParent;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_parent);

  LN_Instruction copy_property;
  copy_property.opcode = LN_OpCode::CopyProperty;
  program.AddInstruction(LN_Event::OnFixedUpdate, copy_property);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 3u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.ops.size(), 3u);
  EXPECT_EQ(ir.ops[0].kind, LN_ExecOpKind::ObjectReferenceCommand);
  EXPECT_EQ(ir.ops[1].kind, LN_ExecOpKind::ObjectReferenceCommand);
  EXPECT_EQ(ir.ops[2].kind, LN_ExecOpKind::ObjectReferenceCommand);
  EXPECT_TRUE(program.ValidateExecBlockIR());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForInputMotionCommands)
{
  LN_Program program;
  LN_ValueExpression head_expression;
  head_expression.kind = LN_ValueExpressionKind::Constant;
  const uint32_t head_expr = program.AddValueExpression(head_expression);

  LN_VectorExpression vector_expression;
  vector_expression.kind = LN_VectorExpressionKind::Constant;
  const uint32_t vector_expr = program.AddVectorExpression(vector_expression);

  LN_FloatExpression float_expression;
  float_expression.kind = LN_FloatExpressionKind::Constant;
  const uint32_t float_expr = program.AddFloatExpression(float_expression);

  LN_Instruction gamepad_look;
  gamepad_look.opcode = LN_OpCode::GamepadLook;
  gamepad_look.property_ref_index = head_expr;
  gamepad_look.vector_expr_index = vector_expr;
  gamepad_look.secondary_vector_expr_index = vector_expr;
  gamepad_look.color_expr_index = vector_expr;
  gamepad_look.int_expr_index = vector_expr;
  gamepad_look.float_expr_index = float_expr;
  gamepad_look.string_expr_index = float_expr;
  program.AddInstruction(LN_Event::OnFixedUpdate, gamepad_look);

  LN_Instruction mouse_look;
  mouse_look.opcode = LN_OpCode::MouseLook;
  mouse_look.property_ref_index = head_expr;
  mouse_look.secondary_vector_expr_index = vector_expr;
  mouse_look.color_expr_index = vector_expr;
  mouse_look.int_expr_index = vector_expr;
  mouse_look.float_expr_index = float_expr;
  mouse_look.string_expr_index = float_expr;
  program.AddInstruction(LN_Event::OnFixedUpdate, mouse_look);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 2u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.ops.size(), 2u);
  EXPECT_EQ(ir.ops[0].kind, LN_ExecOpKind::InputMotionCommand);
  EXPECT_EQ(ir.ops[1].kind, LN_ExecOpKind::InputMotionCommand);
  EXPECT_TRUE(program.ValidateExecBlockIR());
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, DependencySummaryTracksInputMotionInstructionInputReads)
{
  LN_Program program;

  LN_ValueExpression object_expression;
  object_expression.kind = LN_ValueExpressionKind::Constant;
  const uint32_t object_expr = program.AddValueExpression(object_expression);

  LN_VectorExpression vector_expression;
  vector_expression.kind = LN_VectorExpressionKind::Constant;
  const uint32_t vector_expr = program.AddVectorExpression(vector_expression);

  LN_FloatExpression float_expression;
  float_expression.kind = LN_FloatExpressionKind::Constant;
  const uint32_t float_expr = program.AddFloatExpression(float_expression);

  LN_Instruction gamepad_look;
  gamepad_look.opcode = LN_OpCode::GamepadLook;
  gamepad_look.value_expr_index = object_expr;
  gamepad_look.property_ref_index = object_expr;
  gamepad_look.vector_expr_index = vector_expr;
  gamepad_look.secondary_vector_expr_index = vector_expr;
  gamepad_look.color_expr_index = vector_expr;
  gamepad_look.int_expr_index = vector_expr;
  gamepad_look.float_expr_index = float_expr;
  gamepad_look.string_expr_index = float_expr;
  program.AddInstruction(LN_Event::OnFixedUpdate, gamepad_look);

  LN_Instruction mouse_look;
  mouse_look.opcode = LN_OpCode::MouseLook;
  mouse_look.value_expr_index = object_expr;
  mouse_look.property_ref_index = object_expr;
  mouse_look.secondary_vector_expr_index = vector_expr;
  mouse_look.color_expr_index = vector_expr;
  mouse_look.int_expr_index = vector_expr;
  mouse_look.float_expr_index = float_expr;
  mouse_look.string_expr_index = float_expr;
  program.AddInstruction(LN_Event::OnFixedUpdate, mouse_look);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_TRUE(HasDependencyMask(summary.input_channels, LN_DEP_INPUT_MOUSE));
  EXPECT_TRUE(HasDependencyMask(summary.input_channels, LN_DEP_INPUT_GAMEPAD_AXIS));
  EXPECT_TRUE(HasDependencyMask(program.GetSchedulerSummary().resource_access,
                                LN_SCHEDULER_RESOURCE_INPUT));
  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::InputDeviceState);
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForDeferredTreeControlCommands)
{
  LN_Program program;

  LN_Instruction set_enabled;
  set_enabled.opcode = LN_OpCode::SetLogicTreeEnabled;
  set_enabled.bool_value = true;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_enabled);

  LN_Instruction install_tree;
  install_tree.opcode = LN_OpCode::InstallLogicTree;
  install_tree.bool_value = true;
  program.AddInstruction(LN_Event::OnFixedUpdate, install_tree);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 2u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.ops.size(), 2u);
  EXPECT_EQ(ir.ops[0].kind, LN_ExecOpKind::TreeControlCommand);
  EXPECT_EQ(ir.ops[0].opcode, LN_OpCode::SetLogicTreeEnabled);
  EXPECT_EQ(ir.ops[1].kind, LN_ExecOpKind::TreeControlCommand);
  EXPECT_EQ(ir.ops[1].opcode, LN_OpCode::InstallLogicTree);
  EXPECT_TRUE(program.ValidateExecBlockIR());
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, ExecBlockIRBuildsDirectBlocksForApplyVectorVariants)
{
  LN_Program program;
  LN_Instruction apply_transform_vector;
  apply_transform_vector.opcode = LN_OpCode::ApplyTransformVector;
  apply_transform_vector.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
  apply_transform_vector.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Movement);
  program.AddInstruction(LN_Event::OnFixedUpdate, apply_transform_vector);

  LN_Instruction apply_physics_vector;
  apply_physics_vector.opcode = LN_OpCode::ApplyPhysicsVector;
  apply_physics_vector.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
  apply_physics_vector.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Force);
  program.AddInstruction(LN_Event::OnFixedUpdate, apply_physics_vector);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 2u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  ASSERT_EQ(ir.ops.size(), 2u);
  EXPECT_EQ(ir.ops[0].kind, LN_ExecOpKind::VectorCommand);
  EXPECT_EQ(ir.ops[0].opcode, LN_OpCode::ApplyTransformVector);
  EXPECT_EQ(ir.ops[1].kind, LN_ExecOpKind::VectorCommand);
  EXPECT_EQ(ir.ops[1].opcode, LN_OpCode::ApplyPhysicsVector);
  EXPECT_TRUE(program.ValidateExecBlockIR());
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, ExecBlockIRRejectsMalformedPropertyOpcodesThroughPayloadValidation)
{
  LN_Program program;
  LN_Instruction set_property;
  set_property.opcode = LN_OpCode::SetTreeProperty;
  program.AddInstruction(LN_Event::OnFixedUpdate, set_property);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.direct_instruction_count, 1u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  EXPECT_EQ(ir.fallback_block_count, 0u);
  EXPECT_TRUE(program.ValidateExecBlockIR());
  EXPECT_FALSE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, ExecBlockIRKeepsLoopFramedInstructionsInDirectLoopBlocks)
{
  LN_Program program;
  LN_LoopFrame frame;
  const uint32_t loop_frame_index = program.AddLoopFrame(frame);

  LN_Instruction nop;
  nop.opcode = LN_OpCode::Nop;
  nop.loop_frame_index = loop_frame_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, nop);

  const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(ir.valid);
  ASSERT_EQ(ir.blocks.size(), 1u);
  EXPECT_EQ(ir.blocks[0].loop_frame_index, loop_frame_index);
  EXPECT_EQ(ir.direct_instruction_count, 1u);
  EXPECT_EQ(ir.fallback_instruction_count, 0u);
  EXPECT_EQ(ir.fallback_block_count, 0u);
  ASSERT_EQ(ir.ops.size(), 1u);
  EXPECT_EQ(ir.ops[0].opcode, LN_OpCode::Nop);
  EXPECT_TRUE(program.ValidateExecBlockIR());
}

TEST(LN_Program, RegisterExpressionIRLowersScalarVectorMathAndTracksBatches)
{
  LN_Program program;
  LN_FloatExpression one;
  one.kind = LN_FloatExpressionKind::Constant;
  one.float_value = 1.0f;
  const uint32_t one_index = program.AddFloatExpression(one);

  uint32_t constant_indices[4];
  for (int index = 0; index < 4; index++) {
    LN_FloatExpression constant;
    constant.kind = LN_FloatExpressionKind::Constant;
    constant.float_value = float(index + 2);
    constant_indices[index] = program.AddFloatExpression(constant);
  }

  LN_FloatExpression frame_delta;
  frame_delta.kind = LN_FloatExpressionKind::SnapshotFrameDelta;
  const uint32_t frame_delta_index = program.AddFloatExpression(frame_delta);

  for (int index = 0; index < 4; index++) {
    LN_FloatExpression add;
    add.kind = LN_FloatExpressionKind::Add;
    add.input0 = frame_delta_index;
    add.input1 = constant_indices[index];
    program.AddFloatExpression(add);
  }

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = one_index;
  vector.input1 = constant_indices[0];
  vector.input2 = one_index;
  program.AddVectorExpression(vector);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.program_version, LN_PROGRAM_VERSION);
  EXPECT_EQ(ir.schema_version, LN_PROGRAM_SCHEMA_VERSION);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.feature_mask, LN_REGISTER_EXPRESSION_IR_FEATURE_MASK);
  EXPECT_GE(ir.float_register_count, 6u);
  EXPECT_GE(ir.vector_register_count, 1u);
  EXPECT_GE(ir.scalar_op_count, 7u);
  EXPECT_GE(ir.simd_candidate_batch_count, 1u);
  EXPECT_GE(ir.simd_candidate_lane_count, 4u);
  EXPECT_FALSE(ir.lifetimes.empty());
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, RegisterExpressionIRLowersColorComponentAndAlphaCombine)
{
  LN_Program program;

  LN_FloatExpression time_scale;
  time_scale.kind = LN_FloatExpressionKind::SnapshotTimeScale;
  const uint32_t time_scale_index = program.AddFloatExpression(time_scale);

  LN_FloatExpression green;
  green.kind = LN_FloatExpressionKind::Constant;
  green.float_value = 0.4f;
  const uint32_t green_index = program.AddFloatExpression(green);

  LN_FloatExpression blue;
  blue.kind = LN_FloatExpressionKind::Constant;
  blue.float_value = 0.6f;
  const uint32_t blue_index = program.AddFloatExpression(blue);

  LN_FloatExpression alpha;
  alpha.kind = LN_FloatExpressionKind::Constant;
  alpha.float_value = 0.8f;
  const uint32_t alpha_index = program.AddFloatExpression(alpha);

  LN_ColorExpression base_color;
  base_color.kind = LN_ColorExpressionKind::Combine;
  base_color.input0 = time_scale_index;
  base_color.input1 = green_index;
  base_color.input2 = blue_index;
  base_color.input3 = alpha_index;
  const uint32_t base_color_index = program.AddColorExpression(base_color);

  uint32_t color_component_indices[4];
  for (uint32_t component = 0; component < 4; component++) {
    LN_FloatExpression component_float;
    component_float.kind = LN_FloatExpressionKind::ColorComponent;
    component_float.input0 = base_color_index;
    component_float.component_index = uint8_t(component);
    color_component_indices[component] = program.AddFloatExpression(component_float);
  }

  LN_ColorExpression combined_color;
  combined_color.kind = LN_ColorExpressionKind::Combine;
  combined_color.input0 = color_component_indices[0];
  combined_color.input1 = color_component_indices[1];
  combined_color.input2 = color_component_indices[2];
  combined_color.input3 = color_component_indices[3];
  program.AddColorExpression(combined_color);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_GE(ir.color_register_count, 2u);
  EXPECT_GE(ir.float_register_count, 4u);

  bool has_color_component = false;
  bool has_alpha_combine = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::FloatColorComponent) {
      has_color_component = true;
    }
    if (op.kind == LN_RegisterExpressionOpKind::ColorCombine &&
        op.input3.register_index != LN_INVALID_INDEX)
    {
      has_alpha_combine = true;
    }
  }
  EXPECT_TRUE(has_color_component);
  EXPECT_TRUE(has_alpha_combine);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersCombineExpressionsWithDefaultComponents)
{
  LN_Program program;

  LN_FloatExpression dynamic_component;
  dynamic_component.kind = LN_FloatExpressionKind::SnapshotFrameDelta;
  const uint32_t dynamic_component_index = program.AddFloatExpression(dynamic_component);

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.vector_value = MT_Vector3(0.1f, 0.2f, 0.3f);
  vector.input2 = dynamic_component_index;
  program.AddVectorExpression(vector);

  LN_ColorExpression color;
  color.kind = LN_ColorExpressionKind::Combine;
  color.color_value = MT_Vector4(0.4f, 0.5f, 0.6f, 0.7f);
  color.input1 = dynamic_component_index;
  program.AddColorExpression(color);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_GE(ir.vector_register_count, 1u);
  EXPECT_GE(ir.color_register_count, 1u);

  bool has_partial_vector_combine = false;
  bool has_partial_color_combine = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorCombine) {
      has_partial_vector_combine = op.input0.register_index == LN_INVALID_INDEX &&
                                   op.input1.register_index == LN_INVALID_INDEX &&
                                   op.input2.register_index != LN_INVALID_INDEX;
    }
    if (op.kind == LN_RegisterExpressionOpKind::ColorCombine) {
      has_partial_color_combine = op.input0.register_index == LN_INVALID_INDEX &&
                                  op.input1.register_index != LN_INVALID_INDEX &&
                                  op.input2.register_index == LN_INVALID_INDEX &&
                                  op.input3.register_index == LN_INVALID_INDEX;
    }
  }
  EXPECT_TRUE(has_partial_vector_combine);
  EXPECT_TRUE(has_partial_color_combine);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfSnapshotColors)
{
  LN_Program program;

  LN_ColorExpression object_color;
  object_color.kind = LN_ColorExpressionKind::SnapshotObjectColor;
  program.AddColorExpression(object_color);

  LN_ColorExpression light_color;
  light_color.kind = LN_ColorExpressionKind::SnapshotLightColor;
  program.AddColorExpression(light_color);

  LN_ColorExpression targeted_color;
  targeted_color.kind = LN_ColorExpressionKind::SnapshotObjectColor;
  targeted_color.input0 = 0;
  program.AddColorExpression(targeted_color);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.color_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::ColorSnapshotRead) {
      snapshot_read_count++;
    }
  }
  EXPECT_EQ(snapshot_read_count, 2u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfSnapshotVectors)
{
  LN_Program program;

  const LN_VectorExpressionKind snapshot_kinds[] = {
      LN_VectorExpressionKind::SnapshotWorldPosition,
      LN_VectorExpressionKind::SnapshotLocalPosition,
      LN_VectorExpressionKind::SnapshotWorldOrientation,
      LN_VectorExpressionKind::SnapshotLocalOrientation,
      LN_VectorExpressionKind::SnapshotWorldScale,
      LN_VectorExpressionKind::SnapshotLocalScale,
      LN_VectorExpressionKind::SnapshotLinearVelocity,
      LN_VectorExpressionKind::SnapshotLocalLinearVelocity,
      LN_VectorExpressionKind::SnapshotAngularVelocity,
      LN_VectorExpressionKind::SnapshotLocalAngularVelocity,
      LN_VectorExpressionKind::SnapshotGravity,
      LN_VectorExpressionKind::SnapshotCharacterGravity,
      LN_VectorExpressionKind::SnapshotCharacterWalkDirection,
      LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection,
  };
  for (const LN_VectorExpressionKind kind : snapshot_kinds) {
    LN_VectorExpression expression;
    expression.kind = kind;
    program.AddVectorExpression(expression);
  }

  LN_VectorExpression targeted_position;
  targeted_position.kind = LN_VectorExpressionKind::SnapshotWorldPosition;
  targeted_position.input0 = 0;
  program.AddVectorExpression(targeted_position);

  LN_VectorExpression targeted_character_gravity;
  targeted_character_gravity.kind = LN_VectorExpressionKind::SnapshotCharacterGravity;
  targeted_character_gravity.input0 = 0;
  program.AddVectorExpression(targeted_character_gravity);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.vector_register_count, 14u);
  EXPECT_EQ(ir.fallback_expression_count, 2u);

  uint32_t snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorSnapshotRead) {
      snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Vector);
    }
  }
  EXPECT_EQ(snapshot_read_count, 14u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersCursorInputVectors)
{
  LN_Program program;

  LN_VectorExpression cursor_position;
  cursor_position.kind = LN_VectorExpressionKind::CursorPosition;
  program.AddVectorExpression(cursor_position);

  LN_VectorExpression cursor_movement;
  cursor_movement.kind = LN_VectorExpressionKind::CursorMovement;
  program.AddVectorExpression(cursor_movement);

  LN_VectorExpression window_resolution;
  window_resolution.kind = LN_VectorExpressionKind::WindowResolution;
  program.AddVectorExpression(window_resolution);

  LN_IntExpression gamepad_index;
  gamepad_index.kind = LN_IntExpressionKind::Constant;
  gamepad_index.int_value = 0;
  const uint32_t gamepad_index_expr = program.AddIntExpression(gamepad_index);

  LN_FloatExpression threshold;
  threshold.kind = LN_FloatExpressionKind::Constant;
  threshold.float_value = 0.1f;
  const uint32_t threshold_expr = program.AddFloatExpression(threshold);

  LN_FloatExpression sensitivity;
  sensitivity.kind = LN_FloatExpressionKind::Constant;
  sensitivity.float_value = 1.0f;
  const uint32_t sensitivity_expr = program.AddFloatExpression(sensitivity);

  LN_VectorExpression gamepad_stick;
  gamepad_stick.kind = LN_VectorExpressionKind::GamepadStick;
  gamepad_stick.input0 = gamepad_index_expr;
  gamepad_stick.input1 = threshold_expr;
  gamepad_stick.float_expr_index = sensitivity_expr;
  program.AddVectorExpression(gamepad_stick);

  LN_VectorExpression dynamic_gamepad_stick;
  dynamic_gamepad_stick.kind = LN_VectorExpressionKind::GamepadStick;
  program.AddVectorExpression(dynamic_gamepad_stick);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.vector_register_count, 4u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t input_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorInputRead) {
      input_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Vector);
    }
  }
  EXPECT_EQ(input_read_count, 4u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersVectorResize)
{
  LN_Program program;

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Constant;
  vector.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_index = program.AddVectorExpression(vector);

  LN_VectorExpression resize_2d;
  resize_2d.kind = LN_VectorExpressionKind::Resize;
  resize_2d.input0 = vector_index;
  resize_2d.float_value = 2.0f;
  program.AddVectorExpression(resize_2d);

  LN_VectorExpression resize_3d;
  resize_3d.kind = LN_VectorExpressionKind::Resize;
  resize_3d.input0 = vector_index;
  resize_3d.float_value = 3.0f;
  program.AddVectorExpression(resize_3d);

  LN_VectorExpression axis_vector;
  axis_vector.kind = LN_VectorExpressionKind::AxisVector;
  axis_vector.input0 = 0;
  program.AddVectorExpression(axis_vector);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.vector_register_count, 3u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t resize_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorResize) {
      resize_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Vector);
    }
  }
  EXPECT_EQ(resize_count, 2u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfAxisVector)
{
  LN_Program program;

  LN_VectorExpression positive_x;
  positive_x.kind = LN_VectorExpressionKind::AxisVector;
  positive_x.property_ref_index = 0;
  program.AddVectorExpression(positive_x);

  LN_VectorExpression negative_z;
  negative_z.kind = LN_VectorExpressionKind::AxisVector;
  negative_z.property_ref_index = 5;
  program.AddVectorExpression(negative_z);

  LN_VectorExpression targeted_axis;
  targeted_axis.kind = LN_VectorExpressionKind::AxisVector;
  targeted_axis.input0 = 0;
  program.AddVectorExpression(targeted_axis);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.vector_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorSnapshotRead) {
      snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Vector);
    }
  }
  EXPECT_EQ(snapshot_read_count, 2u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersVectorRotateAroundAxis)
{
  LN_Program program;

  LN_VectorExpression origin;
  origin.kind = LN_VectorExpressionKind::Constant;
  origin.vector_value = MT_Vector3(1.0f, 0.0f, 0.0f);
  const uint32_t origin_index = program.AddVectorExpression(origin);

  LN_VectorExpression pivot;
  pivot.kind = LN_VectorExpressionKind::Constant;
  pivot.vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  const uint32_t pivot_index = program.AddVectorExpression(pivot);

  LN_FloatExpression angle;
  angle.kind = LN_FloatExpressionKind::Constant;
  angle.float_value = 1.57079632679f;
  const uint32_t angle_index = program.AddFloatExpression(angle);

  LN_VectorExpression fixed_axis_rotate;
  fixed_axis_rotate.kind = LN_VectorExpressionKind::RotateAroundAxis;
  fixed_axis_rotate.input0 = origin_index;
  fixed_axis_rotate.input1 = pivot_index;
  fixed_axis_rotate.float_expr_index = angle_index;
  fixed_axis_rotate.property_ref_index = 6;
  program.AddVectorExpression(fixed_axis_rotate);

  LN_VectorExpression custom_axis;
  custom_axis.kind = LN_VectorExpressionKind::Constant;
  custom_axis.vector_value = MT_Vector3(0.0f, 0.0f, 1.0f);
  const uint32_t custom_axis_index = program.AddVectorExpression(custom_axis);

  LN_VectorExpression custom_axis_rotate;
  custom_axis_rotate.kind = LN_VectorExpressionKind::RotateAroundAxis;
  custom_axis_rotate.input0 = origin_index;
  custom_axis_rotate.input1 = pivot_index;
  custom_axis_rotate.input2 = custom_axis_index;
  custom_axis_rotate.float_expr_index = angle_index;
  custom_axis_rotate.property_ref_index = 8;
  program.AddVectorExpression(custom_axis_rotate);

  LN_VectorExpression euler;
  euler.kind = LN_VectorExpressionKind::Constant;
  euler.vector_value = MT_Vector3(0.0f, 0.0f, 1.57079632679f);
  const uint32_t euler_index = program.AddVectorExpression(euler);

  LN_VectorExpression euler_rotate;
  euler_rotate.kind = LN_VectorExpressionKind::RotateAroundAxis;
  euler_rotate.input0 = origin_index;
  euler_rotate.input1 = pivot_index;
  euler_rotate.input2 = euler_index;
  euler_rotate.property_ref_index = 12;
  program.AddVectorExpression(euler_rotate);

  LN_VectorExpression dynamic_axis;
  dynamic_axis.kind = LN_VectorExpressionKind::AxisVector;
  dynamic_axis.input0 = 0;
  const uint32_t dynamic_axis_index = program.AddVectorExpression(dynamic_axis);

  LN_VectorExpression fallback_rotate;
  fallback_rotate.kind = LN_VectorExpressionKind::RotateAroundAxis;
  fallback_rotate.input0 = origin_index;
  fallback_rotate.input1 = pivot_index;
  fallback_rotate.input2 = dynamic_axis_index;
  fallback_rotate.float_expr_index = angle_index;
  fallback_rotate.property_ref_index = 8;
  program.AddVectorExpression(fallback_rotate);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.float_register_count, 1u);
  EXPECT_EQ(ir.vector_register_count, 7u);
  EXPECT_EQ(ir.fallback_expression_count, 2u);

  uint32_t rotate_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::VectorRotateAroundAxis) {
      rotate_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Vector);
    }
  }
  EXPECT_EQ(rotate_count, 3u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfLightPowerSnapshotFloat)
{
  LN_Program program;

  LN_FloatExpression light_power;
  light_power.kind = LN_FloatExpressionKind::SnapshotLightPower;
  program.AddFloatExpression(light_power);

  LN_FloatExpression targeted_light_power;
  targeted_light_power.kind = LN_FloatExpressionKind::SnapshotLightPower;
  targeted_light_power.input0 = 0;
  program.AddFloatExpression(targeted_light_power);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.float_register_count, 1u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::FloatSnapshotRead) {
      snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Float);
    }
  }
  EXPECT_EQ(snapshot_read_count, 1u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersGamepadInputFloat)
{
  LN_Program program;

  LN_IntExpression gamepad_index;
  gamepad_index.kind = LN_IntExpressionKind::Constant;
  gamepad_index.int_value = 0;
  const uint32_t gamepad_index_expr = program.AddIntExpression(gamepad_index);

  LN_FloatExpression button_strength;
  button_strength.kind = LN_FloatExpressionKind::GamepadButtonStrength;
  button_strength.input0 = gamepad_index_expr;
  button_strength.input2 = 0;
  program.AddFloatExpression(button_strength);

  LN_FloatExpression dynamic_button_strength;
  dynamic_button_strength.kind = LN_FloatExpressionKind::GamepadButtonStrength;
  dynamic_button_strength.input2 = 0;
  program.AddFloatExpression(dynamic_button_strength);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.int_register_count, 1u);
  EXPECT_EQ(ir.float_register_count, 1u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t input_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::FloatInputRead) {
      input_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Float);
    }
  }
  EXPECT_EQ(input_read_count, 1u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfVisibilitySnapshotBool)
{
  LN_Program program;

  LN_BoolExpression visibility;
  visibility.kind = LN_BoolExpressionKind::SnapshotVisibility;
  program.AddBoolExpression(visibility);

  LN_BoolExpression character_on_ground;
  character_on_ground.kind = LN_BoolExpressionKind::SnapshotCharacterOnGround;
  program.AddBoolExpression(character_on_ground);

  LN_BoolExpression targeted_visibility;
  targeted_visibility.kind = LN_BoolExpressionKind::SnapshotVisibility;
  targeted_visibility.input0 = 0;
  program.AddBoolExpression(targeted_visibility);

  LN_BoolExpression targeted_character_on_ground;
  targeted_character_on_ground.kind = LN_BoolExpressionKind::SnapshotCharacterOnGround;
  targeted_character_on_ground.input0 = 0;
  program.AddBoolExpression(targeted_character_on_ground);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.bool_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 2u);

  uint32_t snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::BoolSnapshotRead) {
      snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Bool);
    }
  }
  EXPECT_EQ(snapshot_read_count, 2u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSnapshotGamePropertyValues)
{
  LN_Program program;

  LN_GamePropertyRef bool_property_ref;
  bool_property_ref.name = "visible";
  bool_property_ref.value_type = LN_ValueType::Bool;
  const uint32_t bool_property_ref_index = program.AddGamePropertyRef(bool_property_ref);

  LN_GamePropertyRef int_property_ref;
  int_property_ref.name = "score";
  int_property_ref.value_type = LN_ValueType::Int;
  const uint32_t int_property_ref_index = program.AddGamePropertyRef(int_property_ref);

  LN_GamePropertyRef float_property_ref;
  float_property_ref.name = "speed";
  float_property_ref.value_type = LN_ValueType::Float;
  const uint32_t float_property_ref_index = program.AddGamePropertyRef(float_property_ref);

  LN_GamePropertyRef string_property_ref;
  string_property_ref.name = "label";
  string_property_ref.value_type = LN_ValueType::String;
  const uint32_t string_property_ref_index = program.AddGamePropertyRef(string_property_ref);

  LN_BoolExpression exists;
  exists.kind = LN_BoolExpressionKind::SnapshotGamePropertyExists;
  exists.property_ref_index = bool_property_ref_index;
  program.AddBoolExpression(exists);

  LN_BoolExpression bool_property;
  bool_property.kind = LN_BoolExpressionKind::SnapshotGameProperty;
  bool_property.property_ref_index = bool_property_ref_index;
  program.AddBoolExpression(bool_property);

  LN_IntExpression int_property;
  int_property.kind = LN_IntExpressionKind::SnapshotGameProperty;
  int_property.property_ref_index = int_property_ref_index;
  program.AddIntExpression(int_property);

  LN_FloatExpression float_property;
  float_property.kind = LN_FloatExpressionKind::SnapshotGameProperty;
  float_property.property_ref_index = float_property_ref_index;
  program.AddFloatExpression(float_property);

  LN_StringExpression string_property;
  string_property.kind = LN_StringExpressionKind::SnapshotGameProperty;
  string_property.property_ref_index = string_property_ref_index;
  program.AddStringExpression(string_property);

  LN_FloatExpression invalid_property;
  invalid_property.kind = LN_FloatExpressionKind::SnapshotGameProperty;
  program.AddFloatExpression(invalid_property);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.bool_register_count, 2u);
  EXPECT_EQ(ir.int_register_count, 1u);
  EXPECT_EQ(ir.float_register_count, 1u);
  EXPECT_EQ(ir.string_register_count, 1u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t bool_snapshot_read_count = 0;
  uint32_t int_snapshot_read_count = 0;
  uint32_t float_snapshot_read_count = 0;
  uint32_t string_snapshot_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::BoolSnapshotRead) {
      bool_snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Bool);
    }
    if (op.kind == LN_RegisterExpressionOpKind::IntSnapshotRead) {
      int_snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Int);
    }
    if (op.kind == LN_RegisterExpressionOpKind::FloatSnapshotRead) {
      float_snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Float);
    }
    if (op.kind == LN_RegisterExpressionOpKind::StringSnapshotRead) {
      string_snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::String);
    }
  }
  EXPECT_EQ(bool_snapshot_read_count, 2u);
  EXPECT_EQ(int_snapshot_read_count, 1u);
  EXPECT_EQ(float_snapshot_read_count, 1u);
  EXPECT_EQ(string_snapshot_read_count, 1u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersStatelessInputBools)
{
  LN_Program program;

  LN_IntExpression gamepad_index;
  gamepad_index.kind = LN_IntExpressionKind::Constant;
  gamepad_index.int_value = 0;
  const uint32_t gamepad_index_expr = program.AddIntExpression(gamepad_index);

  LN_IntExpression gamepad_status;
  gamepad_status.kind = LN_IntExpressionKind::Constant;
  gamepad_status.int_value = 1;
  const uint32_t gamepad_status_expr = program.AddIntExpression(gamepad_status);

  const LN_BoolExpressionKind input_kinds[] = {
      LN_BoolExpressionKind::InputStatus,
      LN_BoolExpressionKind::KeyboardActive,
      LN_BoolExpressionKind::WindowFullscreen,
      LN_BoolExpressionKind::MouseMoved,
      LN_BoolExpressionKind::MouseWheelMoved,
      LN_BoolExpressionKind::KeyLoggerPressed,
  };
  for (const LN_BoolExpressionKind kind : input_kinds) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = 1;
    expression.secondary_int_value = 1;
    program.AddBoolExpression(expression);
  }

  LN_BoolExpression stateful_mouse_moved;
  stateful_mouse_moved.kind = LN_BoolExpressionKind::MouseMoved;
  stateful_mouse_moved.bool_value = true;
  program.AddBoolExpression(stateful_mouse_moved);

  LN_BoolExpression gamepad_active;
  gamepad_active.kind = LN_BoolExpressionKind::GamepadActive;
  gamepad_active.input0 = gamepad_index_expr;
  program.AddBoolExpression(gamepad_active);

  LN_BoolExpression gamepad_button;
  gamepad_button.kind = LN_BoolExpressionKind::GamepadButton;
  gamepad_button.input0 = gamepad_index_expr;
  gamepad_button.int_expr_index = gamepad_status_expr;
  gamepad_button.int_value = 0;
  program.AddBoolExpression(gamepad_button);

  LN_BoolExpression dynamic_gamepad_active;
  dynamic_gamepad_active.kind = LN_BoolExpressionKind::GamepadActive;
  program.AddBoolExpression(dynamic_gamepad_active);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.int_register_count, 2u);
  EXPECT_EQ(ir.bool_register_count, 8u);
  EXPECT_EQ(ir.fallback_expression_count, 2u);

  uint32_t input_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::BoolInputRead) {
      input_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Bool);
    }
  }
  EXPECT_EQ(input_read_count, 8u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersAdvancedScalarMathAndThresholds)
{
  LN_Program program;

  LN_FloatExpression time_scale;
  time_scale.kind = LN_FloatExpressionKind::SnapshotTimeScale;
  const uint32_t time_scale_index = program.AddFloatExpression(time_scale);

  LN_FloatExpression two;
  two.kind = LN_FloatExpressionKind::Constant;
  two.float_value = 2.0f;
  const uint32_t two_index = program.AddFloatExpression(two);

  LN_FloatExpression lower;
  lower.kind = LN_FloatExpressionKind::Constant;
  lower.float_value = 0.25f;
  const uint32_t lower_index = program.AddFloatExpression(lower);

  LN_FloatExpression upper;
  upper.kind = LN_FloatExpressionKind::Constant;
  upper.float_value = 4.0f;
  const uint32_t upper_index = program.AddFloatExpression(upper);

  LN_StringExpression formula_string;
  formula_string.kind = LN_StringExpressionKind::Constant;
  formula_string.string_value = "a * b + 1";
  const uint32_t formula_string_index = program.AddStringExpression(formula_string);

  const LN_FloatExpressionKind scalar_kinds[] = {
      LN_FloatExpressionKind::Power,
      LN_FloatExpressionKind::Sign,
      LN_FloatExpressionKind::Round,
      LN_FloatExpressionKind::Floor,
      LN_FloatExpressionKind::Ceil,
      LN_FloatExpressionKind::Truncate,
      LN_FloatExpressionKind::Fraction,
      LN_FloatExpressionKind::Modulo,
      LN_FloatExpressionKind::Sine,
      LN_FloatExpressionKind::Cosine,
      LN_FloatExpressionKind::Radians,
      LN_FloatExpressionKind::Degrees,
      LN_FloatExpressionKind::Threshold,
      LN_FloatExpressionKind::RangedThreshold,
      LN_FloatExpressionKind::Select,
      LN_FloatExpressionKind::Formula,
  };
  for (const LN_FloatExpressionKind kind : scalar_kinds) {
    LN_FloatExpression expression;
    expression.kind = kind;
    expression.input0 = time_scale_index;
    expression.input1 = kind == LN_FloatExpressionKind::RangedThreshold ? lower_index : two_index;
    expression.input2 = upper_index;
    expression.bool_value = true;
    expression.threshold_operation = LN_ThresholdOperation::Greater;
    expression.range_operation = LN_RangeOperation::Inside;
    if (kind == LN_FloatExpressionKind::Select) {
      expression.bool_expr_index = LN_INVALID_INDEX;
    }
    if (kind == LN_FloatExpressionKind::Formula) {
      expression.string_expr_index = formula_string_index;
    }
    program.AddFloatExpression(expression);
  }

  LN_FloatExpression dynamic_formula;
  dynamic_formula.kind = LN_FloatExpressionKind::Formula;
  dynamic_formula.input0 = time_scale_index;
  dynamic_formula.input1 = two_index;
  program.AddFloatExpression(dynamic_formula);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  const LN_RegisterExpressionOpKind expected_kinds[] = {
      LN_RegisterExpressionOpKind::FloatPower,
      LN_RegisterExpressionOpKind::FloatSign,
      LN_RegisterExpressionOpKind::FloatRound,
      LN_RegisterExpressionOpKind::FloatFloor,
      LN_RegisterExpressionOpKind::FloatCeil,
      LN_RegisterExpressionOpKind::FloatTruncate,
      LN_RegisterExpressionOpKind::FloatFraction,
      LN_RegisterExpressionOpKind::FloatModulo,
      LN_RegisterExpressionOpKind::FloatSine,
      LN_RegisterExpressionOpKind::FloatCosine,
      LN_RegisterExpressionOpKind::FloatRadians,
      LN_RegisterExpressionOpKind::FloatDegrees,
      LN_RegisterExpressionOpKind::FloatThreshold,
      LN_RegisterExpressionOpKind::FloatRangedThreshold,
      LN_RegisterExpressionOpKind::FloatSelect,
      LN_RegisterExpressionOpKind::FloatFormula,
  };
  for (const LN_RegisterExpressionOpKind expected_kind : expected_kinds) {
    bool found = false;
    for (const LN_RegisterExpressionOp &op : ir.ops) {
      if (op.kind == expected_kind) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersConstantStringPredicatesAndFoldedCounts)
{
  LN_Program program;

  LN_StringExpression haystack;
  haystack.kind = LN_StringExpressionKind::Constant;
  haystack.string_value = "logic nodes logic";
  const uint32_t haystack_index = program.AddStringExpression(haystack);

  LN_StringExpression logic;
  logic.kind = LN_StringExpressionKind::Constant;
  logic.string_value = "logic";
  const uint32_t logic_index = program.AddStringExpression(logic);

  LN_StringExpression nodes;
  nodes.kind = LN_StringExpressionKind::Constant;
  nodes.string_value = "nodes";
  const uint32_t nodes_index = program.AddStringExpression(nodes);

  LN_StringExpression dynamic_string;
  dynamic_string.kind = LN_StringExpressionKind::RuntimeTreeProperty;
  const uint32_t dynamic_string_index = program.AddStringExpression(dynamic_string);

  const LN_BoolExpressionKind predicate_kinds[] = {
      LN_BoolExpressionKind::StringContains,
      LN_BoolExpressionKind::StringStartsWith,
      LN_BoolExpressionKind::StringEndsWith,
  };
  for (const LN_BoolExpressionKind kind : predicate_kinds) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.input0 = haystack_index;
    expression.input1 = kind == LN_BoolExpressionKind::StringEndsWith ? logic_index : nodes_index;
    program.AddBoolExpression(expression);
  }

  LN_BoolExpression dynamic_predicate;
  dynamic_predicate.kind = LN_BoolExpressionKind::StringContains;
  dynamic_predicate.input0 = dynamic_string_index;
  dynamic_predicate.input1 = logic_index;
  program.AddBoolExpression(dynamic_predicate);

  LN_IntExpression count;
  count.kind = LN_IntExpressionKind::StringCount;
  count.input0 = haystack_index;
  count.input1 = logic_index;
  program.AddIntExpression(count);

  LN_IntExpression dynamic_count;
  dynamic_count.kind = LN_IntExpressionKind::StringCount;
  dynamic_count.input0 = haystack_index;
  dynamic_count.input1 = dynamic_string_index;
  program.AddIntExpression(dynamic_count);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.bool_register_count, 3u);
  EXPECT_EQ(ir.int_register_count, 1u);
  EXPECT_EQ(ir.string_register_count, 0u);
  EXPECT_EQ(ir.string_id_register_count, 3u);
  EXPECT_NE(ir.string_expression_registers[haystack_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.string_expression_registers[logic_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.string_expression_registers[nodes_index], LN_INVALID_INDEX);
  EXPECT_EQ(ir.string_expression_registers[dynamic_string_index], LN_INVALID_INDEX);
  EXPECT_EQ(ir.fallback_expression_count, 3u);

  bool has_string_predicate = false;
  bool has_folded_string_count = false;
  bool has_string_id_constant = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    has_string_predicate |= op.kind == LN_RegisterExpressionOpKind::BoolStringPredicate;
    has_folded_string_count |= op.kind == LN_RegisterExpressionOpKind::IntConstant &&
                               op.output_kind == LN_RegisterValueKind::Int;
    has_string_id_constant |= op.kind == LN_RegisterExpressionOpKind::StringIdConstant &&
                              op.output_kind == LN_RegisterValueKind::StringId;
  }
  EXPECT_TRUE(has_string_predicate);
  EXPECT_TRUE(has_folded_string_count);
  EXPECT_TRUE(has_string_id_constant);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersPureStringExpressions)
{
  LN_Program program;

  LN_StringExpression logic;
  logic.kind = LN_StringExpressionKind::Constant;
  logic.string_value = "logic";
  const uint32_t logic_index = program.AddStringExpression(logic);

  LN_StringExpression nodes;
  nodes.kind = LN_StringExpressionKind::Constant;
  nodes.string_value = " nodes";
  const uint32_t nodes_index = program.AddStringExpression(nodes);

  LN_StringExpression replacement;
  replacement.kind = LN_StringExpressionKind::Constant;
  replacement.string_value = "UPBGE";
  const uint32_t replacement_index = program.AddStringExpression(replacement);

  LN_IntExpression width;
  width.kind = LN_IntExpressionKind::Constant;
  width.int_value = 8;
  const uint32_t width_index = program.AddIntExpression(width);

  LN_StringExpression join;
  join.kind = LN_StringExpressionKind::Join;
  join.input0 = logic_index;
  join.input1 = nodes_index;
  const uint32_t join_index = program.AddStringExpression(join);

  LN_StringExpression replace;
  replace.kind = LN_StringExpressionKind::Replace;
  replace.input0 = join_index;
  replace.input1 = logic_index;
  replace.input2 = replacement_index;
  const uint32_t replace_index = program.AddStringExpression(replace);

  LN_StringExpression uppercase;
  uppercase.kind = LN_StringExpressionKind::ToUppercase;
  uppercase.input0 = replace_index;
  program.AddStringExpression(uppercase);

  LN_StringExpression lowercase;
  lowercase.kind = LN_StringExpressionKind::ToLowercase;
  lowercase.input0 = replace_index;
  program.AddStringExpression(lowercase);

  LN_StringExpression zerofill;
  zerofill.kind = LN_StringExpressionKind::ZeroFill;
  zerofill.input0 = logic_index;
  zerofill.int_expr_index = width_index;
  const uint32_t zerofill_index = program.AddStringExpression(zerofill);

  LN_StringExpression format_text;
  format_text.kind = LN_StringExpressionKind::Constant;
  format_text.string_value = "A:{} B:{} C:{} D:{}";
  const uint32_t format_text_index = program.AddStringExpression(format_text);

  LN_StringExpression format;
  format.kind = LN_StringExpressionKind::Format;
  format.input0 = format_text_index;
  format.input1 = replace_index;
  format.input2 = logic_index;
  format.input3 = nodes_index;
  format.input4 = zerofill_index;
  program.AddStringExpression(format);

  LN_ValueExpression int_value;
  int_value.kind = LN_ValueExpressionKind::FromInt;
  int_value.input0 = width_index;
  const uint32_t int_value_index = program.AddValueExpression(int_value);

  LN_StringExpression from_value;
  from_value.kind = LN_StringExpressionKind::FromGenericValue;
  from_value.value_expr_index = int_value_index;
  program.AddStringExpression(from_value);

  LN_BoolExpression contains;
  contains.kind = LN_BoolExpressionKind::StringContains;
  contains.input0 = replace_index;
  contains.input1 = replacement_index;
  program.AddBoolExpression(contains);

  LN_BoolExpression starts_with;
  starts_with.kind = LN_BoolExpressionKind::StringStartsWith;
  starts_with.input0 = replace_index;
  starts_with.input1 = replacement_index;
  program.AddBoolExpression(starts_with);

  LN_IntExpression count;
  count.kind = LN_IntExpressionKind::StringCount;
  count.input0 = replace_index;
  count.input1 = nodes_index;
  program.AddIntExpression(count);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.string_id_register_count, 5u);
  EXPECT_EQ(ir.string_register_count, 6u);
  EXPECT_EQ(ir.bool_register_count, 2u);
  EXPECT_EQ(ir.int_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 0u);

  bool has_replace = false;
  bool has_uppercase = false;
  bool has_lowercase = false;
  bool has_zerofill = false;
  bool has_format = false;
  bool has_from_value = false;
  bool has_dynamic_predicate = false;
  bool has_dynamic_count = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    has_replace |= op.kind == LN_RegisterExpressionOpKind::StringReplace;
    has_uppercase |= op.kind == LN_RegisterExpressionOpKind::StringToUppercase;
    has_lowercase |= op.kind == LN_RegisterExpressionOpKind::StringToLowercase;
    has_zerofill |= op.kind == LN_RegisterExpressionOpKind::StringZeroFill;
    has_format |= op.kind == LN_RegisterExpressionOpKind::StringFormat &&
                  op.variable_ref_count == 1;
    has_from_value |= op.kind == LN_RegisterExpressionOpKind::StringFromValue &&
                      op.input0.value_kind == LN_RegisterValueKind::GenericValue;
    has_dynamic_predicate |= op.kind == LN_RegisterExpressionOpKind::BoolStringPredicate &&
                             op.input0.value_kind == LN_RegisterValueKind::String;
    has_dynamic_count |= op.kind == LN_RegisterExpressionOpKind::IntStringCount &&
                         op.input0.value_kind == LN_RegisterValueKind::String;
  }
  EXPECT_TRUE(has_replace);
  EXPECT_TRUE(has_uppercase);
  EXPECT_TRUE(has_lowercase);
  EXPECT_TRUE(has_zerofill);
  EXPECT_TRUE(has_format);
  EXPECT_TRUE(has_from_value);
  EXPECT_TRUE(has_dynamic_predicate);
  EXPECT_TRUE(has_dynamic_count);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersDeterministicRandomExpressions)
{
  LN_Program program;

  LN_IntExpression int_min;
  int_min.kind = LN_IntExpressionKind::Constant;
  int_min.int_value = 1;
  const uint32_t int_min_index = program.AddIntExpression(int_min);

  LN_IntExpression int_max;
  int_max.kind = LN_IntExpressionKind::Constant;
  int_max.int_value = 5;
  const uint32_t int_max_index = program.AddIntExpression(int_max);

  LN_IntExpression int_random;
  int_random.kind = LN_IntExpressionKind::Random;
  int_random.input0 = int_min_index;
  int_random.input1 = int_max_index;
  program.AddIntExpression(int_random);

  LN_FloatExpression float_min;
  float_min.kind = LN_FloatExpressionKind::Constant;
  float_min.float_value = 0.25f;
  const uint32_t float_min_index = program.AddFloatExpression(float_min);

  LN_FloatExpression float_max;
  float_max.kind = LN_FloatExpressionKind::Constant;
  float_max.float_value = 0.75f;
  const uint32_t float_max_index = program.AddFloatExpression(float_max);

  LN_FloatExpression float_random;
  float_random.kind = LN_FloatExpressionKind::Random;
  float_random.input0 = float_min_index;
  float_random.input1 = float_max_index;
  program.AddFloatExpression(float_random);

  LN_VectorExpression axes;
  axes.kind = LN_VectorExpressionKind::Constant;
  axes.vector_value = MT_Vector3(1.0f, 0.0f, 1.0f);
  const uint32_t axes_index = program.AddVectorExpression(axes);

  LN_VectorExpression vector_random;
  vector_random.kind = LN_VectorExpressionKind::Random;
  vector_random.input0 = axes_index;
  program.AddVectorExpression(vector_random);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.int_register_count, 3u);
  EXPECT_EQ(ir.float_register_count, 3u);
  EXPECT_EQ(ir.vector_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 0u);

  bool has_int_random = false;
  bool has_float_random = false;
  bool has_vector_random = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    has_int_random |= op.kind == LN_RegisterExpressionOpKind::IntRandom;
    has_float_random |= op.kind == LN_RegisterExpressionOpKind::FloatRandom;
    has_vector_random |= op.kind == LN_RegisterExpressionOpKind::VectorRandom;
  }
  EXPECT_TRUE(has_int_random);
  EXPECT_TRUE(has_float_random);
  EXPECT_TRUE(has_vector_random);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersTypedRuntimeTreeProperties)
{
  LN_Program program;

  auto add_property_ref = [&](const char *name, const LN_ValueType type, const LN_Value &value) {
    LN_TreePropertyRef property_ref;
    property_ref.name = name;
    property_ref.value_type = type;
    property_ref.default_value = value;
    return program.AddTreePropertyRef(property_ref);
  };

  const uint32_t bool_ref = add_property_ref("visible",
                                             LN_ValueType::Bool,
                                             MakeBoolValue(true));
  const uint32_t int_ref = add_property_ref("group", LN_ValueType::Int, MakeIntValue(3));
  const uint32_t float_ref = add_property_ref("speed",
                                               LN_ValueType::Float,
                                               MakeFloatValue(0.5f));
  const uint32_t vector_ref = add_property_ref("target",
                                                LN_ValueType::Vector,
                                                MakeVectorValue(MT_Vector3(1.0f, 2.0f, 3.0f)));
  const uint32_t color_ref = add_property_ref("tint",
                                              LN_ValueType::Color,
                                              MakeColorValue(MT_Vector4(0.2f, 0.3f, 0.4f, 1.0f)));
  LN_Value string_value;
  string_value.type = LN_ValueType::String;
  string_value.exists = true;
  string_value.string_value = "runtime";
  const uint32_t string_ref = add_property_ref("label", LN_ValueType::String, string_value);

  LN_BoolExpression bool_property;
  bool_property.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
  bool_property.property_ref_index = bool_ref;
  program.AddBoolExpression(bool_property);

  LN_IntExpression int_property;
  int_property.kind = LN_IntExpressionKind::RuntimeTreeProperty;
  int_property.property_ref_index = int_ref;
  program.AddIntExpression(int_property);

  LN_FloatExpression float_property;
  float_property.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  float_property.property_ref_index = float_ref;
  program.AddFloatExpression(float_property);

  LN_VectorExpression vector_property;
  vector_property.kind = LN_VectorExpressionKind::RuntimeTreeProperty;
  vector_property.property_ref_index = vector_ref;
  program.AddVectorExpression(vector_property);

  LN_ColorExpression color_property;
  color_property.kind = LN_ColorExpressionKind::RuntimeTreeProperty;
  color_property.property_ref_index = color_ref;
  program.AddColorExpression(color_property);

  LN_StringExpression string_property;
  string_property.kind = LN_StringExpressionKind::RuntimeTreeProperty;
  string_property.property_ref_index = string_ref;
  program.AddStringExpression(string_property);

  LN_FloatExpression invalid_property;
  invalid_property.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  program.AddFloatExpression(invalid_property);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.bool_register_count, 1u);
  EXPECT_EQ(ir.int_register_count, 1u);
  EXPECT_EQ(ir.float_register_count, 1u);
  EXPECT_EQ(ir.vector_register_count, 1u);
  EXPECT_EQ(ir.color_register_count, 1u);
  EXPECT_EQ(ir.string_register_count, 1u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  bool has_bool_property = false;
  bool has_int_property = false;
  bool has_float_property = false;
  bool has_vector_property = false;
  bool has_color_property = false;
  bool has_string_property = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    has_bool_property |= op.kind == LN_RegisterExpressionOpKind::BoolRuntimePropertyRead;
    has_int_property |= op.kind == LN_RegisterExpressionOpKind::IntRuntimePropertyRead;
    has_float_property |= op.kind == LN_RegisterExpressionOpKind::FloatRuntimePropertyRead;
    has_vector_property |= op.kind == LN_RegisterExpressionOpKind::VectorRuntimePropertyRead;
    has_color_property |= op.kind == LN_RegisterExpressionOpKind::ColorRuntimePropertyRead;
    has_string_property |= op.kind == LN_RegisterExpressionOpKind::StringRuntimePropertyRead;
  }
  EXPECT_TRUE(has_bool_property);
  EXPECT_TRUE(has_int_property);
  EXPECT_TRUE(has_float_property);
  EXPECT_TRUE(has_vector_property);
  EXPECT_TRUE(has_color_property);
  EXPECT_TRUE(has_string_property);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersGenericValueProperties)
{
  LN_Program program;

  LN_GamePropertyRef game_property_ref;
  game_property_ref.name = "snapshot_payload";
  game_property_ref.value_type = LN_ValueType::String;
  const uint32_t game_property_ref_index = program.AddGamePropertyRef(game_property_ref);

  LN_Value runtime_value;
  runtime_value.type = LN_ValueType::Dict;
  runtime_value.exists = true;
  runtime_value.dict_value["score"] = MakeIntValue(7);

  LN_TreePropertyRef tree_property_ref;
  tree_property_ref.name = "runtime_payload";
  tree_property_ref.value_type = LN_ValueType::Dict;
  tree_property_ref.default_value = runtime_value;
  const uint32_t tree_property_ref_index = program.AddTreePropertyRef(tree_property_ref);

  LN_ValueExpression snapshot_property;
  snapshot_property.kind = LN_ValueExpressionKind::SnapshotGameProperty;
  snapshot_property.property_ref_index = game_property_ref_index;
  program.AddValueExpression(snapshot_property);

  LN_ValueExpression runtime_property;
  runtime_property.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  runtime_property.property_ref_index = tree_property_ref_index;
  program.AddValueExpression(runtime_property);

  LN_ValueExpression invalid_property;
  invalid_property.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  program.AddValueExpression(invalid_property);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.generic_value_register_count, 2u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  bool has_snapshot_property = false;
  bool has_runtime_property = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    has_snapshot_property |= op.kind == LN_RegisterExpressionOpKind::ValueSnapshotRead;
    has_runtime_property |= op.kind == LN_RegisterExpressionOpKind::ValueRuntimePropertyRead;
  }
  EXPECT_TRUE(has_snapshot_property);
  EXPECT_TRUE(has_runtime_property);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersSelfCollisionSnapshotInts)
{
  LN_Program program;

  LN_IntExpression collision_group;
  collision_group.kind = LN_IntExpressionKind::SnapshotCollisionGroup;
  program.AddIntExpression(collision_group);

  LN_IntExpression character_max_jumps;
  character_max_jumps.kind = LN_IntExpressionKind::SnapshotCharacterMaxJumps;
  program.AddIntExpression(character_max_jumps);

  LN_IntExpression character_jump_count;
  character_jump_count.kind = LN_IntExpressionKind::SnapshotCharacterJumpCount;
  program.AddIntExpression(character_jump_count);

  LN_IntExpression mouse_wheel;
  mouse_wheel.kind = LN_IntExpressionKind::MouseWheelDelta;
  program.AddIntExpression(mouse_wheel);

  LN_IntExpression window_width;
  window_width.kind = LN_IntExpressionKind::WindowResolutionWidth;
  program.AddIntExpression(window_width);

  LN_IntExpression window_height;
  window_height.kind = LN_IntExpressionKind::WindowResolutionHeight;
  program.AddIntExpression(window_height);

  LN_IntExpression window_vsync;
  window_vsync.kind = LN_IntExpressionKind::WindowVSyncMode;
  program.AddIntExpression(window_vsync);

  LN_IntExpression targeted_collision_group;
  targeted_collision_group.kind = LN_IntExpressionKind::SnapshotCollisionGroup;
  targeted_collision_group.input0 = 0;
  program.AddIntExpression(targeted_collision_group);

  LN_IntExpression targeted_character_max_jumps;
  targeted_character_max_jumps.kind = LN_IntExpressionKind::SnapshotCharacterMaxJumps;
  targeted_character_max_jumps.input0 = 0;
  program.AddIntExpression(targeted_character_max_jumps);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.int_register_count, 7u);
  EXPECT_EQ(ir.fallback_expression_count, 2u);

  uint32_t snapshot_read_count = 0;
  uint32_t input_read_count = 0;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.kind == LN_RegisterExpressionOpKind::IntSnapshotRead) {
      snapshot_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Int);
    }
    if (op.kind == LN_RegisterExpressionOpKind::IntInputRead) {
      input_read_count++;
      EXPECT_EQ(op.output_kind, LN_RegisterValueKind::Int);
    }
  }
  EXPECT_EQ(snapshot_read_count, 3u);
  EXPECT_EQ(input_read_count, 4u);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, DependencySummaryTracksWindowResolutionAsInputRead)
{
  LN_Program program;

  LN_BoolExpression fullscreen;
  fullscreen.kind = LN_BoolExpressionKind::WindowFullscreen;
  program.AddBoolExpression(fullscreen);

  LN_IntExpression width;
  width.kind = LN_IntExpressionKind::WindowResolutionWidth;
  program.AddIntExpression(width);

  LN_IntExpression height;
  height.kind = LN_IntExpressionKind::WindowResolutionHeight;
  program.AddIntExpression(height);

  LN_IntExpression vsync;
  vsync.kind = LN_IntExpressionKind::WindowVSyncMode;
  program.AddIntExpression(vsync);

  LN_VectorExpression resolution;
  resolution.kind = LN_VectorExpressionKind::WindowResolution;
  program.AddVectorExpression(resolution);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_EQ(summary.snapshot_channels, uint32_t(LN_DEP_SNAPSHOT_NONE));
  EXPECT_TRUE(HasDependencyMask(summary.input_channels, LN_DEP_INPUT_WINDOW));
  EXPECT_FALSE(HasDependencyMask(summary.input_channels, LN_DEP_INPUT_KEYBOARD));
  EXPECT_TRUE(HasDependencyMask(summary.access_classes, LN_DEP_ACCESS_IMMUTABLE_READ));
  EXPECT_TRUE(HasDependencyMask(program.GetSchedulerSummary().resource_access,
                                LN_SCHEDULER_RESOURCE_INPUT));
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, RegisterExpressionIRLeavesDynamicValuesAsFallback)
{
  LN_Program program;
  LN_FloatExpression dynamic_float;
  dynamic_float.kind = LN_FloatExpressionKind::FromGenericValue;
  program.AddFloatExpression(dynamic_float);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.float_register_count, 0u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);
  EXPECT_TRUE(ir.scalar_fallback_available);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, RegisterExpressionIRLowersTypedValueConversions)
{
  LN_Program program;

  LN_BoolExpression bool_constant;
  bool_constant.kind = LN_BoolExpressionKind::Constant;
  bool_constant.bool_value = true;
  const uint32_t bool_index = program.AddBoolExpression(bool_constant);

  LN_IntExpression int_constant;
  int_constant.kind = LN_IntExpressionKind::Constant;
  int_constant.int_value = 23;
  const uint32_t int_index = program.AddIntExpression(int_constant);

  LN_FloatExpression float_constant;
  float_constant.kind = LN_FloatExpressionKind::Constant;
  float_constant.float_value = 3.5f;
  const uint32_t float_index = program.AddFloatExpression(float_constant);

  LN_VectorExpression vector_constant;
  vector_constant.kind = LN_VectorExpressionKind::Constant;
  vector_constant.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_index = program.AddVectorExpression(vector_constant);

  LN_ColorExpression color_constant;
  color_constant.kind = LN_ColorExpressionKind::Constant;
  color_constant.color_value = MT_Vector4(0.2f, 0.4f, 0.6f, 1.0f);
  const uint32_t color_index = program.AddColorExpression(color_constant);

  LN_StringExpression string_constant;
  string_constant.kind = LN_StringExpressionKind::Constant;
  string_constant.string_value = "logic";
  const uint32_t string_index = program.AddStringExpression(string_constant);

  const std::array<std::pair<LN_ValueExpressionKind, uint32_t>, 6> conversions = {{
      {LN_ValueExpressionKind::FromBool, bool_index},
      {LN_ValueExpressionKind::FromInt, int_index},
      {LN_ValueExpressionKind::FromFloat, float_index},
      {LN_ValueExpressionKind::FromString, string_index},
      {LN_ValueExpressionKind::FromVector, vector_index},
      {LN_ValueExpressionKind::FromColor, color_index},
  }};
  uint32_t int_value_index = LN_INVALID_INDEX;
  uint32_t float_value_index = LN_INVALID_INDEX;
  uint32_t vector_value_index = LN_INVALID_INDEX;
  for (const auto &conversion : conversions) {
    LN_ValueExpression value;
    value.kind = conversion.first;
    value.input0 = conversion.second;
    const uint32_t value_index = program.AddValueExpression(value);
    if (conversion.first == LN_ValueExpressionKind::FromInt) {
      int_value_index = value_index;
    }
    else if (conversion.first == LN_ValueExpressionKind::FromFloat) {
      float_value_index = value_index;
    }
    else if (conversion.first == LN_ValueExpressionKind::FromVector) {
      vector_value_index = value_index;
    }
  }

  LN_ValueExpression select_value;
  select_value.kind = LN_ValueExpressionKind::Select;
  select_value.bool_expr_index = bool_index;
  select_value.input0 = int_value_index;
  select_value.input1 = float_value_index;
  program.AddValueExpression(select_value);

  LN_ValueExpression rotation_value;
  rotation_value.kind = LN_ValueExpressionKind::FromRotation;
  rotation_value.input0 = vector_index;
  program.AddValueExpression(rotation_value);

  LN_ValueExpression vector4_value;
  vector4_value.kind = LN_ValueExpressionKind::CombineVector4;
  vector4_value.input_indices = {float_index, float_index, float_index, float_index};
  program.AddValueExpression(vector4_value);

  LN_ValueExpression resized_value;
  resized_value.kind = LN_ValueExpressionKind::ResizeVectorValue;
  resized_value.input0 = vector_value_index;
  resized_value.property_ref_index = 2;
  program.AddValueExpression(resized_value);

  LN_ValueExpression matrix_value;
  matrix_value.kind = LN_ValueExpressionKind::EulerToMatrix;
  matrix_value.input0 = vector_index;
  const uint32_t matrix_value_index = program.AddValueExpression(matrix_value);

  LN_ValueExpression euler_value;
  euler_value.kind = LN_ValueExpressionKind::MatrixToEuler;
  euler_value.input0 = matrix_value_index;
  euler_value.value.type = LN_ValueType::Vector;
  program.AddValueExpression(euler_value);

  LN_ValueExpression rotation_constant;
  rotation_constant.kind = LN_ValueExpressionKind::Constant;
  rotation_constant.value.type = LN_ValueType::Rotation;
  rotation_constant.value.exists = true;
  rotation_constant.value.rotation_euler_value = MT_Vector3(0.1f, 0.2f, 0.3f);
  program.AddValueExpression(rotation_constant);

  LN_ValueExpression vector4_constant;
  vector4_constant.kind = LN_ValueExpressionKind::Constant;
  vector4_constant.value.type = LN_ValueType::Vector4;
  vector4_constant.value.exists = true;
  vector4_constant.value.vector4_value = MT_Vector4(1.0f, 2.0f, 3.0f, 4.0f);
  program.AddValueExpression(vector4_constant);

  LN_ValueExpression matrix_constant;
  matrix_constant.kind = LN_ValueExpressionKind::Constant;
  matrix_constant.value.type = LN_ValueType::Matrix;
  matrix_constant.value.exists = true;
  matrix_constant.value.matrix_value = MT_Matrix3x3(MT_Vector3(0.1f, 0.2f, 0.3f));
  program.AddValueExpression(matrix_constant);

  LN_ValueExpression empty_dict;
  empty_dict.kind = LN_ValueExpressionKind::EmptyDict;
  const uint32_t empty_dict_index = program.AddValueExpression(empty_dict);

  LN_ValueExpression make_dict;
  make_dict.kind = LN_ValueExpressionKind::MakeDict;
  make_dict.input0 = string_index;
  make_dict.input1 = int_value_index;
  const uint32_t make_dict_index = program.AddValueExpression(make_dict);

  LN_ValueExpression dict_get_key;
  dict_get_key.kind = LN_ValueExpressionKind::DictGetKey;
  dict_get_key.input0 = make_dict_index;
  dict_get_key.input1 = string_index;
  dict_get_key.input2 = float_value_index;
  program.AddValueExpression(dict_get_key);

  LN_ValueExpression dict_set_key;
  dict_set_key.kind = LN_ValueExpressionKind::DictSetKey;
  dict_set_key.input0 = make_dict_index;
  dict_set_key.input1 = string_index;
  dict_set_key.input2 = float_value_index;
  const uint32_t dict_set_key_index = program.AddValueExpression(dict_set_key);

  LN_ValueExpression dict_remove_key;
  dict_remove_key.kind = LN_ValueExpressionKind::DictRemoveKey;
  dict_remove_key.input0 = dict_set_key_index;
  dict_remove_key.input1 = string_index;
  program.AddValueExpression(dict_remove_key);

  LN_ValueExpression dict_remove_key_value;
  dict_remove_key_value.kind = LN_ValueExpressionKind::DictRemoveKeyValue;
  dict_remove_key_value.input0 = dict_set_key_index;
  dict_remove_key_value.input1 = string_index;
  program.AddValueExpression(dict_remove_key_value);

  LN_ValueExpression dict_merge;
  dict_merge.kind = LN_ValueExpressionKind::DictMerge;
  dict_merge.input0 = empty_dict_index;
  dict_merge.input1 = make_dict_index;
  const uint32_t dict_merge_index = program.AddValueExpression(dict_merge);

  LN_ValueExpression dict_get_keys;
  dict_get_keys.kind = LN_ValueExpressionKind::DictGetKeys;
  dict_get_keys.input0 = dict_merge_index;
  program.AddValueExpression(dict_get_keys);

  LN_ValueExpression empty_list;
  empty_list.kind = LN_ValueExpressionKind::EmptyList;
  empty_list.input0 = int_index;
  const uint32_t empty_list_index = program.AddValueExpression(empty_list);

  LN_ValueExpression make_list;
  make_list.kind = LN_ValueExpressionKind::MakeList;
  make_list.input0 = int_value_index;
  make_list.input1 = float_value_index;
  make_list.input2 = empty_dict_index;
  const uint32_t make_list_index = program.AddValueExpression(make_list);

  LN_ValueExpression list_from_items;
  list_from_items.kind = LN_ValueExpressionKind::ListFromItems;
  list_from_items.input_indices = {
      int_value_index,
      float_value_index,
      empty_dict_index,
      empty_list_index,
      make_list_index,
  };
  program.AddValueExpression(list_from_items);

  LN_ValueExpression list_duplicate;
  list_duplicate.kind = LN_ValueExpressionKind::ListDuplicate;
  list_duplicate.input0 = empty_list_index;
  const uint32_t list_duplicate_index = program.AddValueExpression(list_duplicate);

  LN_ValueExpression list_extend;
  list_extend.kind = LN_ValueExpressionKind::ListExtend;
  list_extend.input0 = list_duplicate_index;
  list_extend.input1 = empty_list_index;
  const uint32_t list_extend_index = program.AddValueExpression(list_extend);

  LN_ValueExpression list_append;
  list_append.kind = LN_ValueExpressionKind::ListAppend;
  list_append.input0 = list_extend_index;
  list_append.input1 = int_value_index;
  const uint32_t list_append_index = program.AddValueExpression(list_append);

  LN_IntExpression negative_index;
  negative_index.kind = LN_IntExpressionKind::Constant;
  negative_index.int_value = -1;
  const uint32_t negative_index_index = program.AddIntExpression(negative_index);

  LN_ValueExpression list_remove_index;
  list_remove_index.kind = LN_ValueExpressionKind::ListRemoveIndex;
  list_remove_index.input0 = list_append_index;
  list_remove_index.input1 = negative_index_index;
  const uint32_t list_remove_index_index = program.AddValueExpression(list_remove_index);

  LN_ValueExpression list_remove_value;
  list_remove_value.kind = LN_ValueExpressionKind::ListRemoveValue;
  list_remove_value.input0 = list_remove_index_index;
  list_remove_value.input1 = float_value_index;
  const uint32_t list_remove_value_index = program.AddValueExpression(list_remove_value);

  LN_ValueExpression list_set_index;
  list_set_index.kind = LN_ValueExpressionKind::ListSetIndex;
  list_set_index.input0 = list_remove_value_index;
  list_set_index.input1 = negative_index_index;
  list_set_index.input2 = int_value_index;
  const uint32_t list_set_index_index = program.AddValueExpression(list_set_index);

  LN_ValueExpression list_element;
  list_element.kind = LN_ValueExpressionKind::ListElement;
  list_element.input0 = list_set_index_index;
  list_element.input1 = int_index;
  program.AddValueExpression(list_element);

  LN_ValueExpression list_random_item;
  list_random_item.kind = LN_ValueExpressionKind::ListRandomItem;
  list_random_item.input0 = list_set_index_index;
  program.AddValueExpression(list_random_item);

  LN_ValueExpression value_switch_list;
  value_switch_list.kind = LN_ValueExpressionKind::ValueSwitchList;
  value_switch_list.input_indices = {
      bool_index,
      int_value_index,
      LN_INVALID_INDEX,
      float_value_index,
  };
  program.AddValueExpression(value_switch_list);

  LN_ValueExpression value_switch_compare;
  value_switch_compare.kind = LN_ValueExpressionKind::ValueSwitchListCompare;
  value_switch_compare.input0 = int_value_index;
  value_switch_compare.input1 = float_value_index;
  value_switch_compare.input_indices = {
      int_value_index,
      vector_value_index,
  };
  value_switch_compare.value.exists = true;
  value_switch_compare.value.int_value = int32_t(LN_FloatCompareOperation::Equal);
  program.AddValueExpression(value_switch_compare);

  LN_BoolExpression value_is_none;
  value_is_none.kind = LN_BoolExpressionKind::ValueIsNone;
  value_is_none.input0 = int_value_index;
  program.AddBoolExpression(value_is_none);

  LN_BoolExpression bool_from_value;
  bool_from_value.kind = LN_BoolExpressionKind::FromGenericValue;
  bool_from_value.input0 = int_value_index;
  program.AddBoolExpression(bool_from_value);

  LN_BoolExpression value_compare;
  value_compare.kind = LN_BoolExpressionKind::ValueCompare;
  value_compare.input0 = int_value_index;
  value_compare.input1 = int_value_index;
  value_compare.float_compare_operation = LN_FloatCompareOperation::Equal;
  program.AddBoolExpression(value_compare);

  LN_IntExpression int_from_value;
  int_from_value.kind = LN_IntExpressionKind::FromGenericValue;
  int_from_value.input0 = int_value_index;
  program.AddIntExpression(int_from_value);

  LN_IntExpression dict_length;
  dict_length.kind = LN_IntExpressionKind::DictLength;
  dict_length.input0 = empty_dict_index;
  program.AddIntExpression(dict_length);

  LN_IntExpression list_length;
  list_length.kind = LN_IntExpressionKind::ListLength;
  list_length.input0 = empty_list_index;
  program.AddIntExpression(list_length);

  LN_FloatExpression float_from_value;
  float_from_value.kind = LN_FloatExpressionKind::FromGenericValue;
  float_from_value.input0 = float_value_index;
  program.AddFloatExpression(float_from_value);

  LN_VectorExpression vector_from_value;
  vector_from_value.kind = LN_VectorExpressionKind::FromGenericValue;
  vector_from_value.input0 = vector_value_index;
  program.AddVectorExpression(vector_from_value);

  LN_ValueExpression from_string;
  from_string.kind = LN_ValueExpressionKind::FromString;
  from_string.input0 = LN_INVALID_INDEX;
  program.AddValueExpression(from_string);

  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.cache_generation, LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION);
  EXPECT_EQ(ir.generic_value_register_count, 36u);
  EXPECT_EQ(ir.fallback_expression_count, 1u);

  uint32_t value_conversion_ops = 0;
  uint32_t value_consumer_ops = 0;
  bool has_int_dict_length = false;
  bool has_int_list_length = false;
  bool has_value_select = false;
  bool has_value_from_rotation = false;
  bool has_value_from_string = false;
  bool has_value_combine_vector4 = false;
  bool has_value_resize_vector = false;
  bool has_value_euler_to_matrix = false;
  bool has_value_matrix_to_euler = false;
  bool has_value_make_list = false;
  bool has_value_list_from_items = false;
  bool has_value_list_duplicate = false;
  bool has_value_list_extend = false;
  bool has_value_list_append = false;
  bool has_value_list_remove_index = false;
  bool has_value_list_remove_value = false;
  bool has_value_list_set_index = false;
  bool has_value_list_element = false;
  bool has_value_list_random_item = false;
  bool has_value_switch_list = false;
  bool has_value_switch_list_compare = false;
  bool has_value_empty_list = false;
  bool has_value_empty_dict = false;
  bool has_value_make_dict = false;
  bool has_value_dict_get_key = false;
  bool has_value_dict_set_key = false;
  bool has_value_dict_remove_key = false;
  bool has_value_dict_remove_key_value = false;
  bool has_value_dict_merge = false;
  bool has_value_dict_get_keys = false;
  for (const LN_RegisterExpressionOp &op : ir.ops) {
    if (op.output_kind == LN_RegisterValueKind::GenericValue) {
      value_conversion_ops++;
      EXPECT_NE(op.output_register, LN_INVALID_INDEX);
    }
    has_value_select |= op.kind == LN_RegisterExpressionOpKind::ValueSelect;
    has_value_from_rotation |= op.kind == LN_RegisterExpressionOpKind::ValueFromRotation;
    has_value_from_string |= op.kind == LN_RegisterExpressionOpKind::ValueFromString;
    has_value_combine_vector4 |= op.kind == LN_RegisterExpressionOpKind::ValueCombineVector4;
    has_value_resize_vector |= op.kind == LN_RegisterExpressionOpKind::ValueResizeVector;
    has_value_euler_to_matrix |= op.kind == LN_RegisterExpressionOpKind::ValueEulerToMatrix;
    has_value_matrix_to_euler |= op.kind == LN_RegisterExpressionOpKind::ValueMatrixToEuler;
    has_value_make_list |= op.kind == LN_RegisterExpressionOpKind::ValueMakeList;
    if (op.kind == LN_RegisterExpressionOpKind::ValueListFromItems) {
      has_value_list_from_items = true;
      EXPECT_EQ(op.variable_ref_count, 5u);
      EXPECT_LT(op.variable_ref_start, ir.variable_refs.size());
    }
    has_value_list_duplicate |= op.kind == LN_RegisterExpressionOpKind::ValueListDuplicate;
    has_value_list_extend |= op.kind == LN_RegisterExpressionOpKind::ValueListExtend;
    has_value_list_append |= op.kind == LN_RegisterExpressionOpKind::ValueListAppend;
    has_value_list_remove_index |= op.kind == LN_RegisterExpressionOpKind::ValueListRemoveIndex;
    has_value_list_remove_value |= op.kind == LN_RegisterExpressionOpKind::ValueListRemoveValue;
    has_value_list_set_index |= op.kind == LN_RegisterExpressionOpKind::ValueListSetIndex;
    has_value_list_element |= op.kind == LN_RegisterExpressionOpKind::ValueListElement;
    has_value_list_random_item |= op.kind == LN_RegisterExpressionOpKind::ValueListRandomItem;
    if (op.kind == LN_RegisterExpressionOpKind::ValueSwitchList) {
      has_value_switch_list = true;
      EXPECT_EQ(op.variable_ref_count, 4u);
    }
    if (op.kind == LN_RegisterExpressionOpKind::ValueSwitchListCompare) {
      has_value_switch_list_compare = true;
      EXPECT_EQ(op.variable_ref_count, 2u);
    }
    has_value_empty_list |= op.kind == LN_RegisterExpressionOpKind::ValueEmptyList;
    has_value_empty_dict |= op.kind == LN_RegisterExpressionOpKind::ValueEmptyDict;
    has_value_make_dict |= op.kind == LN_RegisterExpressionOpKind::ValueMakeDict;
    has_value_dict_get_key |= op.kind == LN_RegisterExpressionOpKind::ValueDictGetKey;
    has_value_dict_set_key |= op.kind == LN_RegisterExpressionOpKind::ValueDictSetKey;
    has_value_dict_remove_key |= op.kind == LN_RegisterExpressionOpKind::ValueDictRemoveKey;
    has_value_dict_remove_key_value |= op.kind ==
                                       LN_RegisterExpressionOpKind::ValueDictRemoveKeyValue;
    has_value_dict_merge |= op.kind == LN_RegisterExpressionOpKind::ValueDictMerge;
    has_value_dict_get_keys |= op.kind == LN_RegisterExpressionOpKind::ValueDictGetKeys;
    if (op.kind == LN_RegisterExpressionOpKind::BoolValueIsNone ||
        op.kind == LN_RegisterExpressionOpKind::BoolFromValue ||
        op.kind == LN_RegisterExpressionOpKind::BoolValueCompare ||
        op.kind == LN_RegisterExpressionOpKind::IntFromValue ||
        op.kind == LN_RegisterExpressionOpKind::IntDictLength ||
        op.kind == LN_RegisterExpressionOpKind::IntListLength ||
        op.kind == LN_RegisterExpressionOpKind::FloatFromValue ||
        op.kind == LN_RegisterExpressionOpKind::VectorFromValue)
    {
      value_consumer_ops++;
      EXPECT_EQ(op.input0.value_kind, LN_RegisterValueKind::GenericValue);
      EXPECT_NE(op.input0.register_index, LN_INVALID_INDEX);
    }
    has_int_dict_length |= op.kind == LN_RegisterExpressionOpKind::IntDictLength;
    has_int_list_length |= op.kind == LN_RegisterExpressionOpKind::IntListLength;
  }
  EXPECT_EQ(value_conversion_ops, 36u);
  EXPECT_EQ(value_consumer_ops, 8u);
  EXPECT_TRUE(has_int_dict_length);
  EXPECT_TRUE(has_int_list_length);
  EXPECT_TRUE(has_value_select);
  EXPECT_TRUE(has_value_from_rotation);
  EXPECT_TRUE(has_value_from_string);
  EXPECT_TRUE(has_value_combine_vector4);
  EXPECT_TRUE(has_value_resize_vector);
  EXPECT_TRUE(has_value_euler_to_matrix);
  EXPECT_TRUE(has_value_matrix_to_euler);
  EXPECT_TRUE(has_value_make_list);
  EXPECT_TRUE(has_value_list_from_items);
  EXPECT_TRUE(has_value_list_duplicate);
  EXPECT_TRUE(has_value_list_extend);
  EXPECT_TRUE(has_value_list_append);
  EXPECT_TRUE(has_value_list_remove_index);
  EXPECT_TRUE(has_value_list_remove_value);
  EXPECT_TRUE(has_value_list_set_index);
  EXPECT_TRUE(has_value_list_element);
  EXPECT_TRUE(has_value_list_random_item);
  EXPECT_TRUE(has_value_switch_list);
  EXPECT_TRUE(has_value_switch_list_compare);
  EXPECT_TRUE(has_value_empty_list);
  EXPECT_TRUE(has_value_empty_dict);
  EXPECT_TRUE(has_value_make_dict);
  EXPECT_TRUE(has_value_dict_get_key);
  EXPECT_TRUE(has_value_dict_set_key);
  EXPECT_TRUE(has_value_dict_remove_key);
  EXPECT_TRUE(has_value_dict_remove_key_value);
  EXPECT_TRUE(has_value_dict_merge);
  EXPECT_TRUE(has_value_dict_get_keys);
  EXPECT_TRUE(program.ValidateRegisterExpressionIR());
}

TEST(LN_Program, DependencySummaryTracksOnlyTransformSnapshotForWorldPosition)
{
  LN_Program program;
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::SnapshotWorldPosition;
  program.AddVectorExpression(expression);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_EQ(summary.snapshot_channels, uint32_t(LN_DEP_SNAPSHOT_TRANSFORM));
  EXPECT_EQ(summary.input_channels, uint32_t(LN_DEP_INPUT_NONE));
  EXPECT_EQ(summary.event_channels, uint32_t(LN_DEP_EVENT_NONE));
  EXPECT_EQ(summary.command_classes, uint32_t(LN_DEP_COMMAND_NONE));
  EXPECT_TRUE(HasDependencyMask(summary.access_classes, LN_DEP_ACCESS_SNAPSHOT_READ));
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, DependencySummaryTracksCameraScreenVectorSnapshotReads)
{
  LN_Program program;
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::ScreenToWorld;
  program.AddVectorExpression(expression);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_TRUE(HasDependencyMask(summary.snapshot_channels,
                                LN_DEP_SNAPSHOT_TRANSFORM |
                                    LN_DEP_SNAPSHOT_OBJECT_GRAPH));
  EXPECT_TRUE(HasDependencyMask(program.GetSchedulerSummary().resource_access,
                                LN_SCHEDULER_RESOURCE_SNAPSHOT));
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, DependencySummaryTracksEventCommandWithoutSnapshotOrInput)
{
  LN_Program program;
  LN_StringExpression subject;
  subject.kind = LN_StringExpressionKind::Constant;
  subject.string_value = "damage";
  const uint32_t subject_index = program.AddStringExpression(subject);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SendEvent;
  instruction.string_expr_index = subject_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_EQ(summary.snapshot_channels, uint32_t(LN_DEP_SNAPSHOT_NONE));
  EXPECT_EQ(summary.input_channels, uint32_t(LN_DEP_INPUT_NONE));
  EXPECT_TRUE(HasDependencyMask(summary.event_channels, LN_DEP_EVENT_SUBJECT_WRITE));
  EXPECT_TRUE(HasDependencyMask(summary.command_classes, LN_DEP_COMMAND_EVENT));
  EXPECT_FALSE(HasDependencyMask(summary.dynamic_flags, LN_DEP_DYNAMIC_EVENT_SUBJECT));
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, InstructionPayloadValidationAcceptsTargetedSendEventValueSlot)
{
  LN_Program program;
  LN_StringExpression subject;
  subject.kind = LN_StringExpressionKind::Constant;
  subject.string_value = "damage";
  const uint32_t subject_index = program.AddStringExpression(subject);

  LN_ValueExpression target;
  target.kind = LN_ValueExpressionKind::Constant;
  target.value.type = LN_ValueType::ObjectRef;
  target.value.exists = true;
  const uint32_t target_index = program.AddValueExpression(target);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SendEvent;
  instruction.string_expr_index = subject_index;
  instruction.int_expr_index = target_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_TRUE(HasDependencyMask(summary.event_channels,
                                LN_DEP_EVENT_SUBJECT_WRITE | LN_DEP_EVENT_TARGETED_SEND));
  EXPECT_TRUE(HasDependencyMask(summary.dynamic_flags, LN_DEP_DYNAMIC_OBJECT_REF));
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, DependencySummaryMarksDynamicEventSubjectsAndPropertyNames)
{
  LN_Program program;

  LN_Instruction event_instruction;
  event_instruction.opcode = LN_OpCode::SendEvent;
  event_instruction.string_expr_index = LN_INVALID_INDEX;
  program.AddInstruction(LN_Event::OnFixedUpdate, event_instruction);

  LN_Instruction property_instruction;
  property_instruction.opcode = LN_OpCode::SetGameProperty;
  property_instruction.property_ref_index = LN_INVALID_INDEX;
  property_instruction.property_value_type = LN_ValueType::Float;
  program.AddInstruction(LN_Event::OnFixedUpdate, property_instruction);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_TRUE(HasDependencyMask(summary.dynamic_flags,
                                LN_DEP_DYNAMIC_EVENT_SUBJECT |
                                    LN_DEP_DYNAMIC_PROPERTY_NAME));
  EXPECT_TRUE(HasDependencyMask(summary.command_classes,
                                LN_DEP_COMMAND_EVENT | LN_DEP_COMMAND_PROPERTY));
}

TEST(LN_Program, DependencyValidationRejectsSummaryDrift)
{
  LN_Program program;
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::SnapshotWorldPosition;
  program.AddVectorExpression(expression);
  ASSERT_TRUE(HasDependencyMask(program.GetDependencySummary().snapshot_channels,
                                LN_DEP_SNAPSHOT_TRANSFORM));

  LN_ProgramDependencySummary &summary = const_cast<LN_ProgramDependencySummary &>(
      program.GetDependencySummary());
  summary.snapshot_channels &= ~LN_DEP_SNAPSHOT_TRANSFORM;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "dependency summary"));
}

TEST(LN_Program, DependencySummaryFeedsParallelEligibility)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SaveGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const LN_ProgramDependencySummary &summary = program.GetDependencySummary();
  EXPECT_FALSE(summary.worker_lane_eligible);
  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_TRUE(HasDependencyMask(summary.access_classes,
                                LN_DEP_ACCESS_IMMEDIATE_MAIN_THREAD_SIDE_EFFECT));
  EXPECT_TRUE(HasDependencyMask(
      summary.main_thread_reason_mask,
      1u << uint8_t(LN_MainThreadOnlyReason::FilePersistence)));
}

TEST(LN_Program, RuntimeTreePropertyReadsUseRuntimeStateDependencies)
{
  LN_Program program;

  LN_TreePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Float;
  property_ref.default_value = MakeFloatValue(1.0f);
  const uint32_t property_ref_index = program.AddTreePropertyRef(property_ref);

  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  expression.property_ref_index = property_ref_index;
  program.AddFloatExpression(expression);

  const LN_ProgramDependencySummary &dependencies = program.GetDependencySummary();
  EXPECT_EQ(dependencies.snapshot_channels, uint32_t(LN_DEP_SNAPSHOT_NONE));
  EXPECT_TRUE(HasDependencyMask(dependencies.state_preservation, LN_DEP_STATE_TREE_PROPERTY));
  EXPECT_TRUE(HasDependencyMask(dependencies.access_classes, LN_DEP_ACCESS_RUNTIME_STATE));
  EXPECT_FALSE(HasDependencyMask(dependencies.access_classes, LN_DEP_ACCESS_SNAPSHOT_READ));

  const LN_SchedulerSummary &scheduler = program.GetSchedulerSummary();
  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_TRUE(HasResource(scheduler, LN_SCHEDULER_RESOURCE_LOGIC_MANAGER));
  EXPECT_FALSE(HasResource(scheduler, LN_SCHEDULER_RESOURCE_SNAPSHOT));
  EXPECT_FALSE(scheduler.reads_snapshot_only);
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, RuntimeInstructionSemanticsCatalogCoversEveryOpcode)
{
  size_t catalog_count = 0;
  const LN_RuntimeInstructionSemantics *catalog = LN_GetRuntimeInstructionSemanticsCatalog(
      &catalog_count);
  ASSERT_NE(catalog, nullptr);
  EXPECT_EQ(catalog_count, size_t(LN_OpCode::Count));
  EXPECT_FALSE(LN_RuntimeInstructionSemanticsCatalogHasDuplicates());

  for (uint32_t opcode_value = 0; opcode_value < uint32_t(LN_OpCode::Count); opcode_value++) {
    const LN_OpCode opcode = static_cast<LN_OpCode>(opcode_value);
    const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(opcode);
    ASSERT_NE(semantics, nullptr) << "missing semantics for opcode value " << opcode_value;
    EXPECT_EQ(semantics->opcode, opcode);
    ASSERT_NE(semantics->name, nullptr);
    EXPECT_STRNE(semantics->name, "");
  }
}

TEST(LN_Program, RuntimeInstructionSemanticsClassifiesCoreOpcodeFamilies)
{
  const LN_RuntimeInstructionSemantics *set_position = LN_GetRuntimeInstructionSemantics(
      LN_OpCode::SetWorldPosition);
  ASSERT_NE(set_position, nullptr);
  EXPECT_NE(set_position->writes & LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM, 0u);
  EXPECT_NE(set_position->writes & LN_RUNTIME_SEMANTIC_WRITE_OBJECT_STATE, 0u);
  EXPECT_EQ(set_position->threading, LN_RuntimeSemanticThreading::WorkerSafeRecord);
  EXPECT_EQ(set_position->coalescing, LN_RuntimeSemanticCoalescing::LastWriteWins);

  const LN_RuntimeInstructionSemantics *send_event = LN_GetRuntimeInstructionSemantics(
      LN_OpCode::SendEvent);
  ASSERT_NE(send_event, nullptr);
  EXPECT_NE(send_event->writes & LN_RUNTIME_SEMANTIC_WRITE_EVENT_BUS, 0u);
  EXPECT_EQ(send_event->ordering, LN_RuntimeSemanticOrdering::ObservableGlobal);
  EXPECT_TRUE(HasFallback(*send_event, LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP));

  const LN_RuntimeInstructionSemantics *save_variable = LN_GetRuntimeInstructionSemantics(
      LN_OpCode::SaveVariable);
  ASSERT_NE(save_variable, nullptr);
  EXPECT_NE(save_variable->writes & LN_RUNTIME_SEMANTIC_WRITE_FILE, 0u);
  EXPECT_EQ(save_variable->writes & LN_RUNTIME_SEMANTIC_WRITE_COMMAND_STREAM, 0u);
  EXPECT_EQ(save_variable->timing, LN_RuntimeSemanticTiming::ImmediateMainThreadRecord);
  EXPECT_EQ(save_variable->threading, LN_RuntimeSemanticThreading::MainThreadRecord);

  const LN_RuntimeInstructionSemantics *branch = LN_GetRuntimeInstructionSemantics(
      LN_OpCode::BranchRoute);
  ASSERT_NE(branch, nullptr);
  EXPECT_EQ(branch->timing, LN_RuntimeSemanticTiming::BranchRoute);
  EXPECT_NE(branch->writes & LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE, 0u);
}

TEST(LN_Program, RuntimeCommandFamiliesCoverEveryOpcode)
{
  for (uint32_t opcode_value = 0; opcode_value < uint32_t(LN_OpCode::Count); opcode_value++) {
    const LN_OpCode opcode = static_cast<LN_OpCode>(opcode_value);
    EXPECT_NE(LN_GetRuntimeCommandFamily(opcode), LN_RuntimeCommandFamily::Unknown)
        << "missing command family for opcode value " << opcode_value;
    EXPECT_NE(LN_GetRuntimeSideEffectDelivery(opcode), LN_RuntimeSideEffectDelivery::Unknown)
        << "missing side-effect delivery for opcode value " << opcode_value;
  }
}

TEST(LN_Program, RuntimeCommandFamiliesClassifyCoreDeliveryPaths)
{
  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::SetWorldPosition),
            LN_RuntimeCommandFamily::ObjectTransform);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::SetWorldPosition),
            LN_RuntimeSideEffectDelivery::DeferredCommandBuffer);

  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::SaveVariable),
            LN_RuntimeCommandFamily::File);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::SaveVariable),
            LN_RuntimeSideEffectDelivery::ImmediateMainThread);

  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::AssignGeometryNodesModifier),
            LN_RuntimeCommandFamily::Datablock);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::AssignGeometryNodesModifier),
            LN_RuntimeSideEffectDelivery::ImmediateMainThread);

  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::AddPhysicsConstraint),
            LN_RuntimeCommandFamily::PhysicsConstraint);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::AddPhysicsConstraint),
            LN_RuntimeSideEffectDelivery::ImmediateAndDeferred);

  const LN_RuntimeInstructionSemantics *follow_path = LN_GetRuntimeInstructionSemantics(
      LN_OpCode::FollowPath);
  ASSERT_NE(follow_path, nullptr);
  EXPECT_EQ(follow_path->threading, LN_RuntimeSemanticThreading::MainThreadRecord);
  EXPECT_TRUE(HasFallback(*follow_path, LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY));
  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::FollowPath),
            LN_RuntimeCommandFamily::Navigation);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::FollowPath),
            LN_RuntimeSideEffectDelivery::DeferredCommandBuffer);

  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::ArmTimer),
            LN_RuntimeCommandFamily::TreeState);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::ArmTimer),
            LN_RuntimeSideEffectDelivery::RuntimeTreeState);

  EXPECT_EQ(LN_GetRuntimeCommandFamily(LN_OpCode::Nop), LN_RuntimeCommandFamily::None);
  EXPECT_EQ(LN_GetRuntimeSideEffectDelivery(LN_OpCode::Nop),
            LN_RuntimeSideEffectDelivery::None);
}

TEST(LN_Program, GeometryNodesModifierAssignmentIsAnImmediateMainThreadMutation)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::AssignGeometryNodesModifier;
  program.AddInstruction(LN_Event::OnInit, instruction);

  std::vector<std::string> errors;
  EXPECT_TRUE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(errors.empty());

  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::MainThreadOnly);
  EXPECT_EQ(summary.main_thread_only_reason,
            LN_MainThreadOnlyReason::ImmediateCommandHelper);
  EXPECT_FALSE(summary.emits_commands);
  EXPECT_FALSE(summary.worker_lane_eligible);
  EXPECT_FALSE(HasResource(summary, LN_SCHEDULER_RESOURCE_COMMAND_BUFFER));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_RENDER));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_LOGIC_MANAGER));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_DATABLOCK));
}

TEST(LN_Program, RuntimeExpressionSemanticsCoversEveryExpressionKind)
{
  for (uint32_t value = 0; value < uint32_t(LN_BoolExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_BoolExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing bool expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Bool);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_FloatExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_FloatExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing float expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Float);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_IntExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_IntExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing int expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Int);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_StringExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_StringExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing string expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::String);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_VectorExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_VectorExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing vector expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Vector);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_ColorExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_ColorExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing color expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Color);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_ValueExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_ValueExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing value expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Value);
    EXPECT_EQ(semantics.kind, value);
  }
  for (uint32_t value = 0; value < uint32_t(LN_QueryExpressionKind::Count); value++) {
    const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
        static_cast<LN_QueryExpressionKind>(value));
    EXPECT_TRUE(semantics.known) << "missing query expression semantics for value " << value;
    EXPECT_EQ(semantics.family, LN_RuntimeExpressionFamily::Query);
    EXPECT_EQ(semantics.kind, value);
  }
}

TEST(LN_Program, RuntimeExpressionSemanticsClassifiesCoreExpressionFamilies)
{
  const LN_RuntimeExpressionSemantics event = LN_GetRuntimeExpressionSemantics(
      LN_BoolExpressionKind::EventReceived);
  EXPECT_TRUE(HasRead(event, LN_RUNTIME_SEMANTIC_READ_EVENT_BUS));
  EXPECT_EQ(event.threading, LN_RuntimeSemanticThreading::WorkerSafeRecord);

  const LN_RuntimeExpressionSemantics transform = LN_GetRuntimeExpressionSemantics(
      LN_VectorExpressionKind::SnapshotWorldPosition);
  EXPECT_TRUE(HasRead(transform, LN_RUNTIME_SEMANTIC_READ_SNAPSHOT));

  const LN_RuntimeExpressionSemantics input = LN_GetRuntimeExpressionSemantics(
      LN_VectorExpressionKind::GamepadStick);
  EXPECT_TRUE(HasRead(input, LN_RUNTIME_SEMANTIC_READ_INPUT));

  const LN_RuntimeExpressionSemantics window_vsync = LN_GetRuntimeExpressionSemantics(
      LN_IntExpressionKind::WindowVSyncMode);
  EXPECT_TRUE(HasRead(window_vsync, LN_RUNTIME_SEMANTIC_READ_INPUT));
  EXPECT_FALSE(HasRead(window_vsync, LN_RUNTIME_SEMANTIC_READ_SNAPSHOT));
  EXPECT_EQ(window_vsync.threading, LN_RuntimeSemanticThreading::WorkerSafeRecord);

  const LN_RuntimeExpressionSemantics query = LN_GetRuntimeExpressionSemantics(
      LN_ValueExpressionKind::PhysicsQueryObject);
  EXPECT_TRUE(HasRead(query, LN_RUNTIME_SEMANTIC_READ_QUERY_CACHE));
  EXPECT_EQ(query.fallback_requirements, LN_RUNTIME_FALLBACK_NONE);

  const LN_RuntimeExpressionSemantics file = LN_GetRuntimeExpressionSemantics(
      LN_ValueExpressionKind::LoadVariable);
  EXPECT_TRUE(HasRead(file, LN_RUNTIME_SEMANTIC_READ_FILE));
  EXPECT_EQ(file.threading, LN_RuntimeSemanticThreading::MainThreadRecord);
  EXPECT_TRUE(HasFallback(file, LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ));
  EXPECT_TRUE(HasFallback(file, LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP));

  const LN_RuntimeExpressionSemantics timer = LN_GetRuntimeExpressionSemantics(
      LN_BoolExpressionKind::TimerElapsed);
  EXPECT_TRUE(HasRead(timer, LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE));
  EXPECT_TRUE(HasWrite(timer, LN_RUNTIME_SEMANTIC_WRITE_RUNTIME_TREE_STATE));

  const LN_RuntimeExpressionSemantics tree_property = LN_GetRuntimeExpressionSemantics(
      LN_FloatExpressionKind::RuntimeTreeProperty);
  EXPECT_TRUE(HasRead(tree_property, LN_RUNTIME_SEMANTIC_READ_RUNTIME_TREE_STATE));
  EXPECT_FALSE(HasRead(tree_property, LN_RUNTIME_SEMANTIC_READ_SNAPSHOT));
  EXPECT_EQ(tree_property.writes, uint32_t(LN_RUNTIME_SEMANTIC_WRITE_NONE));
}

TEST(LN_Program, RuntimeFallbackRequirementsClassifyGenericAndDynamicCases)
{
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_BoolExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_FloatExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_IntExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_StringExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_VectorExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));
  EXPECT_TRUE(HasFallback(LN_GetRuntimeExpressionSemantics(
                              LN_ColorExpressionKind::FromGenericValue),
                          LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION));

  const LN_RuntimeExpressionSemantics object_by_name = LN_GetRuntimeExpressionSemantics(
      LN_ValueExpressionKind::ObjectByName);
  EXPECT_TRUE(HasRead(object_by_name, LN_RUNTIME_SEMANTIC_READ_SCENE));
  EXPECT_TRUE(HasFallback(object_by_name, LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP));

  const LN_RuntimeExpressionSemantics child_by_name = LN_GetRuntimeExpressionSemantics(
      LN_ValueExpressionKind::ObjectChildByName);
  EXPECT_TRUE(HasRead(child_by_name, LN_RUNTIME_SEMANTIC_READ_SNAPSHOT));
  EXPECT_TRUE(HasFallback(child_by_name, LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP));

  const LN_RuntimeExpressionSemantics collision = LN_GetRuntimeExpressionSemantics(
      LN_BoolExpressionKind::CollisionDetected);
  EXPECT_EQ(collision.threading, LN_RuntimeSemanticThreading::MainThreadRecord);
  EXPECT_TRUE(HasFallback(collision, LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ));

  const LN_RuntimeExpressionSemantics pure_math = LN_GetRuntimeExpressionSemantics(
      LN_FloatExpressionKind::Add);
  EXPECT_EQ(pure_math.fallback_requirements, LN_RUNTIME_FALLBACK_NONE);
}

TEST(LN_Program, DeduplicatesEquivalentConstantExpressions)
{
  LN_Program program;

  LN_BoolExpression bool_expression;
  bool_expression.kind = LN_BoolExpressionKind::Constant;
  bool_expression.bool_value = true;
  const uint32_t bool_index = program.AddBoolExpression(bool_expression);
  EXPECT_EQ(program.AddBoolExpression(bool_expression), bool_index);
  EXPECT_EQ(program.GetBoolExpressions().size(), 1u);

  LN_FloatExpression float_expression;
  float_expression.kind = LN_FloatExpressionKind::Constant;
  float_expression.float_value = 4.0f;
  const uint32_t float_index = program.AddFloatExpression(float_expression);
  EXPECT_EQ(program.AddFloatExpression(float_expression), float_index);
  EXPECT_EQ(program.GetFloatExpressions().size(), 1u);

  LN_StringExpression string_expression;
  string_expression.kind = LN_StringExpressionKind::Constant;
  string_expression.string_value = "score";
  const uint32_t string_index = program.AddStringExpression(string_expression);
  EXPECT_EQ(program.AddStringExpression(string_expression), string_index);
  EXPECT_EQ(program.GetStringExpressions().size(), 1u);
}

TEST(LN_Program, FoldsPureConstantExpressionsAtCompileTime)
{
  LN_Program program;

  LN_FloatExpression two;
  two.kind = LN_FloatExpressionKind::Constant;
  two.float_value = 2.0f;
  const uint32_t two_index = program.AddFloatExpression(two);

  LN_FloatExpression three;
  three.kind = LN_FloatExpressionKind::Constant;
  three.float_value = 3.0f;
  const uint32_t three_index = program.AddFloatExpression(three);

  LN_FloatExpression add;
  add.kind = LN_FloatExpressionKind::Add;
  add.input0 = two_index;
  add.input1 = three_index;
  const uint32_t five_index = program.AddFloatExpression(add);
  ASSERT_LT(five_index, program.GetFloatExpressions().size());
  EXPECT_EQ(program.GetFloatExpressions()[five_index].kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(program.GetFloatExpressions()[five_index].float_value, 5.0f);

  LN_BoolExpression compare;
  compare.kind = LN_BoolExpressionKind::FloatCompare;
  compare.input0 = five_index;
  compare.input1 = five_index;
  compare.float_compare_operation = LN_FloatCompareOperation::Equal;
  const uint32_t compare_index = program.AddBoolExpression(compare);
  ASSERT_LT(compare_index, program.GetBoolExpressions().size());
  EXPECT_EQ(program.GetBoolExpressions()[compare_index].kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(program.GetBoolExpressions()[compare_index].bool_value);

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = two_index;
  vector.input1 = three_index;
  vector.input2 = five_index;
  const uint32_t vector_index = program.AddVectorExpression(vector);
  ASSERT_LT(vector_index, program.GetVectorExpressions().size());
  EXPECT_EQ(program.GetVectorExpressions()[vector_index].kind, LN_VectorExpressionKind::Constant);
  EXPECT_FLOAT_EQ(program.GetVectorExpressions()[vector_index].vector_value.x(), 2.0f);
  EXPECT_FLOAT_EQ(program.GetVectorExpressions()[vector_index].vector_value.y(), 3.0f);
  EXPECT_FLOAT_EQ(program.GetVectorExpressions()[vector_index].vector_value.z(), 5.0f);

  EXPECT_TRUE(program.IsParallelEligible());
}

TEST(LN_Program, SchedulerSummaryTracksCommandEmittingWorldWrites)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  const LN_SchedulerSummary &summary = program->GetSchedulerSummary();

  EXPECT_TRUE(program->IsParallelEligible());
  EXPECT_EQ(program->GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::None);
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::WritesCommands);
  EXPECT_EQ(summary.required_phase, LN_SchedulerRequiredPhase::OnInit);
  EXPECT_TRUE(summary.emits_commands);
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_COMMAND_BUFFER));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_WORLD));
  EXPECT_FALSE(summary.uses_jolt_queries_or_contacts);
  EXPECT_TRUE(summary.reads_snapshot_only);
  EXPECT_EQ(summary.estimated_work_class, LN_EstimatedWorkClass::Small);
  EXPECT_TRUE(summary.worker_lane_eligible);
}

TEST(LN_Program, InstructionsUseCompactHeadersAndPayloadTable)
{
  LN_Program program;
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::Constant;
  expression.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_expr_index = program.AddVectorExpression(expression);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.source_ref_index = 0;
  instruction.flags = 7;
  instruction.vector_expr_index = vector_expr_index;
  instruction.vector_value = expression.vector_value;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(
      LN_Event::OnFixedUpdate);
  const std::vector<LN_Instruction> &payloads = program.GetInstructions(LN_Event::OnFixedUpdate);

  ASSERT_EQ(headers.size(), 1);
  ASSERT_EQ(payloads.size(), 1);
  EXPECT_LT(sizeof(LN_InstructionHeader), sizeof(LN_Instruction));

  EXPECT_EQ(headers[0].opcode, LN_OpCode::SetTransformVector);
  EXPECT_EQ(headers[0].source_ref_index, 0u);
  EXPECT_EQ(headers[0].payload_index, 0u);
  EXPECT_EQ(headers[0].flags, 7u);

  const LN_Instruction &payload = program.GetInstructionPayload(LN_Event::OnFixedUpdate,
                                                                headers[0]);
  EXPECT_EQ(payload.payload_kind, LN_InstructionPayloadKind::Transform);
  EXPECT_EQ(payload.command_payload_index, 0u);
  EXPECT_EQ(payload.vector_expr_index, vector_expr_index);
  EXPECT_LT((payload.vector_value - expression.vector_value).length(), 0.0001f);
  ASSERT_EQ(program.GetVectorCommandPayloads().size(), 1u);
  const LN_VectorCommandPayload &vector_payload = program.GetVectorCommandPayloads()[0];
  EXPECT_EQ(vector_payload.vector_expr_index, vector_expr_index);
  EXPECT_LT((vector_payload.vector_value - expression.vector_value).length(), 0.0001f);
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, GamePropertyCommandPayloadSeparatesTargetAndTypedValue)
{
  LN_Program program;

  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;

  LN_IntExpression int_expression;
  int_expression.kind = LN_IntExpressionKind::Constant;
  int_expression.int_value = 42;
  const uint32_t int_expr_index = program.AddIntExpression(int_expression);

  LN_ValueExpression object_expression;
  object_expression.kind = LN_ValueExpressionKind::Constant;
  object_expression.value.type = LN_ValueType::ObjectRef;
  object_expression.value.exists = true;
  object_expression.value.reference_name = "Player";
  const uint32_t object_expr_index = program.AddValueExpression(object_expression);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = program.AddGamePropertyRef(property_ref);
  instruction.property_value_type = LN_ValueType::Int;
  instruction.int_expr_index = int_expr_index;
  instruction.value_expr_index = object_expr_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  ASSERT_EQ(program.GetGamePropertyCommandPayloads().size(), 1u);
  const LN_GamePropertyCommandPayload &payload = program.GetGamePropertyCommandPayloads()[0];
  EXPECT_EQ(payload.property_ref_index, 0u);
  EXPECT_EQ(payload.value_type, LN_ValueType::Int);
  EXPECT_EQ(payload.object_value_expr_index, object_expr_index);
  EXPECT_EQ(payload.value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(payload.int_expr_index, int_expr_index);
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, RigidBodyConstraintPayloadValidationChecksConstraintObject)
{
  LN_Program program;

  LN_RigidBodyConstraintCommandPayload constraint_payload;
  constraint_payload.constraint_object_value_expr_index = 7;

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::AddPhysicsConstraint;
  instruction.command_payload_index = program.AddRigidBodyConstraintCommandPayload(
      constraint_payload);
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "rigid body constraint object frame value"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsInvalidExpressionBounds)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = 12;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("vector index"), std::string::npos);
}

TEST(LN_Program, InstructionPayloadValidationRejectsUnknownExpressionSemantics)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Count;
  program.AddBoolExpression(expression);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_TRUE(ContainsError(errors, "bool expression 0: kind is missing runtime semantics entry"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsInvalidVectorOperationOperands)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVelocityVector;
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("velocity vector operation"), std::string::npos);
}

TEST(LN_Program, InstructionPayloadValidationRejectsInvalidSourceRefs)
{
  LN_Program program;
  LN_SourceRef source_ref;
  source_ref.node_name = "Source";
  program.AddSourceRef(source_ref);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::Print;
  instruction.source_ref_index = 3;
  program.AddInstruction(LN_Event::OnInit, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("source ref index"), std::string::npos);
}

TEST(LN_Program, InstructionPayloadValidationRejectsPayloadTableBounds)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::Print;
  program.AddInstruction(LN_Event::OnInit, instruction);

  std::vector<LN_InstructionHeader> &headers = const_cast<std::vector<LN_InstructionHeader> &>(
      program.GetInstructionHeaders(LN_Event::OnInit));
  headers[0].payload_index = 7;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "payload index"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsPayloadHeaderDrift)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<LN_Instruction> &payloads = const_cast<std::vector<LN_Instruction> &>(
      program.GetInstructions(LN_Event::OnFixedUpdate));
  payloads[0].opcode = LN_OpCode::Print;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "payload opcode"));
  EXPECT_TRUE(ContainsError(errors, "payload kind"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsMissingPropertyValueType)
{
  LN_Program program;
  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Float;

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = program.AddGamePropertyRef(property_ref);
  instruction.property_value_type = LN_ValueType::None;
  instruction.value_expr_index = LN_INVALID_INDEX;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "missing a value type"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsInvalidCommandTargets)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTreeProperty;
  instruction.property_ref_index = 42;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "tree property ref index"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsInvalidQueryRuntimeState)
{
  LN_Program program;
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::Raycast;
  program.AddQueryExpression(query);

  std::vector<LN_QueryExpression> &queries = const_cast<std::vector<LN_QueryExpression> &>(
      program.GetQueryExpressions());
  queries[0].runtime_state_index = program.GetQueryRuntimeStateCount();

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "runtime state id"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsPhaseAndSchedulerDrift)
{
  LN_Program phase_program;
  LN_Instruction phase_instruction;
  phase_instruction.opcode = LN_OpCode::Print;
  phase_program.AddInstruction(LN_Event::OnFixedUpdate, phase_instruction);

  LN_SchedulerSummary &phase_summary = const_cast<LN_SchedulerSummary &>(
      phase_program.GetSchedulerSummary());
  phase_summary.required_phase = LN_SchedulerRequiredPhase::None;

  std::vector<std::string> phase_errors;
  EXPECT_FALSE(phase_program.ValidateInstructionPayloads(&phase_errors));
  EXPECT_TRUE(ContainsError(phase_errors, "phase restriction"));

  LN_Program scheduler_program;
  LN_Instruction scheduler_instruction;
  scheduler_instruction.opcode = LN_OpCode::SaveGame;
  scheduler_program.AddInstruction(LN_Event::OnFixedUpdate, scheduler_instruction);

  LN_SchedulerSummary &scheduler_summary = const_cast<LN_SchedulerSummary &>(
      scheduler_program.GetSchedulerSummary());
  scheduler_summary.worker_lane_eligible = true;
  scheduler_summary.main_thread_only_reason = LN_MainThreadOnlyReason::None;
  scheduler_summary.emits_commands = false;

  std::vector<std::string> scheduler_errors;
  EXPECT_FALSE(scheduler_program.ValidateInstructionPayloads(&scheduler_errors));
  EXPECT_TRUE(ContainsError(scheduler_errors, "scheduler restriction"));
  EXPECT_TRUE(ContainsError(scheduler_errors, "command-emitting"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsSemanticSchedulerResourceDrift)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  ASSERT_TRUE(HasResource(program.GetSchedulerSummary(), LN_SCHEDULER_RESOURCE_WORLD));

  LN_SchedulerSummary &summary = const_cast<LN_SchedulerSummary &>(
      program.GetSchedulerSummary());
  summary.resource_access &= ~LN_SCHEDULER_RESOURCE_WORLD;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "scheduler summary is missing semantic resource access"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsSemanticPersistenceResourceDrift)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SaveVariable;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  ASSERT_TRUE(HasResource(program.GetSchedulerSummary(), LN_SCHEDULER_RESOURCE_FILE));

  LN_SchedulerSummary &summary = const_cast<LN_SchedulerSummary &>(
      program.GetSchedulerSummary());
  summary.resource_access &= ~LN_SCHEDULER_RESOURCE_FILE;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(errors, "scheduler summary is missing semantic resource access"));
}

TEST(LN_Program, InstructionPayloadValidationRejectsSemanticExpressionResourceDrift)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::EventReceived;
  program.AddBoolExpression(expression);
  ASSERT_TRUE(HasResource(program.GetSchedulerSummary(), LN_SCHEDULER_RESOURCE_LOGIC_MANAGER));

  LN_SchedulerSummary &summary = const_cast<LN_SchedulerSummary &>(
      program.GetSchedulerSummary());
  summary.resource_access &= ~LN_SCHEDULER_RESOURCE_LOGIC_MANAGER;

  std::vector<std::string> errors;
  EXPECT_FALSE(program.ValidateInstructionPayloads(&errors));
  EXPECT_TRUE(ContainsError(
      errors, "bool expression 0: scheduler summary is missing semantic resource access"));
}

TEST(LN_Program, InstructionPayloadValidationAcceptsMixedInitAndFixedUpdatePhase)
{
  LN_Program program;

  LN_Instruction init_instruction;
  init_instruction.opcode = LN_OpCode::SetTreeProperty;
  LN_TreePropertyRef tree_property_ref;
  tree_property_ref.name = "score";
  tree_property_ref.value_type = LN_ValueType::Float;
  tree_property_ref.default_value = MakeFloatValue(1.0f);
  init_instruction.property_ref_index = program.AddTreePropertyRef(tree_property_ref);
  program.AddInstruction(LN_Event::OnInit, init_instruction);

  LN_Instruction fixed_instruction;
  fixed_instruction.opcode = LN_OpCode::SetGameProperty;
  LN_GamePropertyRef game_property_ref;
  game_property_ref.name = "score";
  game_property_ref.value_type = LN_ValueType::Float;
  game_property_ref.default_value = MakeFloatValue(0.0f);
  fixed_instruction.property_ref_index = program.AddGamePropertyRef(game_property_ref);
  fixed_instruction.property_value_type = LN_ValueType::Float;
  program.AddInstruction(LN_Event::OnFixedUpdate, fixed_instruction);

  EXPECT_EQ(program.GetSchedulerSummary().required_phase, LN_SchedulerRequiredPhase::OnFixedUpdate);
  EXPECT_TRUE(program.ValidateInstructionPayloads());
}

TEST(LN_Program, InternsRepeatedStaticNames)
{
  LN_Program program;

  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;
  const uint32_t first_property = program.AddGamePropertyRef(property_ref);
  const uint32_t second_property = program.AddGamePropertyRef(property_ref);

  LN_StringExpression first_string;
  first_string.kind = LN_StringExpressionKind::Constant;
  first_string.string_value = "score";
  const uint32_t first_string_index = program.AddStringExpression(first_string);

  LN_StringExpression second_string = first_string;
  const uint32_t second_string_index = program.AddStringExpression(second_string);

  ASSERT_EQ(first_property, second_property);
  ASSERT_EQ(program.GetGamePropertyRefs().size(), 1u);
  EXPECT_TRUE(program.GetGamePropertyRefs()[first_property].name_id.IsValid());
  EXPECT_EQ(program.GetString(program.GetGamePropertyRefs()[first_property].name_id), "score");
  EXPECT_EQ(program.GetStringExpressions()[first_string_index].string_id.index,
            program.GetStringExpressions()[second_string_index].string_id.index);
  EXPECT_EQ(program.GetStringTable().size(), 1u);
}

TEST(LN_Program, AssignsDenseQueryRuntimeStateIds)
{
  LN_Program program;

  LN_QueryExpression first_query;
  first_query.kind = LN_QueryExpressionKind::Raycast;
  first_query.cache_key = 5000;
  const uint32_t first_index = program.AddQueryExpression(first_query);

  LN_QueryExpression shared_query = first_query;
  const uint32_t shared_index = program.AddQueryExpression(shared_query);

  LN_QueryExpression independent_query;
  independent_query.kind = LN_QueryExpressionKind::MouseRay;
  const uint32_t independent_index = program.AddQueryExpression(independent_query);

  const std::vector<LN_QueryExpression> &queries = program.GetQueryExpressions();
  EXPECT_EQ(queries[first_index].runtime_state_index, 0u);
  EXPECT_EQ(queries[shared_index].runtime_state_index, 0u);
  EXPECT_EQ(queries[independent_index].runtime_state_index, 1u);
  EXPECT_EQ(program.GetQueryRuntimeStateCount(), 2u);
}

TEST(LN_Program, PropagatesRayQueryDetailsAcrossSharedRuntimeState)
{
  LN_Program program;

  LN_QueryExpression first_query;
  first_query.kind = LN_QueryExpressionKind::Raycast;
  first_query.cache_key = 5000;
  const uint32_t first_index = program.AddQueryExpression(first_query);

  program.AddRayQueryDetailRequirement(
      first_query.cache_key, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);

  LN_QueryExpression later_query = first_query;
  const uint32_t later_index = program.AddQueryExpression(later_query);

  const std::vector<LN_QueryExpression> &queries = program.GetQueryExpressions();
  EXPECT_EQ(queries[first_index].ray_query_detail_flags,
            LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
  EXPECT_EQ(queries[later_index].ray_query_detail_flags,
            LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
  EXPECT_EQ(queries[first_index].runtime_state_index, queries[later_index].runtime_state_index);
  EXPECT_EQ(program.GetDependencySummary().ray_query_detail_requirements,
            LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
}

TEST(LN_Program, ValidatesDenseSpawnPoolStateIds)
{
  LN_Program program;
  const uint32_t pool_index = program.AddSpawnPoolState();

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SpawnPoolCreate;
  instruction.int_value = int32_t(pool_index);
  program.AddInstruction(LN_Event::OnInit, instruction);

  EXPECT_TRUE(program.ValidateInstructionPayloads());

  LN_Program invalid_program;
  LN_Instruction invalid_instruction;
  invalid_instruction.opcode = LN_OpCode::SpawnPoolSpawn;
  invalid_instruction.int_value = 7;
  invalid_program.AddInstruction(LN_Event::OnFixedUpdate, invalid_instruction);

  std::vector<std::string> errors;
  EXPECT_FALSE(invalid_program.ValidateInstructionPayloads(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("spawn pool state id"), std::string::npos);
}

TEST(LN_Program, SchedulerSummaryTracksInputSnapshotReads)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::KeyboardActive;
  program.AddBoolExpression(expression);
  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();

  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::ReadsInput);
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_INPUT));
  EXPECT_FALSE(summary.emits_commands);
  EXPECT_TRUE(summary.reads_snapshot_only);
  EXPECT_TRUE(summary.worker_lane_eligible);
}

TEST(LN_Program, SchedulerSummaryTracksQueryExpressionReason)
{
  LN_Program program;
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::Raycast;
  program.AddQueryExpression(query);
  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();

  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::MainThreadOnly);
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_SNAPSHOT));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_QUERY_CACHE));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_PHYSICS));
  EXPECT_TRUE(summary.uses_jolt_queries_or_contacts);
  EXPECT_FALSE(summary.reads_snapshot_only);
  EXPECT_EQ(summary.estimated_work_class, LN_EstimatedWorkClass::Heavy);
  EXPECT_FALSE(summary.worker_lane_eligible);
  EXPECT_EQ(summary.main_thread_only_reason, LN_MainThreadOnlyReason::QueryExpression);
}

TEST(LN_Program, SchedulerSummaryTracksPersistenceResources)
{
  LN_Program program;
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::GetGlobalProperty;
  program.AddValueExpression(expression);
  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();

  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::MainThreadOnly);
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_GLOBAL_STATE));
  EXPECT_FALSE(summary.emits_commands);
  EXPECT_FALSE(summary.worker_lane_eligible);
  EXPECT_EQ(summary.main_thread_only_reason,
            LN_MainThreadOnlyReason::GlobalPropertyPersistence);
}

TEST(LN_Program, SchedulerSummaryTracksMainThreadCommandWrites)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SaveGame;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  const LN_SchedulerSummary &summary = program.GetSchedulerSummary();

  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(summary.purity, LN_SchedulerPurity::MainThreadOnly);
  EXPECT_TRUE(summary.emits_commands);
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_COMMAND_BUFFER));
  EXPECT_TRUE(HasResource(summary, LN_SCHEDULER_RESOURCE_FILE));
  EXPECT_FALSE(summary.worker_lane_eligible);
  EXPECT_EQ(summary.main_thread_only_reason, LN_MainThreadOnlyReason::FilePersistence);
}

TEST(LN_Program, SchedulerSummaryDescriptionIsPrintable)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::DrawLine;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const std::string description = program.DescribeSchedulerSummary();
  EXPECT_NE(description.find("purity=MainThreadOnly"), std::string::npos);
  EXPECT_NE(description.find("phase=OnFixedUpdate"), std::string::npos);
  EXPECT_NE(description.find("resources=Render"), std::string::npos);
  EXPECT_NE(description.find("worker_lane_eligible=false"), std::string::npos);
  EXPECT_NE(description.find("main_thread_only_reason=DebugDraw"), std::string::npos);
}

TEST(LN_Program, IsParallelEligibleAllowsSnapshotQueryExpressions)
{
  LN_Program program;
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::MouseRay;
  program.AddQueryExpression(query);
  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::None);
}

TEST(LN_Program, IsParallelEligibleRejectsCollisionBoolExpressions)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::CollisionDetected;
  program.AddBoolExpression(expression);
  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(),
            LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
}

TEST(LN_Program, CollisionDependenciesSeparatePairCacheFromContactDetails)
{
  LN_Program pair_program;
  LN_BoolExpression pair_expression;
  pair_expression.kind = LN_BoolExpressionKind::ObjectsColliding;
  pair_program.AddBoolExpression(pair_expression);
  EXPECT_FALSE(HasDependencyMask(pair_program.GetDependencySummary().query_channels,
                                 LN_DEP_QUERY_COLLISION_CONTACT));
  EXPECT_FALSE(HasDependencyMask(pair_program.GetDependencySummary().query_channels,
                                 LN_DEP_QUERY_COLLISION_CONTACT_DETAILS));
  EXPECT_EQ(pair_program.GetMainThreadOnlyReason(),
            LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);

  LN_Program event_program;
  LN_BoolExpression event_expression;
  event_expression.kind = LN_BoolExpressionKind::CollisionEnter;
  event_expression.int_value = 0;
  event_program.AddBoolExpression(event_expression);
  EXPECT_TRUE(HasDependencyMask(event_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT));
  EXPECT_FALSE(HasDependencyMask(event_program.GetDependencySummary().query_channels,
                                 LN_DEP_QUERY_COLLISION_CONTACT_DETAILS));

  LN_Program contact_event_program;
  LN_BoolExpression contact_event_expression;
  contact_event_expression.kind = LN_BoolExpressionKind::CollisionEnter;
  contact_event_expression.int_value = 2;
  contact_event_program.AddBoolExpression(contact_event_expression);
  EXPECT_TRUE(HasDependencyMask(contact_event_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT));
  EXPECT_TRUE(HasDependencyMask(contact_event_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT_DETAILS));

  LN_Program object_list_program;
  LN_ValueExpression object_list_expression;
  object_list_expression.kind = LN_ValueExpressionKind::CollisionHitObjects;
  object_list_program.AddValueExpression(object_list_expression);
  EXPECT_TRUE(HasDependencyMask(object_list_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT));
  EXPECT_FALSE(HasDependencyMask(object_list_program.GetDependencySummary().query_channels,
                                 LN_DEP_QUERY_COLLISION_CONTACT_DETAILS));

  LN_Program point_list_program;
  LN_ValueExpression point_list_expression;
  point_list_expression.kind = LN_ValueExpressionKind::CollisionHitPoints;
  point_list_program.AddValueExpression(point_list_expression);
  EXPECT_TRUE(HasDependencyMask(point_list_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT));
  EXPECT_TRUE(HasDependencyMask(point_list_program.GetDependencySummary().query_channels,
                                LN_DEP_QUERY_COLLISION_CONTACT_DETAILS));
}

TEST(LN_Program, IsParallelEligibleAllowsMouseOverBoolExpressions)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::MouseOverOver;
  program.AddBoolExpression(expression);
  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::None);
}

TEST(LN_Program, IsParallelEligibleRejectsObjectLifecycleInstructions)
{
  LN_Program program;
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::AddObject;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
  EXPECT_FALSE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::ImmediateCommandHelper);
}

TEST(LN_Program, IsParallelEligibleAllowsSnapshotBoolExpressions)
{
  LN_Program program;
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::KeyboardActive;
  program.AddBoolExpression(expression);
  EXPECT_TRUE(program.IsParallelEligible());
}

TEST(LN_Program, IsParallelEligibleRejectsFileAndGlobalPersistence)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SetGlobalProperty),
            LN_MainThreadOnlyReason::GlobalPropertyPersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SaveVariable),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SaveVariableDict),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::ClearVariables),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::RemoveVariable),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SaveGame), LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::LoadGame), LN_MainThreadOnlyReason::FilePersistence);
}

TEST(LN_Program, IsParallelEligibleRejectsPhysicsConstraintMutation)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::AddPhysicsConstraint),
            LN_MainThreadOnlyReason::PhysicsConstraint);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::RemovePhysicsConstraint),
            LN_MainThreadOnlyReason::PhysicsConstraint);
}

TEST(LN_Program, IsParallelEligibleRejectsMouseLookInputDeviceState)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::MouseLook),
            LN_MainThreadOnlyReason::InputDeviceState);
}

TEST(LN_Program, IsParallelEligibleRejectsSpawnPoolMutation)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SpawnPoolCreate), LN_MainThreadOnlyReason::SpawnPool);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SpawnPoolSpawn), LN_MainThreadOnlyReason::SpawnPool);
}

TEST(LN_Program, IsParallelEligibleRejectsCollectionAndOverlayMutation)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SetCollectionVisibility),
            LN_MainThreadOnlyReason::CollectionVisibility);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::SetOverlayCollection),
            LN_MainThreadOnlyReason::OverlayCollection);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::RemoveOverlayCollection),
            LN_MainThreadOnlyReason::OverlayCollection);
}

TEST(LN_Program, IsParallelEligibleRejectsImmediateDebugDrawMutation)
{
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawLine), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawArrow), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawPath), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawBox), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawMesh), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::DrawAxis), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::Navigate), LN_MainThreadOnlyReason::DebugDraw);
  EXPECT_EQ(ReasonForOpcode(LN_OpCode::FollowPath), LN_MainThreadOnlyReason::DebugDraw);
}

TEST(LN_Program, IsParallelEligibleRejectsFileAndGlobalValueExpressions)
{
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::GetGlobalProperty),
            LN_MainThreadOnlyReason::GlobalPropertyPersistence);
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::ListGlobalProperties),
            LN_MainThreadOnlyReason::GlobalPropertyPersistence);
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::LoadVariable),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::LoadVariableDict),
            LN_MainThreadOnlyReason::FilePersistence);
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::ListSavedVariables),
            LN_MainThreadOnlyReason::FilePersistence);
}

TEST(LN_Program, IsParallelEligibleRejectsLiveQueryValueExpressions)
{
  EXPECT_EQ(ReasonForValueExpression(LN_ValueExpressionKind::CollisionHitObject),
            LN_MainThreadOnlyReason::CollisionOrPhysicsQuery);
}

TEST(LN_Program, IsParallelEligibleAllowsSnapshotQueryValueExpressions)
{
  LN_Program program;
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ProjectileParabola;
  program.AddValueExpression(expression);

  EXPECT_TRUE(program.IsParallelEligible());
  EXPECT_EQ(program.GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::None);
}

TEST(LN_Program, DenseIdInternTablesReturnStableTypedIdsAndKeepDebugNames)
{
  LN_DenseIdRegistry registry;

  const LN_EventSubjectId first_subject = registry.InternEventSubject("bge.events.space");
  const LN_EventSubjectId second_subject = registry.InternEventSubject("bge.events.space");
  const LN_GamePropertyId property_id = registry.InternGameProperty("speed");
  const LN_TreePropertyId tree_property_id = registry.InternTreeProperty("ammo");
  const LN_MaterialSlotId material_id = registry.InternMaterialSlot("body");
  const LN_NodeSocketId socket_id = registry.InternNodeSocket("Principled BSDF.Base Color");
  const LN_ActionId action_id = registry.InternAction("Run");
  const LN_SoundId sound_id = registry.InternSound("laser");
  const LN_InputCodeId input_id = registry.InternInputCode("keyboard.space");

  EXPECT_TRUE(first_subject.IsValid());
  EXPECT_EQ(first_subject.index, second_subject.index);
  EXPECT_TRUE(property_id.IsValid());
  EXPECT_TRUE(tree_property_id.IsValid());
  EXPECT_TRUE(material_id.IsValid());
  EXPECT_TRUE(socket_id.IsValid());
  EXPECT_TRUE(action_id.IsValid());
  EXPECT_TRUE(sound_id.IsValid());
  EXPECT_TRUE(input_id.IsValid());
  EXPECT_EQ(registry.DebugName(first_subject), "bge.events.space");
  EXPECT_EQ(registry.DebugName(property_id), "speed");
  EXPECT_EQ(registry.DebugName(tree_property_id), "ammo");
  EXPECT_EQ(registry.DebugName(material_id), "body");
  EXPECT_EQ(registry.DebugName(socket_id), "Principled BSDF.Base Color");
  EXPECT_EQ(registry.DebugName(action_id), "Run");
  EXPECT_EQ(registry.DebugName(sound_id), "laser");
  EXPECT_EQ(registry.DebugName(input_id), "keyboard.space");
}

TEST(LN_Program, DenseHandlesResolveUntilInvalidatedAndReuseSlotsWithNewGenerations)
{
  LN_DenseIdRegistry registry;
  KX_GameObject *object_a = FakeGameObjectPointer(1);
  KX_GameObject *object_b = FakeGameObjectPointer(2);

  const LN_ObjectHandle first = registry.MakeObjectHandle(object_a, "Cube");
  const LN_ObjectHandle duplicate = registry.MakeObjectHandle(object_a, "Cube renamed");
  EXPECT_TRUE(first.IsValid());
  EXPECT_EQ(first.index, duplicate.index);
  EXPECT_EQ(first.generation, duplicate.generation);
  EXPECT_EQ(registry.ResolveHandle(first), object_a);
  EXPECT_EQ(registry.DebugName(first), "Cube");

  registry.InvalidateHandle(LN_DenseHandleKind::Object, object_a);
  EXPECT_EQ(registry.ResolveHandle(first), nullptr);
  EXPECT_EQ(registry.DebugName(first), "");

  const LN_ObjectHandle reused = registry.MakeObjectHandle(object_b, "Sphere");
  EXPECT_EQ(reused.index, first.index);
  EXPECT_NE(reused.generation, first.generation);
  EXPECT_EQ(registry.ResolveHandle(reused), object_b);
  EXPECT_EQ(registry.DebugName(reused), "Sphere");
}

TEST(LN_Program, DenseRuntimeTreeIdsInvalidateDeterministically)
{
  LN_DenseIdRegistry registry;
  int runtime_tree_a = 0;
  int runtime_tree_b = 0;

  const LN_RuntimeTreeId first = registry.MakeRuntimeTreeId(&runtime_tree_a, "Tree");
  EXPECT_TRUE(first.IsValid());
  EXPECT_EQ(registry.ResolveRuntimeTreeId(first), &runtime_tree_a);
  EXPECT_EQ(registry.DebugName(first), "Tree");

  registry.InvalidateRuntimeTree(&runtime_tree_a);
  EXPECT_EQ(registry.ResolveRuntimeTreeId(first), nullptr);
  EXPECT_EQ(registry.DebugName(first), "");

  const LN_RuntimeTreeId reused = registry.MakeRuntimeTreeId(&runtime_tree_b, "Tree.001");
  EXPECT_EQ(reused.index, first.index);
  EXPECT_NE(reused.generation, first.generation);
  EXPECT_EQ(registry.ResolveRuntimeTreeId(reused), &runtime_tree_b);
  EXPECT_EQ(registry.DebugName(reused), "Tree.001");
}

TEST(LN_Program, RuntimeTreeBindsProgramStringsAndPropertyRefsToSharedIds)
{
  LN_DenseIdRegistry registry;
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_StringExpression subject_expr;
  subject_expr.kind = LN_StringExpressionKind::Constant;
  subject_expr.string_value = "bge.events.space";
  subject_expr.string_id = program->InternString(subject_expr.string_value);
  const uint32_t subject_expr_index = program->AddStringExpression(subject_expr);

  LN_GamePropertyRef property_ref;
  property_ref.name = "speed";
  property_ref.name_id = program->InternString(property_ref.name);
  const uint32_t property_ref_index = program->AddGamePropertyRef(property_ref);

  LN_TreePropertyRef tree_property_ref;
  tree_property_ref.name = "ammo";
  tree_property_ref.name_id = program->InternString(tree_property_ref.name);
  const uint32_t tree_property_ref_index = program->AddTreePropertyRef(tree_property_ref);

  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  runtime_tree.BindDenseIds(registry);

  const LN_StringId subject_id = program->GetStringExpressions()[subject_expr_index].string_id;
  const LN_EventSubjectId event_subject_id = runtime_tree.GetEventSubjectId(subject_id);
  const LN_GamePropertyId game_property_id = runtime_tree.GetGamePropertyId(property_ref_index);
  const LN_TreePropertyId tree_property_id = runtime_tree.GetTreePropertyId(
      tree_property_ref_index);

  EXPECT_TRUE(runtime_tree.GetDenseRuntimeTreeId().IsValid());
  EXPECT_FALSE(runtime_tree.GetOwnerObjectHandle().IsValid());
  EXPECT_TRUE(event_subject_id.IsValid());
  EXPECT_TRUE(game_property_id.IsValid());
  EXPECT_TRUE(tree_property_id.IsValid());
  EXPECT_EQ(registry.DebugName(event_subject_id), "bge.events.space");
  EXPECT_EQ(registry.DebugName(game_property_id), "speed");
  EXPECT_EQ(registry.DebugName(tree_property_id), "ammo");
}

TEST(LN_Program, CommandAdaptersCarryStaticPropertyAndEventIds)
{
  LN_DenseIdRegistry registry;
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_StringExpression subject_expr;
  subject_expr.kind = LN_StringExpressionKind::Constant;
  subject_expr.string_value = "bge.events.space";
  subject_expr.string_id = program->InternString(subject_expr.string_value);

  LN_GamePropertyRef property_ref;
  property_ref.name = "speed";
  property_ref.name_id = program->InternString(property_ref.name);
  const uint32_t property_ref_index = program->AddGamePropertyRef(property_ref);

  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  runtime_tree.BindDenseIds(registry);

  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  value.float_value = 3.5f;

  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendSetGamePropertyRef(&runtime_tree, nullptr, property_ref_index, value, 1, 2);
  command_buffer.AppendSendEvent(&runtime_tree, subject_expr.string_id, value, nullptr, nullptr, 3, 4);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetGameProperty);
  EXPECT_TRUE(commands[0].game_property_id.IsValid());
  EXPECT_TRUE(commands[0].property_name.empty());
  ASSERT_NE(commands[0].property_name_ptr, nullptr);
  EXPECT_EQ(*commands[0].property_name_ptr, "speed");
  EXPECT_EQ(registry.DebugName(commands[0].game_property_id), "speed");

  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::SendEvent);
  EXPECT_TRUE(commands[1].event_subject_id.IsValid());
  EXPECT_TRUE(commands[1].property_name.empty());
  EXPECT_EQ(registry.DebugName(commands[1].event_subject_id), "bge.events.space");
}
