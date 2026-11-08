/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeBatch.cpp
 *  \ingroup logicnodes
 *
 * Which programs may be recorded in a tree batch (LN_TreeBatch.h). The lists below name the nodes
 * whose evaluation reads only the tree's own state, its snapshot, the tick's shared input, the
 * program and the scene (which a batch never changes), and whose writes are recorded commands or
 * the tree's own state (research audit of 2026-10-31, every case read). Anything else, and any
 * node added later, keeps its programs out of batches until it is added here.
 */

#include "LN_KindCatalog.h"
#include "LN_TreeBatch.h"

#include "LN_Columns.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "LN_CommandBuffer.h"
#include "LN_ExecBlockIR.h"
#include "LN_JobPool.h"
#include "LN_Performance.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"
#include "LN_Types.h"

namespace {

bool vector_end_is_fixed(const LN_Program &program, const uint32_t input)
{
  if (input == LN_INVALID_INDEX) {
    return true;
  }
  const std::vector<LN_VectorExpression> &vectors = program.GetVectorExpressions();
  if (input >= vectors.size()) {
    return false;
  }
  const LN_VectorExpression &expression = vectors[input];
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      return true;
    /* The tree's own snapshot: taken before the tick's trees record. */
    default:
      return false;
  }
}

bool value_is_owner(const LN_Program &program, const uint32_t input)
{
  if (input == LN_INVALID_INDEX) {
    return true;
  }
  const std::vector<LN_ValueExpression> &values = program.GetValueExpressions();
  return input < values.size() && values[input].kind == LN_ValueExpressionKind::OwnerObject;
}

bool value_is_constant_object(const LN_Program &program, const uint32_t input)
{
  const std::vector<LN_ValueExpression> &values = program.GetValueExpressions();
  return input != LN_INVALID_INDEX && input < values.size() &&
         values[input].kind == LN_ValueExpressionKind::Constant &&
         values[input].value.type == LN_ValueType::ObjectRef;
}

/** An object end of Get Distance: empty (the owner for end A, nothing for end B), the owner or a
 * constant object. */
bool object_end_is_fixed(const LN_Program &program, const uint32_t input)
{
  return value_is_owner(program, input) || value_is_constant_object(program, input);
}

bool end_is_fixed(const LN_Program &program,
                  const LN_DistanceEndpointMode mode,
                  const uint32_t input)
{
  return mode == LN_DistanceEndpointMode::Position ? vector_end_is_fixed(program, input) :
                                                     object_end_is_fixed(program, input);
}

/** Get Distance measures from or to the tree's own object (its pending moves would be applied
 * at once by the read). */
bool distance_reads_owner(const LN_Program &program, const LN_QueryExpression &expression)
{
  return (expression.distance_endpoint_a == LN_DistanceEndpointMode::Object &&
          value_is_owner(program, expression.input0)) ||
         (expression.distance_endpoint_b == LN_DistanceEndpointMode::Object &&
          expression.input1 != LN_INVALID_INDEX && value_is_owner(program, expression.input1));
}

/** UPBGE_LN_BATCH_WIDE (plan phase B0): 0 keeps the allow lists of step 7 only; anything else (the
 * default) also lets the reads of the plan's B0 audit join a batch. */
std::optional<int> &batch_wide_for_tests()
{
  static std::optional<int> value;
  return value;
}

bool batch_wide()
{
  if (const std::optional<int> &value = batch_wide_for_tests()) {
    return *value != 0;
  }
  static const bool wide = []() {
    const char *value = std::getenv("UPBGE_LN_BATCH_WIDE");
    return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
  }();
  return wide;
}

/** The constant text of a string input, or null when the input is not a constant. */
const std::string *constant_text(const LN_Program &program, const uint32_t input)
{
  const std::vector<LN_StringExpression> &strings = program.GetStringExpressions();
  if (input == LN_INVALID_INDEX || input >= strings.size() ||
      strings[input].kind != LN_StringExpressionKind::Constant)
  {
    return nullptr;
  }
  return &strings[input].string_value;
}

