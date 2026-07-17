/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CommandDiagnostics.cpp
 *  \ingroup logicnodes
 */

#include "LN_CommandDiagnostics.h"

#include <memory>
#include <sstream>
#include <vector>

#include "KX_GameObject.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"

namespace {

const char *value_type_name(const LN_ValueType type)
{
  switch (type) {
    case LN_ValueType::None:
      return "None";
    case LN_ValueType::Bool:
      return "Bool";
    case LN_ValueType::Int:
      return "Int";
    case LN_ValueType::Float:
      return "Float";
    case LN_ValueType::String:
      return "String";
    case LN_ValueType::Vector:
      return "Vector";
    case LN_ValueType::Rotation:
      return "Rotation";
    case LN_ValueType::Vector4:
      return "Vector4";
    case LN_ValueType::Matrix:
      return "Matrix";
    case LN_ValueType::Color:
      return "Color";
    case LN_ValueType::ObjectRef:
      return "ObjectRef";
    case LN_ValueType::SceneRef:
      return "SceneRef";
    case LN_ValueType::CollectionRef:
      return "CollectionRef";
    case LN_ValueType::DatablockRef:
      return "DatablockRef";
    case LN_ValueType::List:
      return "List";
    case LN_ValueType::Dict:
      return "Dict";
    case LN_ValueType::Generic:
      return "Generic";
  }
  return "Unknown";
}

std::string vector_to_string(const MT_Vector3 &value)
{
  std::ostringstream stream;
  stream << '(' << value.x() << ", " << value.y() << ", " << value.z() << ')';
  return stream.str();
}

std::string color_to_string(const MT_Vector4 &value)
{
  std::ostringstream stream;
  stream << '(' << value.x() << ", " << value.y() << ", " << value.z() << ", " << value.w()
         << ')';
  return stream.str();
}

std::string target_name(const LN_CommandBuffer::Command &command)
{
  if (command.object == nullptr) {
    return "<none>";
  }
  return command.object->GetName();
}

const LN_SourceRef *command_source_ref(const LN_CommandBuffer::Command &command)
{
  if (command.runtime_tree == nullptr) {
    return nullptr;
  }
  const std::shared_ptr<const LN_Program> program = command.runtime_tree->GetProgram();
  if (program == nullptr) {
    return nullptr;
  }
  const std::vector<LN_SourceRef> &source_refs = program->GetSourceRefs();
  if (command.source_ref_index >= source_refs.size()) {
    return nullptr;
  }
  return &source_refs[command.source_ref_index];
}

void append_payload_summary(std::ostringstream &stream,
                            const LN_CommandBuffer::Command &command)
{
  bool has_entry = false;
  auto append_separator = [&]() {
    if (has_entry) {
      stream << ", ";
    }
    has_entry = true;
  };

  if (!command.property_name.empty()) {
    append_separator();
    stream << "name=\"" << command.property_name << "\"";
  }
  if (!command.secondary_property_name.empty()) {
    append_separator();
    stream << "secondary=\"" << command.secondary_property_name << "\"";
  }
  if (!command.tertiary_property_name.empty()) {
    append_separator();
    stream << "tertiary=\"" << command.tertiary_property_name << "\"";
  }
  if (!command.quaternary_property_name.empty()) {
    append_separator();
    stream << "quaternary=\"" << command.quaternary_property_name << "\"";
  }
  if (command.property_value.type != LN_ValueType::None) {
    append_separator();
    stream << "value_type=" << value_type_name(command.property_value.type);
  }
  if (command.property_name_id.IsValid()) {
    append_separator();
    stream << "string_id=" << command.property_name_id.index;
  }
  if (command.property_ref_index != LN_INVALID_INDEX) {
    append_separator();
    stream << "property_ref=" << command.property_ref_index;
  }
  if (command.runtime_ref.IsValid()) {
    append_separator();
    stream << "runtime_ref=" << int(command.runtime_ref.kind) << ':' << command.runtime_ref.slot
           << ':' << command.runtime_ref.generation;
    if (!command.runtime_ref.debug_name.empty()) {
      stream << " \"" << command.runtime_ref.debug_name << "\"";
    }
  }
  if (command.vector_value.length2() > 0.0f) {
    append_separator();
    stream << "vector=" << vector_to_string(command.vector_value);
  }
  if (command.secondary_vector_value.length2() > 0.0f) {
    append_separator();
    stream << "secondary_vector=" << vector_to_string(command.secondary_vector_value);
  }
  if (command.color_value.x() != 0.0f || command.color_value.y() != 0.0f ||
      command.color_value.z() != 0.0f || command.color_value.w() != 1.0f)
  {
    append_separator();
    stream << "color=" << color_to_string(command.color_value);
  }
  if (command.scalar_value != 0.0f) {
    append_separator();
    stream << "scalar=" << command.scalar_value;
  }
  if (command.int_value != 0) {
    append_separator();
    stream << "int=" << command.int_value;
  }
  if (command.secondary_int_value != 0) {
    append_separator();
    stream << "secondary_int=" << command.secondary_int_value;
  }
  if (command.bool_value) {
    append_separator();
    stream << "bool=true";
  }
  if (command.secondary_bool_value) {
    append_separator();
    stream << "secondary_bool=true";
  }
  append_separator();
  stream << "sort_key=" << command.sort_key;

  if (!has_entry) {
    stream << "none";
  }
}

}  // namespace

