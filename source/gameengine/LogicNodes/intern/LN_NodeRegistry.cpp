/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_NodeRegistry.cpp
 *  \ingroup logicnodes
 */

#include "LN_NodeRegistry.h"

#include <utility>

#include "BLI_assert.h"

#include "nodes/NOD_logic_descriptors.hh"

namespace {

using blender::Span;
using blender::StringRef;
using blender::nodes::logic::ExecutionClass;
using blender::nodes::logic::NodeMetadata;
using blender::nodes::logic::PinMetadata;
using blender::nodes::logic::PinType;
using blender::nodes::logic::RequiredPhase;

struct NodeKindMapping {
  const char *idname;
  LN_NodeKind kind;
};

static constexpr NodeKindMapping node_kind_mappings[] = {
  {"LogicNativeOnInit", LN_NodeKind::EventOnInit},
  {"LogicNativeOnFixedUpdate", LN_NodeKind::EventOnFixedUpdate},
  {"LogicNativeOnUpdate", LN_NodeKind::EventOnFixedUpdate},
  {"LogicNativeOnNextFrame", LN_NodeKind::OnNextFrame},
  {"LogicNativeOnce", LN_NodeKind::Once},
  {"LogicNativeBooleanEdge", LN_NodeKind::BooleanEdge},
  {"LogicNativeBranch", LN_NodeKind::Branch},
  {"LogicNativeGate", LN_NodeKind::Gate},
  {"LogicNativeGateList", LN_NodeKind::GateList},
  {"LogicNativeValueChanged", LN_NodeKind::ValueChanged},
  {"LogicNativeValueChangedTo", LN_NodeKind::ValueChangedTo},
  {"LogicNativeDelay", LN_NodeKind::Delay},
  {"LogicNativeTimer", LN_NodeKind::Timer},
  {"LogicNativePulsify", LN_NodeKind::Pulsify},
  {"LogicNativeBarrier", LN_NodeKind::Barrier},
  {"LogicNativeCooldown", LN_NodeKind::Cooldown},
  {"LogicNativeLimitRange", LN_NodeKind::LimitRange},
  {"LogicNativeRandomValue", LN_NodeKind::RandomValue},
  {"LogicNativeValueSwitch", LN_NodeKind::ValueSwitch},
  {"LogicNativeStoreValue", LN_NodeKind::StoreValue},
  {"LogicNativeHasProperty", LN_NodeKind::HasProperty},
  {"LogicNativeToggleProperty", LN_NodeKind::ToggleProperty},
  {"LogicNativeModifyProperty", LN_NodeKind::ModifyProperty},
  {"LogicNativeModifyPropertyClamped", LN_NodeKind::ModifyPropertyClamped},
  {"LogicNativeNone", LN_NodeKind::IsNone},
  {"LogicNativeNotNone", LN_NodeKind::NotNone},
  {"LogicNativeGetTreeProperty", LN_NodeKind::GetTreeProperty},
  {"LogicNativeSetTreeProperty", LN_NodeKind::SetTreeProperty},
  {"LogicNativeToggleTreeProperty", LN_NodeKind::ToggleTreeProperty},
  {"LogicNativeGetGlobalProperty", LN_NodeKind::GetGlobalProperty},
  {"LogicNativeSetGlobalProperty", LN_NodeKind::SetGlobalProperty},
  {"LogicNativeListGlobalProperties", LN_NodeKind::ListGlobalProperties},
  {"LogicNativeLoadVariable", LN_NodeKind::LoadVariable},
  {"LogicNativeSaveVariable", LN_NodeKind::SaveVariable},
  {"LogicNativeLoadVariableDict", LN_NodeKind::LoadVariableDict},
  {"LogicNativeSaveVariableDict", LN_NodeKind::SaveVariableDict},
  {"LogicNativeClearVariables", LN_NodeKind::ClearVariables},
  {"LogicNativeListSavedVariables", LN_NodeKind::ListSavedVariables},
  {"LogicNativeRemoveVariable", LN_NodeKind::RemoveVariable},
  {"LogicNativeKeyboardKey", LN_NodeKind::KeyboardKey},
  {"LogicNativeKeyboardActive", LN_NodeKind::KeyboardActive},
  {"LogicNativeKeyCode", LN_NodeKind::KeyCode},
  {"LogicNativeKeyLogger", LN_NodeKind::KeyLogger},
  {"LogicNativeMouseButton", LN_NodeKind::MouseButton},
  {"LogicNativeMouseMoved", LN_NodeKind::MouseMoved},
  {"LogicNativeMouseWheel", LN_NodeKind::MouseWheel},
  {"LogicNativeCursorPosition", LN_NodeKind::CursorPosition},
  {"LogicNativeCursorMovement", LN_NodeKind::CursorMovement},
  {"LogicNativeSetCursorVisibility", LN_NodeKind::SetCursorVisibility},
  {"LogicNativeSetCursorPosition", LN_NodeKind::SetCursorPosition},
  {"LogicNativeGamepadActive", LN_NodeKind::GamepadActive},
  {"LogicNativeGamepadButton", LN_NodeKind::GamepadButton},
  {"LogicNativeGamepadStick", LN_NodeKind::GamepadStick},
  {"LogicNativeGamepadVibration", LN_NodeKind::GamepadVibration},
  {"LogicNativeGamepadLook", LN_NodeKind::GamepadLook},
  {"LogicNativeMouseLook", LN_NodeKind::MouseLook},
  {"LogicNativeRemoveObject", LN_NodeKind::RemoveObject},
  {"LogicNativeAddObject", LN_NodeKind::AddObject},
  {"LogicNativeSpawnPool", LN_NodeKind::SpawnPool},
  {"LogicNativeSetParent", LN_NodeKind::SetParent},
  {"LogicNativeFindObject", LN_NodeKind::FindObject},
  {"LogicNativeObjectByName", LN_NodeKind::ObjectByName},
  {"LogicNativeGetOwner", LN_NodeKind::GetOwner},
  {"LogicNativeGetParent", LN_NodeKind::GetParent},
  {"LogicNativeGetChild", LN_NodeKind::GetChild},
  {"LogicNativeGetChildByName", LN_NodeKind::GetChildByName},
  {"LogicNativeRemoveParent", LN_NodeKind::RemoveParent},
  {"LogicNativeMouseOver", LN_NodeKind::MouseOver},
  {"LogicNativeGetGravity", LN_NodeKind::GetGravity},
  {"LogicNativeSetGravity", LN_NodeKind::SetGravity},
  {"LogicNativeGetTimescale", LN_NodeKind::GetTimescale},
  {"LogicNativeSetTimescale", LN_NodeKind::SetTimescale},
  {"LogicNativeGetActiveCamera", LN_NodeKind::GetActiveCamera},
  {"LogicNativeSetCamera", LN_NodeKind::SetCamera},
  {"LogicNativeSetCameraFov", LN_NodeKind::SetCameraFov},
  {"LogicNativeSetCameraOrthoScale", LN_NodeKind::SetCameraOrthoScale},
  {"LogicNativeWorldToScreen", LN_NodeKind::WorldToScreen},
  {"LogicNativeScreenToWorld", LN_NodeKind::ScreenToWorld},
  {"LogicNativeGetFullscreen", LN_NodeKind::GetFullscreen},
  {"LogicNativeSetFullscreen", LN_NodeKind::SetFullscreen},
  {"LogicNativeGetResolution", LN_NodeKind::GetResolution},
  {"LogicNativeSetResolution", LN_NodeKind::SetResolution},
  {"LogicNativeGetVSync", LN_NodeKind::GetVSync},
  {"LogicNativeSetVSync", LN_NodeKind::SetVSync},
  {"LogicNativeShowFramerate", LN_NodeKind::ShowFramerate},
  {"LogicNativeShowProfile", LN_NodeKind::ShowProfile},
  {"LogicNativeGetCollisionGroup", LN_NodeKind::GetCollisionGroup},
  {"LogicNativeSetCollisionGroup", LN_NodeKind::SetCollisionGroup},
  {"LogicNativeSetPhysics", LN_NodeKind::SetPhysics},
  {"LogicNativeSetDynamics", LN_NodeKind::SetDynamics},
  {"LogicNativeRebuildCollisionShape", LN_NodeKind::RebuildCollisionShape},
  {"LogicNativeGetRigidBodyAttribute", LN_NodeKind::GetRigidBodyAttribute},
  {"LogicNativeSetRigidBodyAttribute", LN_NodeKind::SetRigidBodyAttribute},
  {"LogicNativeGetCharacterInfo", LN_NodeKind::GetCharacterInfo},
  {"LogicNativeCharacterJump", LN_NodeKind::CharacterJump},
  {"LogicNativeSetCharacterGravity", LN_NodeKind::SetCharacterGravity},
  {"LogicNativeSetCharacterJumpSpeed", LN_NodeKind::SetCharacterJumpSpeed},
  {"LogicNativeSetCharacterMaxJumps", LN_NodeKind::SetCharacterMaxJumps},
  {"LogicNativeSetCharacterWalkDirection", LN_NodeKind::SetCharacterWalkDirection},
  {"LogicNativeSetCharacterVelocity", LN_NodeKind::SetCharacterVelocity},
  {"LogicNativeVehicleControl", LN_NodeKind::VehicleControl},
  {"LogicNativeVehicleAccelerate", LN_NodeKind::VehicleAccelerate},
  {"LogicNativeVehicleBrake", LN_NodeKind::VehicleBrake},
  {"LogicNativeVehicleSteer", LN_NodeKind::VehicleSteer},
  {"LogicNativeVehicleSetAttributes", LN_NodeKind::VehicleSetAttributes},
  {"LogicNativeValueBool", LN_NodeKind::ValueBool},
  {"LogicNativeValueInt", LN_NodeKind::ValueInt},
  {"LogicNativeValueFloat", LN_NodeKind::ValueFloat},
  {"LogicNativeValueString", LN_NodeKind::ValueString},
  {"LogicNativeStringOperation", LN_NodeKind::StringOperation},
  {"LogicNativeFormattedString", LN_NodeKind::FormattedString},
  {"LogicNativeValueColor", LN_NodeKind::ValueColor},
  {"LogicNativeColorRGB", LN_NodeKind::ColorRGB},
  {"LogicNativeColorRGBA", LN_NodeKind::ColorRGBA},
  {"LogicNativeValueVector", LN_NodeKind::ValueVector},
  {"LogicNativeEuler", LN_NodeKind::Euler},
  {"LogicNativeSeparateEuler", LN_NodeKind::SeparateEuler},
  {"LogicNativeVectorToRotation", LN_NodeKind::VectorToRotation},
  {"LogicNativeCombineXY", LN_NodeKind::CombineXY},
  {"LogicNativeCombineXYZ", LN_NodeKind::CombineXYZ},
  {"LogicNativeCombineXYZW", LN_NodeKind::CombineXYZW},
  {"LogicNativeResizeVector", LN_NodeKind::ResizeVector},
  {"LogicNativeXYZToMatrix", LN_NodeKind::XYZToMatrix},
  {"LogicNativeMatrixToXYZ", LN_NodeKind::MatrixToXYZ},
  {"LogicNativeVectorRotate", LN_NodeKind::VectorRotate},
  {"LogicNativeFilePath", LN_NodeKind::FilePath},
  {"LogicNativeGetSound", LN_NodeKind::GetSound},
  {"LogicNativeGetImage", LN_NodeKind::GetImage},
  {"LogicNativeGetFont", LN_NodeKind::GetFont},
  {"LogicNativeGetObjectID", LN_NodeKind::GetObjectID},
  {"LogicNativeGetAxisVector", LN_NodeKind::GetAxisVector},
  {"LogicNativeSeparateXY", LN_NodeKind::SeparateXY},
  {"LogicNativeSeparateXYZ", LN_NodeKind::SeparateXYZ},
  {"LogicNativeInvertValue", LN_NodeKind::InvertValue},
  {"LogicNativeClampValue", LN_NodeKind::ClampValue},
  {"LogicNativeMapRange", LN_NodeKind::MapRange},
  {"LogicNativeCompare", LN_NodeKind::Compare},
  {"LogicNativeTypecast", LN_NodeKind::Typecast},
  {"LogicNativeValueValid", LN_NodeKind::ValueValid},
  {"LogicNativeThreshold", LN_NodeKind::Threshold},
  {"LogicNativeRangedThreshold", LN_NodeKind::RangedThreshold},
  {"LogicNativeWithinRange", LN_NodeKind::WithinRange},
  {"LogicNativeMath", LN_NodeKind::Math},
  {"LogicNativeRandomFloat", LN_NodeKind::RandomFloat},
  {"LogicNativeRandomInt", LN_NodeKind::RandomInt},
  {"LogicNativeRandomVector", LN_NodeKind::RandomVector},
  {"LogicNativeVectorMath", LN_NodeKind::VectorMath},
  {"LogicNativeGetGamePropertyInt", LN_NodeKind::GetGamePropertyInt},
  {"LogicNativeGetGamePropertyFloat", LN_NodeKind::GetGamePropertyFloat},
  {"LogicNativeGetGamePropertyBool", LN_NodeKind::GetGamePropertyBool},
  {"LogicNativeGetGamePropertyString", LN_NodeKind::GetGamePropertyString},
  {"LogicNativeGetLightColor", LN_NodeKind::GetLightColor},
  {"LogicNativeGetLightPower", LN_NodeKind::GetLightPower},
  {"LogicNativeApplyImpulse", LN_NodeKind::ApplyImpulse},
  {"LogicNativeMakeLightUnique", LN_NodeKind::MakeLightUnique},
  {"LogicNativeSetLightColor", LN_NodeKind::SetLightColor},
  {"LogicNativeSetLightPower", LN_NodeKind::SetLightPower},
  {"LogicNativeSetLightShadow", LN_NodeKind::SetLightShadow},
  {"LogicNativeApplyMovement", LN_NodeKind::ApplyMovement},
  {"LogicNativeApplyRotation", LN_NodeKind::ApplyRotation},
  {"LogicNativeApplyForce", LN_NodeKind::ApplyForce},
  {"LogicNativeApplyTorque", LN_NodeKind::ApplyTorque},
  {"LogicNativeSetGamePropertyInt", LN_NodeKind::SetGamePropertyInt},
  {"LogicNativeSetGamePropertyFloat", LN_NodeKind::SetGamePropertyFloat},
  {"LogicNativeSetGamePropertyBool", LN_NodeKind::SetGamePropertyBool},
  {"LogicNativeSetGamePropertyString", LN_NodeKind::SetGamePropertyString},
  {"LogicNativeRaycast", LN_NodeKind::Raycast},
  {"LogicNativeRaycastAll", LN_NodeKind::RaycastAll},
  {"LogicNativeShapeCast", LN_NodeKind::ShapeCast},
  {"LogicNativeShapeCastAll", LN_NodeKind::ShapeCastAll},
  {"LogicNativeTime", LN_NodeKind::Time},
  {"LogicNativeDeltaFactor", LN_NodeKind::DeltaFactor},
  {"LogicNativeStartLogicTree", LN_NodeKind::StartLogicTree},
  {"LogicNativeStopLogicTree", LN_NodeKind::StopLogicTree},
  {"LogicNativeRunLogicTree", LN_NodeKind::RunLogicTree},
  {"LogicNativeLogicTreeStatus", LN_NodeKind::LogicTreeStatus},
  {"LogicNativeInstallLogicTree", LN_NodeKind::InstallLogicTree},
  {"LogicNativePrint", LN_NodeKind::Print},
  {"LogicNativeQuitGame", LN_NodeKind::QuitGame},
  {"LogicNativeRestartGame", LN_NodeKind::RestartGame},
  {"LogicNativePlayAction", LN_NodeKind::PlayAction},
  {"LogicNativeStopAction", LN_NodeKind::StopAction},
  {"LogicNativeSetActionFrame", LN_NodeKind::SetActionFrame},
  {"LogicNativeActionDone", LN_NodeKind::ActionDone},
  {"LogicNativeObjectsColliding", LN_NodeKind::ObjectsColliding},
  {"LogicNativeStopAllSounds", LN_NodeKind::StopAllSounds},
  {"LogicNativePlaySound", LN_NodeKind::PlaySound},
  {"LogicNativePlaySound3D", LN_NodeKind::PlaySound3D},
  {"LogicNativeStartSpeaker", LN_NodeKind::StartSpeaker},
  {"LogicNativePauseSound", LN_NodeKind::PauseSound},
  {"LogicNativeResumeSound", LN_NodeKind::ResumeSound},
  {"LogicNativeStopSound", LN_NodeKind::StopSound},
  {"LogicNativeGetBoneHeadWorld", LN_NodeKind::GetBoneHeadWorld},
  {"LogicNativeGetBoneAttribute", LN_NodeKind::GetBoneAttribute},
  {"LogicNativeLoadBlendFile", LN_NodeKind::LoadBlendFile},
  {"LogicNativeSendEvent", LN_NodeKind::SendEvent},
  {"LogicNativeReceiveEvent", LN_NodeKind::ReceiveEvent},
  {"LogicNativeCollision", LN_NodeKind::Collision},
  {"LogicNativeOnCollision", LN_NodeKind::OnCollision},
  {"LogicNativeAnimationStatus", LN_NodeKind::AnimationStatus},
  {"LogicNativeMoveToward", LN_NodeKind::MoveToward},
  {"LogicNativeNavigate", LN_NodeKind::Navigate},
  {"LogicNativeFollowPath", LN_NodeKind::FollowPath},
  {"LogicNativeTranslate", LN_NodeKind::Translate},
  {"LogicNativeSlowFollow", LN_NodeKind::SlowFollow},
  {"LogicNativeLoadScene", LN_NodeKind::LoadScene},
  {"LogicNativeSetScene", LN_NodeKind::SetScene},
  {"LogicNativeGetScene", LN_NodeKind::GetScene},
  {"LogicNativeGetCollection", LN_NodeKind::GetCollection},
  {"LogicNativeGetCollectionObjects", LN_NodeKind::GetCollectionObjects},
  {"LogicNativeGetCollectionObjectNames", LN_NodeKind::GetCollectionObjectNames},
  {"LogicNativeSetCollectionVisibility", LN_NodeKind::SetCollectionVisibility},
  {"LogicNativeSetOverlayCollection", LN_NodeKind::SetOverlayCollection},
  {"LogicNativeRemoveOverlayCollection", LN_NodeKind::RemoveOverlayCollection},
  {"LogicNativeSaveGame", LN_NodeKind::SaveGame},
  {"LogicNativeLoadGame", LN_NodeKind::LoadGame},
  {"LogicNativeAlignAxisToVector", LN_NodeKind::AlignAxisToVector},
  {"LogicNativeRotateToward", LN_NodeKind::RotateToward},
  {"LogicNativeReplaceMesh", LN_NodeKind::ReplaceMesh},
  {"LogicNativeCopyProperty", LN_NodeKind::CopyProperty},
  {"LogicNativeGetObjectAttribute", LN_NodeKind::GetObjectAttribute},
  {"LogicNativeEvaluateProperty", LN_NodeKind::EvaluateProperty},
  {"LogicNativeSetObjectAttribute", LN_NodeKind::SetObjectAttribute},
  {"LogicNativeLoop", LN_NodeKind::Loop},
  {"LogicNativeLoopFromList", LN_NodeKind::LoopFromList},
  {"LogicNativeListLength", LN_NodeKind::ListLength},
  {"LogicNativeListGetItem", LN_NodeKind::ListGetItem},
  {"LogicNativeListRandomItem", LN_NodeKind::ListRandomItem},
  {"LogicNativeMakeList", LN_NodeKind::MakeList},
  {"LogicNativeListExtend", LN_NodeKind::ListExtend},
  {"LogicNativeDictGetKey", LN_NodeKind::DictGetKey},
  {"LogicNativeMakeDict", LN_NodeKind::MakeDict},
  {"LogicNativeDictLength", LN_NodeKind::DictLength},
  {"LogicNativeDictGetKeys", LN_NodeKind::DictGetKeys},
  {"LogicNativeGetBoneTailWorld", LN_NodeKind::GetBoneTailWorld},
  {"LogicNativeGetBoneLength", LN_NodeKind::GetBoneLength},
  {"LogicNativeGetBoneCenterWorld", LN_NodeKind::GetBoneCenterWorld},
  {"LogicNativeEmptyList", LN_NodeKind::EmptyList},
  {"LogicNativeEmptyDict", LN_NodeKind::EmptyDict},
  {"LogicNativeDictHasKey", LN_NodeKind::DictHasKey},
  {"LogicNativeListDuplicate", LN_NodeKind::ListDuplicate},
  {"LogicNativeDictMerge", LN_NodeKind::DictMerge},
  {"LogicNativeListContains", LN_NodeKind::ListContains},
  {"LogicNativeListAppend", LN_NodeKind::ListAppend},
  {"LogicNativeListRemoveIndex", LN_NodeKind::ListRemoveIndex},
  {"LogicNativeListRemoveValue", LN_NodeKind::ListRemoveValue},
  {"LogicNativeListSetIndex", LN_NodeKind::ListSetIndex},
  {"LogicNativeDictSetKey", LN_NodeKind::DictSetKey},
  {"LogicNativeDictRemoveKey", LN_NodeKind::DictRemoveKey},
  {"LogicNativeListFromItems", LN_NodeKind::ListFromItems},
  {"LogicNativeGetBoneHeadPoseWorld", LN_NodeKind::GetBoneHeadPoseWorld},
  {"LogicNativeGetBoneTailPoseWorld", LN_NodeKind::GetBoneTailPoseWorld},
  {"LogicNativeGetBoneCenterPoseWorld", LN_NodeKind::GetBoneCenterPoseWorld},
  {"LogicNativeGetBonePoseRotation", LN_NodeKind::GetBonePoseRotation},
  {"LogicNativeGetBonePoseScale", LN_NodeKind::GetBonePoseScale},
  {"LogicNativeGetBonePoseTransform", LN_NodeKind::GetBonePoseTransform},
  {"LogicNativeSetBonePoseLocation", LN_NodeKind::SetBonePoseLocation},
  {"LogicNativeSetBonePoseRotation", LN_NodeKind::SetBonePoseRotation},
  {"LogicNativeSetBonePoseScale", LN_NodeKind::SetBonePoseScale},
  {"LogicNativeSetBonePoseTransform", LN_NodeKind::SetBonePoseTransform},
  {"LogicNativeSetBoneAttribute", LN_NodeKind::SetBoneAttribute},
  {"LogicNativeSetBoneConstraintInfluence", LN_NodeKind::SetBoneConstraintInfluence},
  {"LogicNativeSetBoneConstraintTarget", LN_NodeKind::SetBoneConstraintTarget},
  {"LogicNativeSetBoneConstraintAttribute", LN_NodeKind::SetBoneConstraintAttribute},
  {"LogicNativeSetMaterialSlot", LN_NodeKind::SetMaterialSlot},
  {"LogicNativeGetMaterialFromSlot", LN_NodeKind::GetMaterialFromSlot},
  {"LogicNativeGetMaterialSlotCount", LN_NodeKind::GetMaterialSlotCount},
  {"LogicNativeGetMaterialName", LN_NodeKind::GetMaterialName},
  {"LogicNativeGetMaterialParameter", LN_NodeKind::GetMaterialParameter},
  {"LogicNativeSetMaterialParameter", LN_NodeKind::SetMaterialParameter},
  {"LogicNativeSetGeometryNodesInput", LN_NodeKind::SetGeometryNodesInput},
  {"LogicNativeGetEditorNodeValue", LN_NodeKind::GetEditorNodeValue},
  {"LogicNativeSetEditorNodeValue", LN_NodeKind::SetEditorNodeValue},
  {"LogicNativeMakeNodeTreeUnique", LN_NodeKind::MakeNodeTreeUnique},
  {"LogicNativeSetNodeMute", LN_NodeKind::SetNodeMute},
  {"LogicNativeEnableDisableModifier", LN_NodeKind::EnableDisableModifier},
  {"LogicNativeAssignGeometryNodesModifier", LN_NodeKind::AssignGeometryNodesModifier},
  {"LogicNativeGetNodeGroupSocketValue", LN_NodeKind::GetNodeGroupSocketValue},
  {"LogicNativeSetNodeGroupSocketValue", LN_NodeKind::SetNodeGroupSocketValue},
  {"LogicNativePlayMaterialSequence", LN_NodeKind::PlayMaterialSequence},
  {"LogicNativeDrawLine", LN_NodeKind::DrawLine},
  {"LogicNativeDrawCube", LN_NodeKind::DrawCube},
  {"LogicNativeDrawBox", LN_NodeKind::DrawBox},
  {"LogicNativeDraw", LN_NodeKind::Draw},
  {"LogicNativeJoinPath", LN_NodeKind::JoinPath},
  {"LogicNativeGetMasterFolder", LN_NodeKind::GetMasterFolder},
  {"LogicNativeLoadFileContent", LN_NodeKind::LoadFileContent},
  {"LogicNativeSetCustomCursor", LN_NodeKind::SetCustomCursor},
  {"LogicNativeMouseRay", LN_NodeKind::MouseRay},
  {"LogicNativeCameraRay", LN_NodeKind::CameraRay},
  {"LogicNativeEvaluateCurve", LN_NodeKind::EvaluateCurve},
  {"LogicNativeValueSwitchList", LN_NodeKind::ValueSwitchList},
  {"LogicNativeValueSwitchListCompare", LN_NodeKind::ValueSwitchListCompare},
  {"LogicNativeFormula", LN_NodeKind::Formula},
  {"LogicNativeTweenValue", LN_NodeKind::TweenValue},
  {"LogicNativeAddPhysicsConstraint", LN_NodeKind::AddPhysicsConstraint},
  {"LogicNativeGetRigidBodyConstraints", LN_NodeKind::GetRigidBodyConstraints},
  {"LogicNativeRemovePhysicsConstraint", LN_NodeKind::RemovePhysicsConstraint},
  {"LogicNativeProjectileRay", LN_NodeKind::ProjectileRay},
  {"LogicNativeGetDistance", LN_NodeKind::GetDistance},
  {"LogicNativeGetGroupCenterPosition", LN_NodeKind::GetGroupCenterPosition},
  {"LogicNativeApplyForceToTarget", LN_NodeKind::ApplyForceToTarget},
};

bool node_kind_for_idname(const StringRef idname, LN_NodeKind &r_kind)
{
  for (const NodeKindMapping &mapping : node_kind_mappings) {
    if (idname == mapping.idname) {
      r_kind = mapping.kind;
      return true;
    }
  }
  return false;
}

LN_ValueType value_type_for_pin(const PinType type)
{
  switch (type) {
    case PinType::Execution:
      return LN_ValueType::None;
    case PinType::Condition:
    case PinType::Bool:
      return LN_ValueType::Bool;
    case PinType::Int:
    case PinType::CollisionLayers:
      return LN_ValueType::Int;
    case PinType::Float:
      return LN_ValueType::Float;
    case PinType::String:
      return LN_ValueType::String;
    case PinType::Vector:
    case PinType::VectorXYAngle:
      return LN_ValueType::Vector;
    case PinType::Rotation:
      return LN_ValueType::Rotation;
    case PinType::Color:
      return LN_ValueType::Color;
    case PinType::Object:
      return LN_ValueType::ObjectRef;
    case PinType::List:
      return LN_ValueType::List;
    case PinType::Dictionary:
      return LN_ValueType::Dict;
    case PinType::GeometryTree:
    case PinType::Material:
    case PinType::Image:
    case PinType::Sound:
    case PinType::Font:
    case PinType::Text:
    case PinType::Mesh:
    case PinType::Datablock:
      return LN_ValueType::DatablockRef;
    case PinType::Collection:
      return LN_ValueType::CollectionRef;
    case PinType::Scene:
      return LN_ValueType::SceneRef;
    case PinType::Generic:
    case PinType::UI:
      return LN_ValueType::Generic;
    case PinType::Python:
      return LN_ValueType::None;
  }
  return LN_ValueType::None;
}

LN_PinKind pin_kind_for_pin(const PinType type)
{
  switch (type) {
    case PinType::Execution:
      return LN_PinKind::Execution;
    case PinType::Condition:
      return LN_PinKind::Condition;
    default:
      return LN_PinKind::Value;
  }
}

LN_RequiredPhase required_phase_for_metadata(const RequiredPhase phase)
{
  switch (phase) {
    case RequiredPhase::None:
      return LN_RequiredPhase::None;
    case RequiredPhase::OnInit:
      return LN_RequiredPhase::OnInit;
    case RequiredPhase::FixedUpdate:
      return LN_RequiredPhase::FixedUpdate;
  }
  return LN_RequiredPhase::None;
}

LN_ExecutionClass execution_class_for_metadata(const ExecutionClass execution_class)
{
  switch (execution_class) {
    case ExecutionClass::SnapshotReadOnly:
      return LN_ExecutionClass::SnapshotReadOnly;
    case ExecutionClass::CommandEmitting:
      return LN_ExecutionClass::CommandEmitting;
    case ExecutionClass::MainThreadOnly:
      return LN_ExecutionClass::MainThreadOnly;
  }
  return LN_ExecutionClass::SnapshotReadOnly;
}

std::vector<LN_PinDefinition> pin_definitions_for_metadata(const Span<PinMetadata> pins)
{
  std::vector<LN_PinDefinition> definitions;
  definitions.reserve(pins.size());
  for (const PinMetadata &pin : pins) {
    LN_PinDefinition definition;
    definition.name = pin.identifier;
    if (const char *socket_idname = pin_type_socket_idname(pin.type)) {
      definition.socket_idname = socket_idname;
    }
    definition.kind = pin_kind_for_pin(pin.type);
    definition.value_type = value_type_for_pin(pin.type);
    definition.requires_link = pin.requires_link;
    definitions.push_back(definition);
  }
  return definitions;
}

}  // namespace