const char *bool_kind_refusal(const LN_BoolExpression &expression)
{
  switch (expression.kind) {
    case LN_BoolExpressionKind::Constant:
    case LN_BoolExpressionKind::GamePaused:
    case LN_BoolExpressionKind::Random:
    case LN_BoolExpressionKind::SnapshotGameProperty:
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
    case LN_BoolExpressionKind::RuntimeTreeProperty:
    case LN_BoolExpressionKind::Not:
    case LN_BoolExpressionKind::And:
    case LN_BoolExpressionKind::Or:
    case LN_BoolExpressionKind::FloatCompare:
    case LN_BoolExpressionKind::StringContains:
    case LN_BoolExpressionKind::StringStartsWith:
    case LN_BoolExpressionKind::StringEndsWith:
    case LN_BoolExpressionKind::ValueIsNone:
    case LN_BoolExpressionKind::FromGenericValue:
    case LN_BoolExpressionKind::FromInt:
    case LN_BoolExpressionKind::FromFloat:
    case LN_BoolExpressionKind::FromString:
    case LN_BoolExpressionKind::FromVector:
    case LN_BoolExpressionKind::FromColor:
    case LN_BoolExpressionKind::ValueCompare:
    case LN_BoolExpressionKind::DictHasKey:
    case LN_BoolExpressionKind::ListContains:
    case LN_BoolExpressionKind::StoreValueDone:
    case LN_BoolExpressionKind::InstructionExecuted:
    case LN_BoolExpressionKind::InstructionReached:
    case LN_BoolExpressionKind::InputStatus:
    case LN_BoolExpressionKind::KeyboardActive:
    case LN_BoolExpressionKind::MouseMoved:
    case LN_BoolExpressionKind::MouseWheelMoved:
    case LN_BoolExpressionKind::KeyLoggerPressed:
    case LN_BoolExpressionKind::GamepadActive:
    case LN_BoolExpressionKind::GamepadButton:
    case LN_BoolExpressionKind::MotionValid:
    case LN_BoolExpressionKind::TweenValue:
    case LN_BoolExpressionKind::TweenReached:
    case LN_BoolExpressionKind::TimerElapsed:
    case LN_BoolExpressionKind::DelayDone:
    case LN_BoolExpressionKind::PulsifyPulse:
    case LN_BoolExpressionKind::BarrierPassed:
    case LN_BoolExpressionKind::CooldownAccepted:
    case LN_BoolExpressionKind::CooldownBlocked:
    case LN_BoolExpressionKind::CooldownCompleted:
    case LN_BoolExpressionKind::CooldownReady:
    case LN_BoolExpressionKind::NextFrameDone:
    case LN_BoolExpressionKind::BooleanEdge:
    case LN_BoolExpressionKind::BooleanEdgeFalling:
    case LN_BoolExpressionKind::Once:
    case LN_BoolExpressionKind::FlowGateOpen:
    case LN_BoolExpressionKind::FlowRouterSelected:
    case LN_BoolExpressionKind::FlowRouterLastRoute:
    case LN_BoolExpressionKind::RepeatLimitAccepted:
    case LN_BoolExpressionKind::RepeatLimitBlocked:
    case LN_BoolExpressionKind::RepeatLimitExhausted:
    case LN_BoolExpressionKind::LoopActive:
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo:
    case LN_BoolExpressionKind::QueryValid:
    /* Another object's visibility: a field read of an object the batch does not change. */
    case LN_BoolExpressionKind::SnapshotVisibility:
      return nullptr;
    case LN_BoolExpressionKind::SnapshotCharacterOnGround:
      return expression.input0 == LN_INVALID_INDEX ? nullptr :
                                                     "another object's character state";
    default:
      return "a bool node that is not batch safe";
  }
}

const char *int_kind_refusal(const LN_IntExpression &expression)
{
  switch (expression.kind) {
    case LN_IntExpressionKind::Constant:
    case LN_IntExpressionKind::SnapshotGameProperty:
    case LN_IntExpressionKind::RuntimeTreeProperty:
    case LN_IntExpressionKind::StringCount:
    case LN_IntExpressionKind::SnapshotCollisionGroup:
    case LN_IntExpressionKind::MouseWheelDelta:
    case LN_IntExpressionKind::WindowResolutionWidth:
    case LN_IntExpressionKind::WindowResolutionHeight:
    case LN_IntExpressionKind::ListLength:
    case LN_IntExpressionKind::LoopIndex:
    case LN_IntExpressionKind::DictLength:
    case LN_IntExpressionKind::FromGenericValue:
    case LN_IntExpressionKind::Random:
    case LN_IntExpressionKind::GameRandomSeed:
    case LN_IntExpressionKind::FromBool:
    case LN_IntExpressionKind::FromFloat:
    case LN_IntExpressionKind::FromString:
    case LN_IntExpressionKind::RepeatLimitRemaining:
    case LN_IntExpressionKind::QueryVectorCount:
      return nullptr;
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps:
    case LN_IntExpressionKind::SnapshotCharacterJumpCount:
      return expression.input0 == LN_INVALID_INDEX ? nullptr :
                                                     "another object's character state";
    default:
      return "an integer node that is not batch safe";
  }
}

const char *float_kind_refusal(const LN_FloatExpression &expression)
{
  switch (expression.kind) {
    case LN_FloatExpressionKind::Constant:
    case LN_FloatExpressionKind::UnscaledDeltaTime:
    case LN_FloatExpressionKind::StepTimePassed:
    case LN_FloatExpressionKind::SnapshotGameProperty:
    case LN_FloatExpressionKind::RuntimeTreeProperty:
    case LN_FloatExpressionKind::Add:
    case LN_FloatExpressionKind::Subtract:
    case LN_FloatExpressionKind::Multiply:
    case LN_FloatExpressionKind::Divide:
    case LN_FloatExpressionKind::SafeDivide:
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
    case LN_FloatExpressionKind::StoreValue:
    case LN_FloatExpressionKind::SnapshotTimeScale:
    case LN_FloatExpressionKind::SnapshotElapsedTime:
    case LN_FloatExpressionKind::SnapshotFrameDelta:
    case LN_FloatExpressionKind::SnapshotFPS:
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
    case LN_FloatExpressionKind::FromGenericValue:
    case LN_FloatExpressionKind::GamepadButtonStrength:
    case LN_FloatExpressionKind::Formula:
    case LN_FloatExpressionKind::FloatCurve:
    case LN_FloatExpressionKind::TweenFactor:
    case LN_FloatExpressionKind::TweenFloatResult:
    case LN_FloatExpressionKind::CooldownRemaining:
    case LN_FloatExpressionKind::CooldownProgress:
    case LN_FloatExpressionKind::QueryDistance:
    case LN_FloatExpressionKind::QuerySquaredDistance:
    case LN_FloatExpressionKind::MotionSpeed:
    case LN_FloatExpressionKind::FromBool:
    case LN_FloatExpressionKind::FromInt:
    case LN_FloatExpressionKind::FromString:
    /* Two objects' positions read as they are (no pending move applied): the batch moves none. */
    case LN_FloatExpressionKind::ObjectDistance:
      return nullptr;
    case LN_FloatExpressionKind::SnapshotLightPower:
      return expression.input0 == LN_INVALID_INDEX ? nullptr : "another object's light";
    default:
      return "a float node that is not batch safe";
  }
}