const char *LN_CommandTypeName(const LN_CommandBuffer::CommandType type)
{
  switch (type) {
    case LN_CommandBuffer::CommandType::SetWorldPosition:
      return "SetWorldPosition";
    case LN_CommandBuffer::CommandType::SetLocalPosition:
      return "SetLocalPosition";
    case LN_CommandBuffer::CommandType::SetWorldOrientation:
      return "SetWorldOrientation";
    case LN_CommandBuffer::CommandType::SetLocalOrientation:
      return "SetLocalOrientation";
    case LN_CommandBuffer::CommandType::SetWorldScale:
      return "SetWorldScale";
    case LN_CommandBuffer::CommandType::SetLocalScale:
      return "SetLocalScale";
    case LN_CommandBuffer::CommandType::SetLinearVelocity:
      return "SetLinearVelocity";
    case LN_CommandBuffer::CommandType::SetLocalLinearVelocity:
      return "SetLocalLinearVelocity";
    case LN_CommandBuffer::CommandType::SetAngularVelocity:
      return "SetAngularVelocity";
    case LN_CommandBuffer::CommandType::SetLocalAngularVelocity:
      return "SetLocalAngularVelocity";
    case LN_CommandBuffer::CommandType::ApplyImpulse:
      return "ApplyImpulse";
    case LN_CommandBuffer::CommandType::SetVisibility:
      return "SetVisibility";
    case LN_CommandBuffer::CommandType::SetObjectColor:
      return "SetObjectColor";
    case LN_CommandBuffer::CommandType::MakeLightUnique:
      return "MakeLightUnique";
    case LN_CommandBuffer::CommandType::SetLightColor:
      return "SetLightColor";
    case LN_CommandBuffer::CommandType::SetLightPower:
      return "SetLightPower";
    case LN_CommandBuffer::CommandType::SetLightShadow:
      return "SetLightShadow";
    case LN_CommandBuffer::CommandType::ApplyMovement:
      return "ApplyMovement";
    case LN_CommandBuffer::CommandType::ApplyRotation:
      return "ApplyRotation";
    case LN_CommandBuffer::CommandType::ApplyForce:
      return "ApplyForce";
    case LN_CommandBuffer::CommandType::ApplyTorque:
      return "ApplyTorque";
    case LN_CommandBuffer::CommandType::SetGameProperty:
      return "SetGameProperty";
    case LN_CommandBuffer::CommandType::AddObject:
      return "AddObject";
    case LN_CommandBuffer::CommandType::AddObjectFromRef:
      return "AddObjectFromRef";
    case LN_CommandBuffer::CommandType::SetParent:
      return "SetParent";
    case LN_CommandBuffer::CommandType::SetParentFromRef:
      return "SetParentFromRef";
    case LN_CommandBuffer::CommandType::RemoveParent:
      return "RemoveParent";
    case LN_CommandBuffer::CommandType::RemoveObject:
      return "RemoveObject";
    case LN_CommandBuffer::CommandType::SetGravity:
      return "SetGravity";
    case LN_CommandBuffer::CommandType::SetTimeScale:
      return "SetTimeScale";
    case LN_CommandBuffer::CommandType::SetActiveCamera:
      return "SetActiveCamera";
    case LN_CommandBuffer::CommandType::SetCameraFov:
      return "SetCameraFov";
    case LN_CommandBuffer::CommandType::SetCameraOrthoScale:
      return "SetCameraOrthoScale";
    case LN_CommandBuffer::CommandType::SetCollisionGroup:
      return "SetCollisionGroup";
    case LN_CommandBuffer::CommandType::SetPhysics:
      return "SetPhysics";
    case LN_CommandBuffer::CommandType::SetDynamics:
      return "SetDynamics";
    case LN_CommandBuffer::CommandType::RebuildCollisionShape:
      return "RebuildCollisionShape";
    case LN_CommandBuffer::CommandType::SetRigidBodyAttribute:
      return "SetRigidBodyAttribute";
    case LN_CommandBuffer::CommandType::CharacterJump:
      return "CharacterJump";
    case LN_CommandBuffer::CommandType::SetCharacterGravity:
      return "SetCharacterGravity";
    case LN_CommandBuffer::CommandType::SetCharacterJumpSpeed:
      return "SetCharacterJumpSpeed";
    case LN_CommandBuffer::CommandType::SetCharacterMaxJumps:
      return "SetCharacterMaxJumps";
    case LN_CommandBuffer::CommandType::SetCharacterWalkDirection:
      return "SetCharacterWalkDirection";
    case LN_CommandBuffer::CommandType::SetCharacterVelocity:
      return "SetCharacterVelocity";
    case LN_CommandBuffer::CommandType::VehicleControl:
      return "VehicleControl";
    case LN_CommandBuffer::CommandType::VehicleApplyEngineForce:
      return "VehicleApplyEngineForce";
    case LN_CommandBuffer::CommandType::VehicleApplyBraking:
      return "VehicleApplyBraking";
    case LN_CommandBuffer::CommandType::VehicleApplySteering:
      return "VehicleApplySteering";
    case LN_CommandBuffer::CommandType::SetVehicleSuspensionCompression:
      return "SetVehicleSuspensionCompression";
    case LN_CommandBuffer::CommandType::SetVehicleSuspensionStiffness:
      return "SetVehicleSuspensionStiffness";
    case LN_CommandBuffer::CommandType::SetVehicleSuspensionDamping:
      return "SetVehicleSuspensionDamping";
    case LN_CommandBuffer::CommandType::SetVehicleWheelFriction:
      return "SetVehicleWheelFriction";
    case LN_CommandBuffer::CommandType::SetTreeProperty:
      return "SetTreeProperty";
    case LN_CommandBuffer::CommandType::SetCursorVisibility:
      return "SetCursorVisibility";
    case LN_CommandBuffer::CommandType::SetCursorPosition:
      return "SetCursorPosition";
    case LN_CommandBuffer::CommandType::SetGamepadVibration:
      return "SetGamepadVibration";
    case LN_CommandBuffer::CommandType::SetWindowSize:
      return "SetWindowSize";
    case LN_CommandBuffer::CommandType::SetFullscreen:
      return "SetFullscreen";
    case LN_CommandBuffer::CommandType::SetVSync:
      return "SetVSync";
    case LN_CommandBuffer::CommandType::SetShowFramerate:
      return "SetShowFramerate";
    case LN_CommandBuffer::CommandType::SetShowProfile:
      return "SetShowProfile";
    case LN_CommandBuffer::CommandType::SubsystemCommand:
      return "SubsystemCommand";
    case LN_CommandBuffer::CommandType::Print:
      return "Print";
    case LN_CommandBuffer::CommandType::QuitGame:
      return "QuitGame";
    case LN_CommandBuffer::CommandType::RestartGame:
      return "RestartGame";
    case LN_CommandBuffer::CommandType::LoadBlendFile:
      return "LoadBlendFile";
    case LN_CommandBuffer::CommandType::PlayAction:
      return "PlayAction";
    case LN_CommandBuffer::CommandType::StopAction:
      return "StopAction";
    case LN_CommandBuffer::CommandType::SetActionFrame:
      return "SetActionFrame";
    case LN_CommandBuffer::CommandType::StopAllSounds:
      return "StopAllSounds";
    case LN_CommandBuffer::CommandType::PlaySound:
      return "PlaySound";
    case LN_CommandBuffer::CommandType::PlaySound3D:
      return "PlaySound3D";
    case LN_CommandBuffer::CommandType::PauseSound:
      return "PauseSound";
    case LN_CommandBuffer::CommandType::ResumeSound:
      return "ResumeSound";
    case LN_CommandBuffer::CommandType::StopSound:
      return "StopSound";
    case LN_CommandBuffer::CommandType::SetLogicTreeEnabled:
      return "SetLogicTreeEnabled";
    case LN_CommandBuffer::CommandType::InstallLogicTree:
      return "InstallLogicTree";
    case LN_CommandBuffer::CommandType::SendEvent:
      return "SendEvent";
    case LN_CommandBuffer::CommandType::MoveToward:
      return "MoveToward";
    case LN_CommandBuffer::CommandType::SlowFollow:
      return "SlowFollow";
    case LN_CommandBuffer::CommandType::LoadScene:
      return "LoadScene";
    case LN_CommandBuffer::CommandType::SetScene:
      return "SetScene";
    case LN_CommandBuffer::CommandType::SaveGame:
      return "SaveGame";
    case LN_CommandBuffer::CommandType::LoadGame:
      return "LoadGame";
    case LN_CommandBuffer::CommandType::AlignAxisToVector:
      return "AlignAxisToVector";
    case LN_CommandBuffer::CommandType::RotateToward:
      return "RotateToward";
    case LN_CommandBuffer::CommandType::ReplaceMesh:
      return "ReplaceMesh";
    case LN_CommandBuffer::CommandType::CopyProperty:
      return "CopyProperty";
    case LN_CommandBuffer::CommandType::SetBonePoseLocation:
      return "SetBonePoseLocation";
    case LN_CommandBuffer::CommandType::SetBonePoseRotation:
      return "SetBonePoseRotation";
    case LN_CommandBuffer::CommandType::SetBonePoseScale:
      return "SetBonePoseScale";
    case LN_CommandBuffer::CommandType::SetBonePoseTransform:
      return "SetBonePoseTransform";
    case LN_CommandBuffer::CommandType::SetBoneAttribute:
      return "SetBoneAttribute";
    case LN_CommandBuffer::CommandType::SetBoneConstraintInfluence:
      return "SetBoneConstraintInfluence";
    case LN_CommandBuffer::CommandType::SetBoneConstraintTarget:
      return "SetBoneConstraintTarget";
    case LN_CommandBuffer::CommandType::SetBoneConstraintAttribute:
      return "SetBoneConstraintAttribute";
    case LN_CommandBuffer::CommandType::SetMaterialSlot:
      return "SetMaterialSlot";
    case LN_CommandBuffer::CommandType::SetMaterialParameter:
      return "SetMaterialParameter";
    case LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue:
      return "SetMaterialNodeSocketValue";
    case LN_CommandBuffer::CommandType::SetGeometryNodesInput:
      return "SetGeometryNodesInput";
    case LN_CommandBuffer::CommandType::SetGeometryNodeSocketValue:
      return "SetGeometryNodeSocketValue";
    case LN_CommandBuffer::CommandType::SetCompositorNodeSocketValue:
      return "SetCompositorNodeSocketValue";
    case LN_CommandBuffer::CommandType::MakeNodeTreeUnique:
      return "MakeNodeTreeUnique";
    case LN_CommandBuffer::CommandType::SetNodeMute:
      return "SetNodeMute";
    case LN_CommandBuffer::CommandType::EnableDisableModifier:
      return "EnableDisableModifier";
  }
  return "Unknown";
}