LN_NodeRegistry::LN_NodeRegistry()
{
  const Span<NodeMetadata> metadata_items = blender::nodes::logic::logic_node_metadata();
  m_definitions.reserve(metadata_items.size());

  for (const NodeMetadata &metadata : metadata_items) {
    LN_NodeKind kind;
    if (!node_kind_for_idname(metadata.idname, kind)) {
      BLI_assert_unreachable();
      continue;
    }

    LN_NodeDefinition definition;
    definition.idname = metadata.idname;
    definition.kind = kind;
    definition.inputs = pin_definitions_for_metadata(metadata.inputs);
    definition.outputs = pin_definitions_for_metadata(metadata.outputs);
    definition.category = metadata.category;
    definition.ui_name = metadata.ui_name;
    definition.ui_description = metadata.ui_description;
    definition.has_side_effects = metadata.has_side_effects;
    definition.required_phase = required_phase_for_metadata(metadata.required_phase);
    definition.requires_jolt = metadata.requires_jolt;
    definition.future_pure_batchable = metadata.future_pure_batchable;
    definition.execution_class = execution_class_for_metadata(metadata.execution_class);

    m_definitions.push_back(std::move(definition));
  }
}

const LN_NodeRegistry &LN_NodeRegistry::GetBuiltin()
{
  static const LN_NodeRegistry registry;
  return registry;
}

const LN_NodeDefinition *LN_NodeRegistry::FindNodeDefinition(const std::string &idname) const
{
  for (const LN_NodeDefinition &definition : m_definitions) {
    if (definition.idname == idname) {
      return &definition;
    }
  }
  return nullptr;
}