const char *string_kind_refusal(const LN_StringExpression &expression)
{
  switch (expression.kind) {
    case LN_StringExpressionKind::Constant:
    case LN_StringExpressionKind::SnapshotGameProperty:
    case LN_StringExpressionKind::RuntimeTreeProperty:
    case LN_StringExpressionKind::Join:
    case LN_StringExpressionKind::Replace:
    case LN_StringExpressionKind::ToUppercase:
    case LN_StringExpressionKind::ToLowercase:
    case LN_StringExpressionKind::ZeroFill:
    case LN_StringExpressionKind::Format:
    case LN_StringExpressionKind::KeyLoggerCharacter:
    case LN_StringExpressionKind::KeyLoggerKeycode:
    case LN_StringExpressionKind::FromGenericValue:
    case LN_StringExpressionKind::FromBool:
    case LN_StringExpressionKind::FromInt:
    case LN_StringExpressionKind::FromFloat:
    case LN_StringExpressionKind::FromVector:
    case LN_StringExpressionKind::FromColor:
      return nullptr;
    default:
      return "a text node that is not batch safe";
  }
}

const char *vector_kind_refusal(const LN_VectorExpression &expression)
{
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
    /* Transforms of the owner (its snapshot) or of another object, read as they are. */
    case LN_VectorExpressionKind::SnapshotGravity:
    case LN_VectorExpressionKind::WindowResolution:
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
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
    case LN_VectorExpressionKind::Combine:
    case LN_VectorExpressionKind::RuntimeTreeProperty:
    case LN_VectorExpressionKind::FromGenericValue:
    case LN_VectorExpressionKind::CursorPosition:
    case LN_VectorExpressionKind::CursorMovement:
    case LN_VectorExpressionKind::GamepadStick:
    case LN_VectorExpressionKind::InstructionNextPoint:
    case LN_VectorExpressionKind::Random:
    case LN_VectorExpressionKind::TweenVectorResult:
    case LN_VectorExpressionKind::TweenRotationResult:
    case LN_VectorExpressionKind::MotionPosition:
    case LN_VectorExpressionKind::MotionVelocity:
    case LN_VectorExpressionKind::MotionDirection:
    case LN_VectorExpressionKind::MotionPredictedPosition:
    case LN_VectorExpressionKind::BroadcastFloat:
    case LN_VectorExpressionKind::BroadcastInt:
    case LN_VectorExpressionKind::FromColor:
      return nullptr;
    /* Velocities of another object go through its physics body. */
    case LN_VectorExpressionKind::SnapshotCharacterGravity:
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection:
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection:
      return expression.input0 == LN_INVALID_INDEX ? nullptr :
                                                     "another object's velocity or character";
    default:
      return "a vector node that is not batch safe";
  }
}

const char *color_kind_refusal(const LN_ColorExpression &expression)
{
  switch (expression.kind) {
    case LN_ColorExpressionKind::Constant:
    case LN_ColorExpressionKind::SnapshotObjectColor:
    case LN_ColorExpressionKind::RuntimeTreeProperty:
    case LN_ColorExpressionKind::Combine:
    case LN_ColorExpressionKind::FromGenericValue:
      return nullptr;
    case LN_ColorExpressionKind::SnapshotLightColor:
      return expression.input0 == LN_INVALID_INDEX ? nullptr : "another object's light";
    default:
      return "a color node that is not batch safe";
  }
}