const char *LN_CommandSubsystemName(const LN_CommandBuffer::CommandSubsystem subsystem)
{
  switch (subsystem) {
    case LN_CommandBuffer::CommandSubsystem::ObjectTransform:
      return "ObjectTransform";
    case LN_CommandBuffer::CommandSubsystem::ObjectPhysics:
      return "ObjectPhysics";
    case LN_CommandBuffer::CommandSubsystem::ObjectState:
      return "ObjectState";
    case LN_CommandBuffer::CommandSubsystem::ObjectProperty:
      return "ObjectProperty";
    case LN_CommandBuffer::CommandSubsystem::ObjectLifecycle:
      return "ObjectLifecycle";
    case LN_CommandBuffer::CommandSubsystem::SceneState:
      return "SceneState";
    case LN_CommandBuffer::CommandSubsystem::Parenting:
      return "Parenting";
    case LN_CommandBuffer::CommandSubsystem::Animation:
      return "Animation";
    case LN_CommandBuffer::CommandSubsystem::Sound:
      return "Sound";
    case LN_CommandBuffer::CommandSubsystem::Camera:
      return "Camera";
    case LN_CommandBuffer::CommandSubsystem::Light:
      return "Light";
    case LN_CommandBuffer::CommandSubsystem::Render:
      return "Render";
    case LN_CommandBuffer::CommandSubsystem::Window:
      return "Window";
    case LN_CommandBuffer::CommandSubsystem::Datablock:
      return "Datablock";
    case LN_CommandBuffer::CommandSubsystem::Collection:
      return "Collection";
    case LN_CommandBuffer::CommandSubsystem::Geometry:
      return "Geometry";
    case LN_CommandBuffer::CommandSubsystem::Group:
      return "Group";
    case LN_CommandBuffer::CommandSubsystem::Input:
      return "Input";
    case LN_CommandBuffer::CommandSubsystem::TreeState:
      return "TreeState";
    case LN_CommandBuffer::CommandSubsystem::GameLifecycle:
      return "GameLifecycle";
    case LN_CommandBuffer::CommandSubsystem::Events:
      return "Events";
  }
  return "Unknown";
}