const char *value_kind_refusal(const LN_Program &program, const LN_ValueExpression &expression)
{
  switch (expression.kind) {
    /* Get Object Attribute of the owner or of a constant object (plan phase B0): a transform,
     * the colour, the visibility, the name or a game property, read as it is. Nothing a batch
     * runs changes the scene and the read applies no pending move (unlike Get Distance), so it
     * gives the same answer on any thread. Velocities go through the physics body: not here. */
    case LN_ValueExpressionKind::ObjectAttribute: {
      if (!batch_wide()) {
        return "a value node that is not batch safe";
      }
      if (!object_end_is_fixed(program, expression.input0)) {
        return "an attribute of an object that is not the owner or a constant object";
      }
      const std::string *name = constant_text(program, expression.input1);
      if (name == nullptr || name->empty()) {
        return "an attribute that is chosen while the game runs";
      }
      if (name->find("elocity") != std::string::npos) {
        return "an object's velocity";
      }
      return nullptr;
    }
    case LN_ValueExpressionKind::Constant:
      switch (expression.value.type) {
        case LN_ValueType::SceneRef:
        case LN_ValueType::CollectionRef:
        case LN_ValueType::DatablockRef:
          return "a scene, collection or data reference";
        default:
          return nullptr;
      }
    case LN_ValueExpressionKind::RuntimeTreeProperty:
    case LN_ValueExpressionKind::Mix:
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
    case LN_ValueExpressionKind::OwnerObject:
    /* By name, through the manager's name lookup, built before a batch starts. */
    case LN_ValueExpressionKind::ObjectByName:
    case LN_ValueExpressionKind::LoopCurrentValue:
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
    case LN_ValueExpressionKind::ListAppend:
    case LN_ValueExpressionKind::ListRemoveIndex:
    case LN_ValueExpressionKind::ListRemoveValue:
    case LN_ValueExpressionKind::ListSetIndex:
    case LN_ValueExpressionKind::DictSetKey:
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue:
    case LN_ValueExpressionKind::ListFromItems:
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew:
    case LN_ValueExpressionKind::ListRandomItem:
    case LN_ValueExpressionKind::ValueSwitchList:
    case LN_ValueExpressionKind::ValueSwitchListCompare:
    case LN_ValueExpressionKind::ListSort:
    case LN_ValueExpressionKind::ListFindIndex:
    case LN_ValueExpressionKind::DictAddValue:
      return nullptr;
    default:
      return "a value node that is not batch safe";
  }
}

const char *query_refusal(const LN_Program &program, const LN_QueryExpression &expression)
{
  switch (expression.kind) {
    case LN_QueryExpressionKind::DistanceBetween: {
      const bool positions_only = expression.distance_endpoint_a ==
                                      LN_DistanceEndpointMode::Position &&
                                  expression.distance_endpoint_b ==
                                      LN_DistanceEndpointMode::Position;
      /* Positions only: no object's moves applied, whatever the inputs. With an object end it
       * must be taken once a pass, at a known moment (see the owner rule below). */
      if (positions_only || LN_DistanceQueryEndsAreFixed(program, expression)) {
        return nullptr;
      }
      return "a Get Distance whose ends can change during a pass";
    }
    case LN_QueryExpressionKind::Count:
      return nullptr;
    default:
      return "a ray, cast, pointer or UI query";
  }
}

template<typename Expression, typename Refusal>
const char *first_refusal(const std::vector<Expression> &expressions, Refusal refusal)
{
  for (const Expression &expression : expressions) {
    if (const char *reason = refusal(expression)) {
      return reason;
    }
  }
  return nullptr;
}

/** The ops whose commands move an object (IsObjectTransformCommand): a read of the moved object
 * applies them at once. */
bool op_moves_object(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::ApplyTransformVector:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
      return true;
    default:
      return false;
  }
}

/** The ops that read their target from the vector command payload (vector and physics vector
 * commands, LN_RuntimeTree's execute_direct_vector_command / execute_direct_physics_command). */
bool op_uses_vector_payload(const LN_OpCode opcode)
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
    case LN_OpCode::SetTransformVector:
    case LN_OpCode::SetVelocityVector:
    case LN_OpCode::ApplyTransformVector:
    case LN_OpCode::ApplyPhysicsVector:
    case LN_OpCode::ApplyMovement:
    case LN_OpCode::ApplyRotation:
    case LN_OpCode::ApplyForce:
    case LN_OpCode::ApplyTorque:
    case LN_OpCode::ApplyImpulse:
      return true;
    default:
      return false;
  }
}

uint32_t vector_payload_target(const LN_Program &program, const LN_Instruction &instruction)
{
  const std::vector<LN_VectorCommandPayload> &payloads = program.GetVectorCommandPayloads();
  return instruction.command_payload_index < payloads.size() ?
             payloads[instruction.command_payload_index].object_value_expr_index :
             instruction.value_expr_index;
}

/** The game property payload as the runtime reads it (game_property_command_payload). */
LN_GamePropertyCommandPayload game_property_payload(const LN_Program &program,
                                                     const LN_Instruction &instruction)
{
  const std::vector<LN_GamePropertyCommandPayload> &payloads =
      program.GetGamePropertyCommandPayloads();
  if (instruction.command_payload_index < payloads.size()) {
    return payloads[instruction.command_payload_index];
  }
  LN_GamePropertyCommandPayload payload;
  payload.property_ref_index = instruction.property_ref_index;
  payload.value_type = instruction.property_value_type;
  payload.bool_expr_index = instruction.bool_expr_index;
  payload.int_expr_index = instruction.int_expr_index;
  payload.float_expr_index = instruction.float_expr_index;
  payload.modify_op = LN_PropertyModifyOp(instruction.int_value);
  if (instruction.property_value_type != LN_ValueType::None &&
      (payload.modify_op != LN_PropertyModifyOp::None ||
       instruction.bool_expr_index != LN_INVALID_INDEX ||
       instruction.int_expr_index != LN_INVALID_INDEX ||
       instruction.float_expr_index != LN_INVALID_INDEX ||
       instruction.string_expr_index != LN_INVALID_INDEX ||
       instruction.vector_expr_index != LN_INVALID_INDEX ||
       instruction.color_expr_index != LN_INVALID_INDEX))
  {
    payload.object_value_expr_index = instruction.value_expr_index;
  }
  else {
    payload.value_expr_index = instruction.value_expr_index;
  }
  return payload;
}

const char *op_refusal(const LN_Program &program, const LN_Instruction &instruction)
{
  const LN_OpCode opcode = instruction.opcode;
  if (op_uses_vector_payload(opcode)) {
    /* Moves and physics of the tree's own object only: no other tree moves it, and it is not
     * measured by another tree of the batch. */
    return value_is_owner(program, vector_payload_target(program, instruction)) ?
               nullptr :
               "a move or physics change of another object";
  }
  switch (opcode) {
    case LN_OpCode::Nop:
    case LN_OpCode::BranchRoute:
    case LN_OpCode::ArmTimer:
    case LN_OpCode::ArmDelay:
    case LN_OpCode::ArmNextFrame:
    case LN_OpCode::UpdatePulsify:
    case LN_OpCode::UpdateBarrier:
    case LN_OpCode::TryOnce:
    case LN_OpCode::ResetOnce:
    case LN_OpCode::SetFlowGate:
    case LN_OpCode::TryFlowGate:
    case LN_OpCode::TryCooldown:
    case LN_OpCode::ResetCooldown:
    case LN_OpCode::TryFlowRouter:
    case LN_OpCode::ResetFlowRouter:
    case LN_OpCode::TryRepeatLimit:
    case LN_OpCode::ResetRepeatLimit:
    case LN_OpCode::ResetMotion:
    case LN_OpCode::BreakLoop:
    /* Recorded writes (the object reference resolved through the tree's own slots). */
    case LN_OpCode::SetTreeProperty:
    case LN_OpCode::SetVisibility:
    case LN_OpCode::SetObjectColor:
    case LN_OpCode::PlayAction:
    case LN_OpCode::StopAction:
    case LN_OpCode::SetActionFrame:
    case LN_OpCode::Print:
    /* A list or dictionary change of a game property: recorded, made at the flush (its value's
     * objects through the tree's own references, as Set Object Property's). */
    case LN_OpCode::EditGameProperty:
      return nullptr;
    case LN_OpCode::SampleMotion:
      /* Predict Motion of an object applies its pending moves; of a position it does not. */
      return LN_MotionSourceMode(instruction.secondary_int_value) == LN_MotionSourceMode::Position ?
                 nullptr :
                 "Predict Motion of an object";
    case LN_OpCode::SetGameProperty:
      /* The property's id comes from the tree (its reference): no shared name table written. */
      return game_property_payload(program, instruction).property_ref_index != LN_INVALID_INDEX ?
                 nullptr :
                 "a game property write without a property reference";
    case LN_OpCode::SendEvent: {
      /* A subject known at compile time: its id is the tree's own. */
      const std::vector<LN_StringExpression> &strings = program.GetStringExpressions();
      const uint32_t subject = instruction.string_expr_index;
      return subject < strings.size() &&
                     strings[subject].kind == LN_StringExpressionKind::Constant &&
                     strings[subject].string_id.IsValid() ?
                 nullptr :
                 "Send Event with a subject made while playing";
    }
    default:
      return "a node that is not batch safe";
  }
}

constexpr int k_max_expression_depth = 16;

bool float_certainly_evaluates(const LN_Program &program,
                               uint32_t index,
                               uint32_t query,
                               int depth);

/** Evaluating the bool expression always evaluates query `query` (no short cut skips it). */
bool bool_certainly_evaluates(const LN_Program &program,
                              const uint32_t index,
                              const uint32_t query,
                              const int depth)
{
  const std::vector<LN_BoolExpression> &bools = program.GetBoolExpressions();
  if (index >= bools.size() || depth > k_max_expression_depth) {
    return false;
  }
  const LN_BoolExpression &expression = bools[index];
  switch (expression.kind) {
    case LN_BoolExpressionKind::QueryValid:
      return expression.input0 == query;
    case LN_BoolExpressionKind::FloatCompare:
      /* Both sides are evaluated (the register path falls back to this when a side is a query). */
      return float_certainly_evaluates(program, expression.input0, query, depth + 1) ||
             float_certainly_evaluates(program, expression.input1, query, depth + 1);
    case LN_BoolExpressionKind::Not:
    case LN_BoolExpressionKind::And:
    case LN_BoolExpressionKind::Or:
      /* The first input is always evaluated, the second not after a short cut. */
      return bool_certainly_evaluates(program, expression.input0, query, depth + 1);
    default:
      return false;
  }
}

bool float_certainly_evaluates(const LN_Program &program,
                               const uint32_t index,
                               const uint32_t query,
                               const int depth)
{
  const std::vector<LN_FloatExpression> &floats = program.GetFloatExpressions();
  if (index >= floats.size() || depth > k_max_expression_depth) {
    return false;
  }
  const LN_FloatExpression &expression = floats[index];
  switch (expression.kind) {
    case LN_FloatExpressionKind::QueryDistance:
    case LN_FloatExpressionKind::QuerySquaredDistance:
      return expression.input0 == query;
    default:
      return false;
  }
}