std::string LN_DescribeCommandForDiagnostics(const LN_CommandBuffer::Command &command)
{
  std::ostringstream stream;
  const LN_CommandBuffer::CommandSubsystem subsystem =
      LN_CommandBuffer::GetCommandSubsystem(command);
  stream << "command=" << LN_CommandTypeName(command.type);
  stream << " subsystem=" << LN_CommandSubsystemName(subsystem);
  stream << " target=\"" << target_name(command) << "\"";
  stream << " payload={";
  append_payload_summary(stream, command);
  stream << '}';

  if (const LN_SourceRef *source_ref = command_source_ref(command)) {
    stream << " source={";
    if (!source_ref->source_tree_name.empty()) {
      stream << "tree=\"" << source_ref->source_tree_name << "\"";
    }
    if (!source_ref->node_name.empty()) {
      stream << " node=\"" << source_ref->node_name << "\"";
    }
    if (!source_ref->node_idname.empty()) {
      stream << " idname=\"" << source_ref->node_idname << "\"";
    }
    if (!source_ref->socket_name.empty()) {
      stream << " socket=\"" << source_ref->socket_name << "\"";
    }
    stream << '}';
  }

  return stream.str();
}

std::string LN_DescribeCommandFailure(const LN_CommandBuffer::Command &command,
                                      const std::string &reason)
{
  std::ostringstream stream;
  stream << "Logic Nodes command failed";
  if (!reason.empty()) {
    stream << ": " << reason;
  }
  stream << " (" << LN_DescribeCommandForDiagnostics(command) << ')';
  return stream.str();
}