/** Running the op always evaluates query `query` before anything it records. */
bool op_certainly_evaluates(const LN_Program &program,
                            const LN_Instruction &instruction,
                            const uint32_t query)
{
  switch (instruction.opcode) {
    case LN_OpCode::BranchRoute:
      return bool_certainly_evaluates(program, instruction.bool_expr_index, query, 0);
    case LN_OpCode::SetGameProperty: {
      /* Set Game Property Bool/Int/Float: the number is resolved before the target. */
      const LN_GamePropertyCommandPayload payload = game_property_payload(program, instruction);
      if (payload.property_ref_index == LN_INVALID_INDEX ||
          payload.modify_op != LN_PropertyModifyOp::None ||
          payload.value_expr_index != LN_INVALID_INDEX)
      {
        return false;
      }
      switch (payload.value_type) {
        case LN_ValueType::Bool:
          return bool_certainly_evaluates(program, payload.bool_expr_index, query, 0);
        case LN_ValueType::Float:
          return float_certainly_evaluates(program, payload.float_expr_index, query, 0);
        default:
          return false;
      }
    }
    default:
      return false;
  }
}

/** In each event, every Get Distance from the owner is taken before each move of the owner, by an
 * op that runs whenever that move runs (no guard, no Loop, the move's step gate or none). The
 * distance is then kept for the pass (LN_QUERY_PASS_KEY_BIT), so no read applies the tree's own
 * moves. */
const char *owner_reads_before_moves_refusal(const LN_Program &program,
                                             const std::vector<uint32_t> &owner_queries)
{
  for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(event);
    const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(event);
    struct Evaluation {
      uint32_t query;
      uint32_t step_gate;
    };
    std::vector<Evaluation> evaluations;
    for (const LN_ExecBlock &block : ir.blocks) {
      for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
           op_index++)
      {
        const LN_ExecOp &op = ir.ops[op_index];
        if (op.instruction_index >= headers.size()) {
          return "an op without its instruction";
        }
        const LN_Instruction &instruction = program.GetInstructionPayload(
            event, headers[op.instruction_index]);
        if (op_moves_object(op.opcode)) {
          /* Every move, not only the first: Every N Steps skips the blocks that are not due,
           * so a later move can be a step's first. */
          for (const uint32_t query : owner_queries) {
            const bool taken = std::any_of(
                evaluations.begin(), evaluations.end(), [&](const Evaluation &evaluation) {
                  return evaluation.query == query &&
                         (evaluation.step_gate == LN_INVALID_INDEX ||
                          evaluation.step_gate == block.step_gate_index);
                });
            if (!taken) {
              return "a Get Distance from the object read after the tree moves it";
            }
          }
          continue;
        }
        const bool always_runs = op.guard_bool_expr_index == LN_INVALID_INDEX &&
                                 block.loop_frame_index == LN_INVALID_INDEX &&
                                 block.completed_loop_frame_index == LN_INVALID_INDEX;
        if (!always_runs) {
          continue;
        }
        for (const uint32_t query : owner_queries) {
          if (op_certainly_evaluates(program, instruction, query)) {
            evaluations.push_back({query, block.step_gate_index});
          }
        }
      }
    }
  }
  return nullptr;
}

}  // namespace

bool LN_DistanceQueryEndsAreFixed(const LN_Program &program, const LN_QueryExpression &expression)
{
  return expression.kind == LN_QueryExpressionKind::DistanceBetween &&
         end_is_fixed(program, expression.distance_endpoint_a, expression.input0) &&
         end_is_fixed(program, expression.distance_endpoint_b, expression.input1);
}

LN_TreeBatchProfile LN_AnalyzeTreeBatchSafety(const LN_Program &program)
{
  LN_TreeBatchProfile profile;
  const auto refuse = [&](const char *reason) {
    profile.eligible = false;
    profile.reason = reason;
    profile.endpoint_value_expressions.clear();
    return profile;
  };

  /* UI event routes run again per event and while paused, with their own query scopes. */
  if (program.HasUIEventRoutes()) {
    return refuse("UI event routes");
  }

  const char *reason = first_refusal(program.GetBoolExpressions(), bool_kind_refusal);
  if (reason == nullptr) {
    reason = first_refusal(program.GetIntExpressions(), int_kind_refusal);
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetFloatExpressions(), float_kind_refusal);
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetStringExpressions(), string_kind_refusal);
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetVectorExpressions(), vector_kind_refusal);
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetColorExpressions(), color_kind_refusal);
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetValueExpressions(),
                           [&](const LN_ValueExpression &expression) {
                             return value_kind_refusal(program, expression);
                           });
  }
  if (reason == nullptr) {
    reason = first_refusal(program.GetQueryExpressions(),
                           [&](const LN_QueryExpression &expression) {
                             return query_refusal(program, expression);
                           });
  }
  if (reason != nullptr) {
    return refuse(reason);
  }

  bool moves_owner = false;
  bool has_loops = false;
  for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(event);
    const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(event);
    if (!headers.empty() && !ir.valid) {
      return refuse("routes without a compiled block program");
    }
    for (const LN_ExecBlock &block : ir.blocks) {
      if (block.loop_frame_index != LN_INVALID_INDEX ||
          block.completed_loop_frame_index != LN_INVALID_INDEX)
      {
        has_loops = true;
      }
    }
    for (const LN_ExecOp &op : ir.ops) {
      if (op.instruction_index >= headers.size()) {
        return refuse("an op without its instruction");
      }
      const LN_Instruction &instruction = program.GetInstructionPayload(
          event, headers[op.instruction_index]);
      if (instruction.opcode != op.opcode) {
        return refuse("an op that does not match its instruction");
      }
      if (const char *op_reason = op_refusal(program, instruction)) {
        return refuse(op_reason);
      }
      moves_owner = moves_owner || op_moves_object(op.opcode);
    }
  }

  std::vector<uint32_t> owner_queries;
  const std::vector<LN_QueryExpression> &queries = program.GetQueryExpressions();
  for (uint32_t index = 0; index < queries.size(); index++) {
    const LN_QueryExpression &query = queries[index];
    if (query.kind != LN_QueryExpressionKind::DistanceBetween) {
      continue;
    }
    if (distance_reads_owner(program, query)) {
      owner_queries.push_back(index);
    }
    for (const auto &[mode, input] : {std::pair(query.distance_endpoint_a, query.input0),
                                      std::pair(query.distance_endpoint_b, query.input1)})
    {
      if (mode == LN_DistanceEndpointMode::Object && value_is_constant_object(program, input) &&
          std::find(profile.endpoint_value_expressions.begin(),
                    profile.endpoint_value_expressions.end(),
                    input) == profile.endpoint_value_expressions.end())
      {
        profile.endpoint_value_expressions.push_back(input);
      }
    }
  }

  if (moves_owner && !owner_queries.empty()) {
    /* A Loop iteration is a scope of its own: a distance read there is taken again. */
    if (has_loops) {
      return refuse("a Loop in a tree that measures and moves its object");
    }
    if (const char *order_reason = owner_reads_before_moves_refusal(program, owner_queries)) {
      return refuse(order_reason);
    }
  }

  profile.reads_owner_distance = !owner_queries.empty();
  profile.moves_owner = moves_owner;
  profile.eligible = true;
  profile.reason.clear();
  return profile;
}

namespace {

std::optional<LN_TreeBatchMode> &tree_batch_mode_for_tests()
{
  static std::optional<LN_TreeBatchMode> mode;
  return mode;
}

LN_TreeBatchMode tree_batch_mode_from_environment()
{
  const char *value = std::getenv("UPBGE_LN_BATCHES");
  if (value == nullptr || value[0] == '\0') {
    return LN_TreeBatchMode::Auto;
  }
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
    return LN_TreeBatchMode::Off;
  }
  if (std::strcmp(value, "force") == 0) {
    return LN_TreeBatchMode::Force;
  }
  return LN_TreeBatchMode::Auto;
}

}  // namespace

namespace {
int parallel_capture_setting()
{
  static const int setting = []() {
    const char *value = std::getenv("UPBGE_LN_CAPTURE_PARALLEL");
    if (value == nullptr || value[0] == '\0') {
      return 1;
    }
    if (std::strcmp(value, "0") == 0) {
      return 0;
    }
    return std::strcmp(value, "force") == 0 ? 2 : 1;
  }();
  return setting;
}
}  // namespace

size_t LN_ParallelCaptureMinTrees()
{
  switch (parallel_capture_setting()) {
    case 0:
      return SIZE_MAX;
    case 2:
      return 2;
    default:
      return LN_PARALLEL_CAPTURE_MIN_TREES;
  }
}

size_t LN_ParallelCaptureMinChunk()
{
  return parallel_capture_setting() == 2 ? 1 : LN_PARALLEL_CAPTURE_MIN_CHUNK;
}

LN_TreeBatchMode LN_GetTreeBatchMode()
{
  if (const std::optional<LN_TreeBatchMode> &mode = tree_batch_mode_for_tests()) {
    return *mode;
  }
  static const LN_TreeBatchMode mode = tree_batch_mode_from_environment();
  return mode;
}

bool LN_TreeBatchWide()
{
  return batch_wide();
}

void LN_SetBatchWideForTests(const int wide)
{
  if (wide < 0) {
    batch_wide_for_tests().reset();
  }
  else {
    batch_wide_for_tests() = wide;
  }
}

void LN_SetTreeBatchModeForTests(const LN_TreeBatchMode mode)
{
  tree_batch_mode_for_tests() = mode;
}

void LN_ClearTreeBatchModeForTests()
{
  tree_batch_mode_for_tests().reset();
}

LN_TreeBatchRecorder::LN_TreeBatchRecorder() = default;
LN_TreeBatchRecorder::~LN_TreeBatchRecorder() = default;

size_t LN_TreeBatchRecorder::Record(LN_JobPool &pool,
                                    LN_CommandBuffer &buffer,
                                    LN_RuntimeTree *const *trees,
                                    const size_t count,
                                    const LN_TickContext &context,
                                    LN_RuntimeProfileCounters *profile_counters,
                                    LN_Manager *manager,
                                    size_t chunk_count)
{
  m_treesThatReadAMove.clear();
  if (count == 0) {
    return 0;
  }
  chunk_count = std::clamp<size_t>(chunk_count, 1, count);
  while (m_chunks.size() < chunk_count) {
    m_chunks.push_back(std::make_unique<LN_CommandBuffer>());
  }
  /* Lean programs are made on the main thread only (plan phase A17): before the chunks go to the
   * helpers, every tree's program has its lean form (none in the reference mode). */
  if (LN_GetLeanMode() == LN_LeanMode::Lean) {
    for (size_t index = 0; index < count; index++) {
      trees[index]->PrepareLeanProgram();
    }
  }
  /* The chunks' reads look for pending moves in the tick's buffer too (the safety net). */
  buffer.PrepareForChunkReads();
  for (size_t chunk = 0; chunk < chunk_count; chunk++) {
    m_chunks[chunk]->SetLogicManager(manager);
    m_chunks[chunk]->SetChunkTickBuffer(&buffer);
  }

  /* Chunk k holds trees [count * k / chunks, count * (k + 1) / chunks): consecutive trees, so the
   * chunks appended in order are the trees' order. The pool hands chunks out one at a time. */
  const auto first_tree = [&](const size_t chunk) { return count * chunk / chunk_count; };
  pool.Run(chunk_count, [&](const size_t chunk, uint32_t /*thread*/) {
    LN_CommandBuffer &chunk_buffer = *m_chunks[chunk];
    chunk_buffer.BeginChunkRecording();
    for (size_t index = first_tree(chunk); index < first_tree(chunk + 1); index++) {
      LN_RuntimeProfileCounters *counters = profile_counters ? &profile_counters[index] : nullptr;
      trees[index]->ExecuteReady(chunk_buffer, context, counters);
    }
    chunk_buffer.EndChunkRecording();
  });
  LN_AddColumnPasses(count);

  const std::chrono::steady_clock::time_point join_start = std::chrono::steady_clock::now();
  size_t chunks_that_read_a_move = 0;
  for (size_t chunk = 0; chunk < chunk_count; chunk++) {
    LN_CommandBuffer &chunk_buffer = *m_chunks[chunk];
    if (chunk_buffer.ChunkReadPendingMove()) {
      chunks_that_read_a_move++;
      for (size_t index = first_tree(chunk); index < first_tree(chunk + 1); index++) {
        m_treesThatReadAMove.push_back(index);
      }
    }
    buffer.AppendRecordedChunk(chunk_buffer);
  }
  m_lastJoinMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           join_start)
                     .count();
  return chunks_that_read_a_move;
}

const char *LN_BatchExpressionRefusal(const LN_Program &program,
                                      const uint8_t domain,
                                      const uint32_t index)
{
  const auto in = [&](const auto &table) { return index < table.size(); };
  switch (LN_KindDomain(domain)) {
    case LN_KindDomain::Bool:
      return in(program.GetBoolExpressions()) ?
                 bool_kind_refusal(program.GetBoolExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Int:
      return in(program.GetIntExpressions()) ?
                 int_kind_refusal(program.GetIntExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Float:
      return in(program.GetFloatExpressions()) ?
                 float_kind_refusal(program.GetFloatExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::String:
      return in(program.GetStringExpressions()) ?
                 string_kind_refusal(program.GetStringExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Vector:
      return in(program.GetVectorExpressions()) ?
                 vector_kind_refusal(program.GetVectorExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Color:
      return in(program.GetColorExpressions()) ?
                 color_kind_refusal(program.GetColorExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Value:
      return in(program.GetValueExpressions()) ?
                 value_kind_refusal(program, program.GetValueExpressions()[index]) :
                 "no such expression";
    case LN_KindDomain::Query:
      return in(program.GetQueryExpressions()) ?
                 query_refusal(program, program.GetQueryExpressions()[index]) :
                 "no such expression";
    default:
      return "not an expression table";
  }
}

const char *LN_BatchOpRefusal(const LN_Program &program, const LN_Instruction &instruction)
{
  return op_refusal(program, instruction);
}

const char *LN_BatchKindRefusalForTests(const uint8_t domain, const uint16_t value)
{
  switch (LN_KindDomain(domain)) {
    case LN_KindDomain::Op: {
      const LN_Program program;
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode(value);
      return op_refusal(program, instruction);
    }
    case LN_KindDomain::Bool: {
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind(value);
      return bool_kind_refusal(expression);
    }
    case LN_KindDomain::Int: {
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind(value);
      return int_kind_refusal(expression);
    }
    case LN_KindDomain::Float: {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind(value);
      return float_kind_refusal(expression);
    }
    case LN_KindDomain::String: {
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind(value);
      return string_kind_refusal(expression);
    }
    case LN_KindDomain::Vector: {
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind(value);
      return vector_kind_refusal(expression);
    }
    case LN_KindDomain::Color: {
      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind(value);
      return color_kind_refusal(expression);
    }
    case LN_KindDomain::Value: {
      const LN_Program program;
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind(value);
      return value_kind_refusal(program, expression);
    }
    case LN_KindDomain::Query: {
      const LN_Program program;
      LN_QueryExpression expression;
      expression.kind = LN_QueryExpressionKind(value);
      return query_refusal(program, expression);
    }
    case LN_KindDomain::Count:
      break;
  }
  return "not a kind";
}
