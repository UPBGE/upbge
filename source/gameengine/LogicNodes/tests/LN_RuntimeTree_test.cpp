/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DEV_InputDevice.h"
#include "KX_GameObject.h"
#include "LN_CommandBuffer.h"
#include "LN_Manager.h"
#include "LN_ParallelTreeExecutor.h"
#include "LN_Program.h"
#include "LN_QueryDiagnostics.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "RAS_ICanvas.h"
#include "RAS_Rect.h"
#include "SG_Node.h"

class LN_RuntimeTreeTestAccess {
 public:
  static void StoreCollisionPayload(LN_RuntimeTree &tree,
                                    KX_GameObject *owner,
                                    KX_GameObject *hit_object,
                                    const uint64_t tick_index)
  {
    StoreCollisionPayloads(tree, owner, {hit_object}, tick_index);
  }

  static void StoreCollisionPayloads(LN_RuntimeTree &tree,
                                     KX_GameObject *owner,
                                     const std::vector<KX_GameObject *> &hit_objects,
                                     const uint64_t tick_index)
  {
    LN_RuntimeTree::CollisionResult result;
    result.hit = true;
    result.owner = owner;
    result.hit_object = hit_objects.empty() ? nullptr : hit_objects.front();
    result.hit_objects = hit_objects;
    result.detail = LN_RuntimeTree::CollisionResult::Detail::Objects;
    tree.StoreCollisionEventPayload(result, tick_index);
  }

  static void MarkCollisionExit(LN_RuntimeTree &tree,
                                KX_GameObject *owner,
                                const uint64_t tick_index)
  {
    LN_RuntimeTree::CollisionResult current;
    current.owner = owner;
    tree.MarkCollisionExitPayload(current, tick_index);
  }

  static KX_GameObject *ExitHitObject(LN_RuntimeTree &tree,
                                      KX_GameObject *owner,
                                      const uint64_t tick_index)
  {
    const LN_RuntimeTree::CollisionResult *result = tree.FindCollisionExitPayload(
        owner, std::string(), std::string(), tick_index);
    return result != nullptr ? result->hit_object : nullptr;
  }

  static size_t ExitHitObjectCount(LN_RuntimeTree &tree,
                                   KX_GameObject *owner,
                                   const uint64_t tick_index)
  {
    const LN_RuntimeTree::CollisionResult *result = tree.FindCollisionExitPayload(
        owner, std::string(), std::string(), tick_index);
    return result != nullptr ? result->hit_objects.size() : 0;
  }

  static bool PersistentPayloadContainsRawObjectPointers(const LN_RuntimeTree &tree)
  {
    for (const LN_RuntimeTree::CollisionEventPayload &payload : tree.m_collisionEventPayloads) {
      if (payload.last_result.owner != nullptr || payload.last_result.hit_object != nullptr ||
          !payload.last_result.hit_objects.empty())
      {
        return true;
      }
    }
    return false;
  }
};

namespace {

KX_GameObject *FakeGameObjectPointer(uint32_t value)
{
  return reinterpret_cast<KX_GameObject *>(uintptr_t(value));
}

class TestRootParentRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child,
                              const SG_Node * /*parent*/,
                              bool &parentUpdated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    parentUpdated = false;
    return true;
  }

  SG_ParentRelation *NewCopy() override
  {
    return new TestRootParentRelation();
  }
};

class TestCanvas : public RAS_ICanvas {
 public:
  TestCanvas(const int width, const int height) : RAS_ICanvas(nullptr)
  {
    m_mousestate = MOUSE_NORMAL;
    m_frame = 0;
    Resize(width, height);
  }

  void Init() override {}
  void BeginFrame() override {}
  void EndFrame() override {}
  void BeginDraw() override {}
  void EndDraw() override {}
  void SwapBuffers() override {}
  void SetSwapInterval(const int interval) override
  {
    m_swapInterval = interval;
  }
  bool GetSwapInterval(int &intervalOut) override
  {
    intervalOut = m_swapInterval;
    return true;
  }
  void ConvertMousePosition(const int x, const int y, int &r_x, int &r_y, bool /*screen*/) override
  {
    r_x = x;
    r_y = y;
  }
  void SetMouseState(const RAS_MouseState mousestate) override
  {
    m_mousestate = mousestate;
  }
  void SetMousePosition(const int /*x*/, const int /*y*/) override {}
  void MakeScreenShot(const std::string & /*filename*/) override {}
  void GetDisplayDimensions(blender::int2 &scr_size) override
  {
    scr_size[0] = GetWidth();
    scr_size[1] = GetHeight();
  }
  void ResizeWindow(const int width, const int height) override
  {
    Resize(width, height);
  }
  void Resize(const int width, const int height) override
  {
    m_viewportArea = RAS_Rect(width, height);
    m_windowArea = RAS_Rect(width, height);
  }
  void SetFullScreen(const bool enable) override
  {
    m_fullscreen = enable;
  }
  bool GetFullScreen() override
  {
    return m_fullscreen;
  }

 private:
  int m_swapInterval = 0;
  bool m_fullscreen = false;
};

std::vector<LN_CommandBuffer::Command> RecordReadyCommands(LN_RuntimeTree &runtime_tree,
                                                           const LN_TickContext &context)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();
  return command_buffer.TakeRecordedCommands();
}

std::vector<LN_CommandBuffer::Command> RecordForcedCommands(LN_RuntimeTree &runtime_tree,
                                                            const LN_TickContext &context)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteForcedUpdate(command_buffer, context);
  command_buffer.EndRecording();
  return command_buffer.TakeRecordedCommands();
}

void CaptureMousePosition(DEV_InputDevice &input_device,
                          LN_InputSnapshot &input_snapshot,
                          const int x,
                          const int y,
                          RAS_ICanvas *canvas = nullptr)
{
  input_device.ClearInputs();
  input_device.ConvertMoveEvent(x, y);
  input_snapshot.Capture(&input_device, canvas);
}

void CaptureCurrentMousePositionWithoutFrameDelta(DEV_InputDevice &input_device,
                                                  LN_InputSnapshot &input_snapshot,
                                                  RAS_ICanvas *canvas = nullptr)
{
  input_device.ClearInputs();
  input_snapshot.Capture(&input_device, canvas);
}

uint32_t AddStringConstant(LN_Program &program, const std::string &value)
{
  LN_StringExpression expression;
  expression.kind = LN_StringExpressionKind::Constant;
  expression.string_value = value;
  return program.AddStringExpression(expression);
}

uint32_t AddIntConstant(LN_Program &program, const int32_t value)
{
  LN_IntExpression expression;
  expression.kind = LN_IntExpressionKind::Constant;
  expression.int_value = value;
  return program.AddIntExpression(expression);
}

uint32_t AddBoolConstant(LN_Program &program, const bool value)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Constant;
  expression.bool_value = value;
  return program.AddBoolExpression(expression);
}

uint32_t AddVectorConstant(LN_Program &program, const MT_Vector3 &value)
{
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::Constant;
  expression.vector_value = value;
  return program.AddVectorExpression(expression);
}

uint32_t AddIntValueConstant(LN_Program &program, const int32_t value)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  expression.value.type = LN_ValueType::Int;
  expression.value.exists = true;
  expression.value.int_value = value;
  return program.AddValueExpression(expression);
}

uint32_t AddNoneValueConstant(LN_Program &program)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  return program.AddValueExpression(expression);
}

uint32_t AddObjectRefConstant(LN_Program &program, const LN_RuntimeRef &runtime_ref)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  expression.value.type = LN_ValueType::ObjectRef;
  expression.value.exists = true;
  expression.value.runtime_ref = runtime_ref;
  return program.AddValueExpression(expression);
}

uint32_t AddDatablockRefConstant(LN_Program &program, const std::string &name)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  expression.value.type = LN_ValueType::DatablockRef;
  expression.value.exists = true;
  expression.value.reference_name = name;
  return program.AddValueExpression(expression);
}

uint32_t AddFloatValueConstant(LN_Program &program, const float value)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  expression.value.type = LN_ValueType::Float;
  expression.value.exists = true;
  expression.value.float_value = value;
  return program.AddValueExpression(expression);
}

uint32_t AddFloatConstant(LN_Program &program, const float value)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::Constant;
  expression.float_value = value;
  return program.AddFloatExpression(expression);
}

uint32_t AddLinearTweenCurveTable(LN_Program &program, const float end_value = 1.0f)
{
  std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT> samples{};
  for (int sample_index = 0; sample_index < LN_TWEEN_CURVE_SAMPLE_COUNT; sample_index++) {
    const float factor = float(sample_index) / float(LN_TWEEN_CURVE_SAMPLE_COUNT - 1);
    samples[size_t(sample_index)] = factor * end_value;
  }
  return program.AddTweenCurveTable(samples);
}

uint32_t AddOnceExpression(LN_Program &program)
{
  LN_BoolExpression once;
  once.kind = LN_BoolExpressionKind::Once;
  return program.AddBoolExpression(once);
}

uint32_t AddOncePulse(LN_Program &program,
                      const uint32_t flow_index = LN_INVALID_INDEX,
                      const uint32_t reset_index = LN_INVALID_INDEX,
                      const uint32_t loop_frame_index = LN_INVALID_INDEX)
{
  const uint32_t once_index = AddOnceExpression(program);
  if (reset_index != LN_INVALID_INDEX) {
    LN_Instruction reset;
    reset.opcode = LN_OpCode::ResetOnce;
    reset.bool_guard_expr_index = reset_index;
    reset.int_value = int32_t(once_index);
    reset.loop_frame_index = loop_frame_index;
    program.AddInstruction(LN_Event::OnFixedUpdate, reset);
  }

  LN_Instruction attempt;
  attempt.opcode = LN_OpCode::TryOnce;
  attempt.bool_guard_expr_index = flow_index == LN_INVALID_INDEX ? AddBoolConstant(program, true) :
                                                                   flow_index;
  attempt.int_value = int32_t(once_index);
  attempt.loop_frame_index = loop_frame_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, attempt);
  return once_index;
}

void AddGuardedPrint(LN_Program &program,
                     const uint32_t guard_index,
                     const int32_t value,
                     const uint32_t loop_frame_index = LN_INVALID_INDEX)
{
  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = guard_index;
  print.value_expr_index = AddIntValueConstant(program, value);
  print.loop_frame_index = loop_frame_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, print);
}

struct PropertyDrivenOnceProgram {
  std::shared_ptr<LN_Program> program;
  uint32_t flow_ref_index = LN_INVALID_INDEX;
  uint32_t reset_ref_index = LN_INVALID_INDEX;
};

uint32_t AddBoolTreePropertyExpression(LN_Program &program,
                                       const char *name,
                                       const bool default_value,
                                       uint32_t &r_property_ref_index)
{
  LN_Value value;
  value.type = LN_ValueType::Bool;
  value.exists = true;
  value.bool_value = default_value;

  LN_TreePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = LN_ValueType::Bool;
  property_ref.default_value = value;
  r_property_ref_index = program.AddTreePropertyRef(property_ref);

  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
  expression.property_ref_index = r_property_ref_index;
  return program.AddBoolExpression(expression);
}

PropertyDrivenOnceProgram CreatePropertyDrivenOnceProgram()
{
  PropertyDrivenOnceProgram refs;
  refs.program = std::make_shared<LN_Program>();
  const uint32_t flow_index = AddBoolTreePropertyExpression(
      *refs.program, "flow", false, refs.flow_ref_index);
  const uint32_t reset_index = AddBoolTreePropertyExpression(
      *refs.program, "reset", false, refs.reset_ref_index);
  const uint32_t once_index = AddOncePulse(*refs.program, flow_index, reset_index);
  AddGuardedPrint(*refs.program, once_index, 1);
  return refs;
}

bool SetBoolTreeProperty(LN_RuntimeTree &runtime_tree,
                         const uint32_t property_ref_index,
                         const bool value)
{
  LN_Value property_value;
  property_value.type = LN_ValueType::Bool;
  property_value.exists = true;
  property_value.bool_value = value;
  return runtime_tree.SetTreePropertyValue(property_ref_index, property_value);
}

uint32_t AddFloatTreePropertyExpression(LN_Program &program,
                                        const char *name,
                                        const float default_value,
                                        uint32_t &r_property_ref_index)
{
  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  value.float_value = default_value;

  LN_TreePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = LN_ValueType::Float;
  property_ref.default_value = value;
  r_property_ref_index = program.AddTreePropertyRef(property_ref);

  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  expression.property_ref_index = r_property_ref_index;
  return program.AddFloatExpression(expression);
}

bool SetFloatTreeProperty(LN_RuntimeTree &runtime_tree,
                          const uint32_t property_ref_index,
                          const float value)
{
  LN_Value property_value;
  property_value.type = LN_ValueType::Float;
  property_value.exists = true;
  property_value.float_value = value;
  return runtime_tree.SetTreePropertyValue(property_ref_index, property_value);
}

struct BooleanEdgeExpressionRefs {
  uint32_t rising = LN_INVALID_INDEX;
  uint32_t falling = LN_INVALID_INDEX;
};

BooleanEdgeExpressionRefs AddBooleanEdgeExpressions(LN_Program &program,
                                                    const uint32_t condition_index)
{
  BooleanEdgeExpressionRefs refs;
  LN_BoolExpression rising;
  rising.kind = LN_BoolExpressionKind::BooleanEdge;
  rising.input0 = condition_index;
  refs.rising = program.AddBoolExpression(rising);

  LN_BoolExpression falling;
  falling.kind = LN_BoolExpressionKind::BooleanEdgeFalling;
  falling.input0 = refs.rising;
  refs.falling = program.AddBoolExpression(falling);
  return refs;
}

struct CooldownExpressionRefs {
  uint32_t state = LN_INVALID_INDEX;
  uint32_t accepted = LN_INVALID_INDEX;
  uint32_t blocked = LN_INVALID_INDEX;
  uint32_t completed = LN_INVALID_INDEX;
  uint32_t ready = LN_INVALID_INDEX;
  uint32_t remaining = LN_INVALID_INDEX;
  uint32_t progress = LN_INVALID_INDEX;
};

CooldownExpressionRefs AddCooldownExpressions(LN_Program &program)
{
  CooldownExpressionRefs refs;
  refs.state = program.AddTimeFlowState();
  auto add_bool = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(refs.state);
    return program.AddBoolExpression(expression);
  };
  refs.accepted = add_bool(LN_BoolExpressionKind::CooldownAccepted);
  refs.blocked = add_bool(LN_BoolExpressionKind::CooldownBlocked);
  refs.completed = add_bool(LN_BoolExpressionKind::CooldownCompleted);
  refs.ready = add_bool(LN_BoolExpressionKind::CooldownReady);

  LN_FloatExpression remaining;
  remaining.kind = LN_FloatExpressionKind::CooldownRemaining;
  remaining.int_value = int32_t(refs.state);
  refs.remaining = program.AddFloatExpression(remaining);

  LN_FloatExpression progress;
  progress.kind = LN_FloatExpressionKind::CooldownProgress;
  progress.int_value = int32_t(refs.state);
  refs.progress = program.AddFloatExpression(progress);
  return refs;
}

void AddCooldownControl(LN_Program &program,
                        const CooldownExpressionRefs &refs,
                        const uint32_t flow_index,
                        const uint32_t reset_index,
                        const float duration,
                        const bool ignore_timescale = false,
                        const uint32_t loop_frame_index = LN_INVALID_INDEX)
{
  if (reset_index != LN_INVALID_INDEX) {
    LN_Instruction reset;
    reset.opcode = LN_OpCode::ResetCooldown;
    reset.bool_guard_expr_index = reset_index;
    reset.int_value = int32_t(refs.state);
    reset.loop_frame_index = loop_frame_index;
    program.AddInstruction(LN_Event::OnFixedUpdate, reset);
  }

  LN_Instruction attempt;
  attempt.opcode = LN_OpCode::TryCooldown;
  attempt.bool_guard_expr_index = flow_index;
  attempt.float_expr_index = AddFloatConstant(program, duration);
  attempt.secondary_bool_expr_index = AddBoolConstant(program, ignore_timescale);
  attempt.int_value = int32_t(refs.state);
  attempt.loop_frame_index = loop_frame_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, attempt);
}

void AddFloatPrint(LN_Program &program, const uint32_t float_expression_index)
{
  LN_ValueExpression value;
  value.kind = LN_ValueExpressionKind::FromFloat;
  value.input0 = float_expression_index;

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = program.AddValueExpression(value);
  program.AddInstruction(LN_Event::OnFixedUpdate, print);
}

uint32_t AddDelayedPulse(LN_Program &program, const float duration)
{
  LN_BoolExpression timer;
  timer.kind = LN_BoolExpressionKind::Timer;
  timer.input0 = AddBoolConstant(program, true);
  timer.float_expr_index = AddFloatConstant(program, duration);
  return program.AddBoolExpression(timer);
}

struct TweenExpressionRefs {
  uint32_t tween_index = LN_INVALID_INDEX;
  uint32_t reached_index = LN_INVALID_INDEX;
  uint32_t factor_index = LN_INVALID_INDEX;
  uint32_t float_result_index = LN_INVALID_INDEX;
  uint32_t vector_result_index = LN_INVALID_INDEX;
};

TweenExpressionRefs AddTweenExpressions(LN_Program &program,
                                        const uint32_t forward_index,
                                        const uint32_t back_index,
                                        const float duration,
                                        const bool on_demand = false,
                                        const bool instant_reset = false,
                                        const float curve_end_value = 1.0f)
{
  TweenExpressionRefs refs;

  LN_BoolExpression tween;
  tween.kind = LN_BoolExpressionKind::TweenValue;
  tween.input0 = forward_index;
  tween.input1 = back_index;
  tween.float_expr_index = AddFloatConstant(program, duration);
  tween.int_expr_index = AddLinearTweenCurveTable(program, curve_end_value);
  tween.bool_value = on_demand;
  tween.float_value = instant_reset ? 1.0f : 0.0f;
  refs.tween_index = program.AddBoolExpression(tween);

  LN_BoolExpression reached;
  reached.kind = LN_BoolExpressionKind::TweenReached;
  reached.input0 = refs.tween_index;
  refs.reached_index = program.AddBoolExpression(reached);

  LN_FloatExpression factor;
  factor.kind = LN_FloatExpressionKind::TweenFactor;
  factor.input0 = refs.tween_index;
  refs.factor_index = program.AddFloatExpression(factor);

  LN_FloatExpression float_result;
  float_result.kind = LN_FloatExpressionKind::TweenFloatResult;
  float_result.input0 = refs.tween_index;
  float_result.input1 = AddFloatConstant(program, 10.0f);
  float_result.input2 = AddFloatConstant(program, 20.0f);
  refs.float_result_index = program.AddFloatExpression(float_result);

  LN_VectorExpression vector_result;
  vector_result.kind = LN_VectorExpressionKind::TweenVectorResult;
  vector_result.input0 = refs.tween_index;
  vector_result.input1 = AddVectorConstant(program, MT_Vector3(0.0f, 0.0f, 0.0f));
  vector_result.input2 = AddVectorConstant(program, MT_Vector3(10.0f, 20.0f, 30.0f));
  refs.vector_result_index = program.AddVectorExpression(vector_result);

  return refs;
}

uint32_t AddTweenTestPropertyRef(LN_Program &program,
                                 const std::string &name,
                                 const LN_ValueType type)
{
  LN_GamePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = type;
  return program.AddGamePropertyRef(property_ref);
}

void AddSetFloatPropertyInstruction(LN_Program &program,
                                    const uint32_t property_ref_index,
                                    const uint32_t value_expr_index,
                                    const uint32_t guard_expr_index = LN_INVALID_INDEX)
{
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = property_ref_index;
  instruction.property_value_type = LN_ValueType::Float;
  instruction.float_expr_index = value_expr_index;
  instruction.bool_guard_expr_index = guard_expr_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
}

void AddSetVectorPropertyInstruction(LN_Program &program,
                                     const uint32_t property_ref_index,
                                     const uint32_t value_expr_index,
                                     const uint32_t guard_expr_index = LN_INVALID_INDEX)
{
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = property_ref_index;
  instruction.property_value_type = LN_ValueType::Vector;
  instruction.vector_expr_index = value_expr_index;
  instruction.bool_guard_expr_index = guard_expr_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
}

void AddSetBoolPropertyInstruction(LN_Program &program,
                                   const uint32_t property_ref_index,
                                   const bool value,
                                   const uint32_t guard_expr_index)
{
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = property_ref_index;
  instruction.property_value_type = LN_ValueType::Bool;
  instruction.bool_expr_index = AddBoolConstant(program, value);
  instruction.bool_guard_expr_index = guard_expr_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
}

const LN_CommandBuffer::Command *FindPropertyCommand(
    const std::vector<LN_CommandBuffer::Command> &commands, const uint32_t property_ref_index)
{
  for (const LN_CommandBuffer::Command &command : commands) {
    if (command.type == LN_CommandBuffer::CommandType::SetGameProperty &&
        command.property_ref_index == property_ref_index)
    {
      return &command;
    }
  }
  return nullptr;
}

std::shared_ptr<LN_Program> CreateMouseLookInputOnlyProgram(const bool center_mouse)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t empty_object_expr = AddNoneValueConstant(*program);
  const uint32_t invert_expr = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  const uint32_t cap_x_expr = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  const uint32_t cap_y_expr = AddVectorConstant(*program,
                                                MT_Vector3(-1.57079637f, 1.57079637f, 0.0f));
  const uint32_t no_cap_expr = AddBoolConstant(*program, false);
  const uint32_t sensitivity_expr = AddFloatConstant(*program, 1000.0f);
  const uint32_t smoothing_expr = AddFloatConstant(*program, 0.0f);

  LN_Instruction mouse_look;
  mouse_look.opcode = LN_OpCode::MouseLook;
  mouse_look.value_expr_index = empty_object_expr;
  mouse_look.property_ref_index = empty_object_expr;
  mouse_look.secondary_vector_expr_index = invert_expr;
  mouse_look.color_expr_index = cap_x_expr;
  mouse_look.int_expr_index = cap_y_expr;
  mouse_look.bool_expr_index = no_cap_expr;
  mouse_look.secondary_bool_expr_index = no_cap_expr;
  mouse_look.float_expr_index = sensitivity_expr;
  mouse_look.string_expr_index = smoothing_expr;
  mouse_look.int_value = 1;
  mouse_look.bool_value = center_mouse;
  mouse_look.vector_value = MT_Vector3(0.0f, 0.0f, 1000.0f);
  mouse_look.secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  mouse_look.color_value = MT_Vector4(-1.57079637f, 1.57079637f, 0.0f, 0.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, mouse_look);
  return program;
}

std::shared_ptr<LN_Program> CreateTimerQuitProgram(const float duration,
                                                   const LN_Event arm_event,
                                                   const bool ignore_timescale = false)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t timer_state_index = program->AddTimerState();
  const uint32_t duration_index = AddFloatConstant(*program, duration);
  const uint32_t ignore_timescale_index = ignore_timescale ? AddBoolConstant(*program, true) :
                                                             LN_INVALID_INDEX;

  LN_BoolExpression elapsed;
  elapsed.kind = LN_BoolExpressionKind::TimerElapsed;
  elapsed.int_value = int32_t(timer_state_index);
  const uint32_t elapsed_index = program->AddBoolExpression(elapsed);

  LN_Instruction arm_timer;
  arm_timer.opcode = LN_OpCode::ArmTimer;
  arm_timer.int_value = int32_t(timer_state_index);
  arm_timer.float_expr_index = duration_index;
  arm_timer.secondary_bool_expr_index = ignore_timescale_index;
  program->AddInstruction(arm_event, arm_timer);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = elapsed_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateImmediateTimeFlowQuitProgram(const LN_OpCode opcode)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t timer_state_index = program->AddTimerState();
  const uint32_t zero_duration_index = AddFloatConstant(*program, 0.0f);

  LN_BoolExpression elapsed;
  elapsed.kind = LN_BoolExpressionKind::TimerElapsed;
  elapsed.int_value = int32_t(timer_state_index);
  const uint32_t elapsed_index = program->AddBoolExpression(elapsed);

  LN_Instruction time_flow;
  time_flow.opcode = opcode;
  time_flow.int_value = int32_t(timer_state_index);
  time_flow.float_expr_index = zero_duration_index;
  if (opcode == LN_OpCode::UpdateBarrier) {
    time_flow.bool_expr_index = AddBoolConstant(*program, true);
  }
  program->AddInstruction(LN_Event::OnFixedUpdate, time_flow);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = elapsed_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateTwoTimerSameTickProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t first_timer_state_index = program->AddTimerState();
  const uint32_t second_timer_state_index = program->AddTimerState();
  const uint32_t duration_index = AddFloatConstant(*program, 0.2f);

  auto add_elapsed_expression = [&](const uint32_t timer_state_index) {
    LN_BoolExpression elapsed;
    elapsed.kind = LN_BoolExpressionKind::TimerElapsed;
    elapsed.int_value = int32_t(timer_state_index);
    return program->AddBoolExpression(elapsed);
  };
  const uint32_t first_elapsed_index = add_elapsed_expression(first_timer_state_index);
  const uint32_t second_elapsed_index = add_elapsed_expression(second_timer_state_index);

  auto add_arm_instruction = [&](const uint32_t timer_state_index) {
    LN_Instruction arm_timer;
    arm_timer.opcode = LN_OpCode::ArmTimer;
    arm_timer.int_value = int32_t(timer_state_index);
    arm_timer.float_expr_index = duration_index;
    program->AddInstruction(LN_Event::OnInit, arm_timer);
  };
  add_arm_instruction(first_timer_state_index);
  add_arm_instruction(second_timer_state_index);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = first_elapsed_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);

  LN_Instruction restart;
  restart.opcode = LN_OpCode::RestartGame;
  restart.bool_guard_expr_index = second_elapsed_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, restart);
  return program;
}

std::shared_ptr<LN_Program> CreateRegisterMathPositionProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression one;
  one.kind = LN_FloatExpressionKind::Constant;
  one.float_value = 1.0f;
  const uint32_t one_index = program->AddFloatExpression(one);

  LN_FloatExpression two;
  two.kind = LN_FloatExpressionKind::Constant;
  two.float_value = 2.0f;
  const uint32_t two_index = program->AddFloatExpression(two);

  LN_FloatExpression three;
  three.kind = LN_FloatExpressionKind::Add;
  three.input0 = one_index;
  three.input1 = two_index;
  const uint32_t three_index = program->AddFloatExpression(three);

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = one_index;
  vector.input1 = two_index;
  vector.input2 = three_index;
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_FloatExpression scale;
  scale.kind = LN_FloatExpressionKind::Constant;
  scale.float_value = 2.0f;
  const uint32_t scale_index = program->AddFloatExpression(scale);

  LN_VectorExpression scaled_vector;
  scaled_vector.kind = LN_VectorExpressionKind::Scale;
  scaled_vector.input0 = vector_index;
  scaled_vector.float_expr_index = scale_index;
  const uint32_t scaled_vector_index = program->AddVectorExpression(scaled_vector);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = scaled_vector_index;
  program->AddInstruction(LN_Event::OnInit, instruction);
  return program;
}

std::shared_ptr<LN_Program> CreateDelayQuitProgram(const float duration,
                                                   const LN_Event arm_event,
                                                   const bool ignore_timescale = false)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();
  const uint32_t duration_index = AddFloatConstant(*program, duration);
  const uint32_t ignore_timescale_index = ignore_timescale ? AddBoolConstant(*program, true) :
                                                             LN_INVALID_INDEX;

  LN_BoolExpression done;
  done.kind = LN_BoolExpressionKind::DelayDone;
  done.int_value = int32_t(state_index);
  const uint32_t done_index = program->AddBoolExpression(done);

  LN_Instruction arm_delay;
  arm_delay.opcode = LN_OpCode::ArmDelay;
  arm_delay.int_value = int32_t(state_index);
  arm_delay.float_expr_index = duration_index;
  arm_delay.secondary_bool_expr_index = ignore_timescale_index;
  program->AddInstruction(arm_event, arm_delay);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = done_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreatePulsifyQuitProgram(const float interval,
                                                     const LN_Event update_event,
                                                     const bool ignore_timescale = false)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();
  const uint32_t interval_index = AddFloatConstant(*program, interval);
  const uint32_t ignore_timescale_index = ignore_timescale ? AddBoolConstant(*program, true) :
                                                             LN_INVALID_INDEX;

  LN_BoolExpression pulse;
  pulse.kind = LN_BoolExpressionKind::PulsifyPulse;
  pulse.int_value = int32_t(state_index);
  const uint32_t pulse_index = program->AddBoolExpression(pulse);

  LN_Instruction update_pulsify;
  update_pulsify.opcode = LN_OpCode::UpdatePulsify;
  update_pulsify.int_value = int32_t(state_index);
  update_pulsify.float_expr_index = interval_index;
  update_pulsify.secondary_bool_expr_index = ignore_timescale_index;
  program->AddInstruction(update_event, update_pulsify);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = pulse_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateBarrierQuitProgram(const float duration,
                                                     const bool condition,
                                                     const bool ignore_timescale = false)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();
  const uint32_t duration_index = AddFloatConstant(*program, duration);
  const uint32_t condition_index = AddBoolConstant(*program, condition);
  const uint32_t ignore_timescale_index = ignore_timescale ? AddBoolConstant(*program, true) :
                                                             LN_INVALID_INDEX;

  LN_BoolExpression passed;
  passed.kind = LN_BoolExpressionKind::BarrierPassed;
  passed.int_value = int32_t(state_index);
  const uint32_t passed_index = program->AddBoolExpression(passed);

  LN_Instruction update_barrier;
  update_barrier.opcode = LN_OpCode::UpdateBarrier;
  update_barrier.int_value = int32_t(state_index);
  update_barrier.float_expr_index = duration_index;
  update_barrier.bool_expr_index = condition_index;
  update_barrier.secondary_bool_expr_index = ignore_timescale_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, update_barrier);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = passed_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateBranchRouteQuitProgram(const bool condition,
                                                         const bool route_value)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t condition_index = AddBoolConstant(*program, condition);

  LN_Instruction route;
  route.opcode = LN_OpCode::BranchRoute;
  route.bool_expr_index = condition_index;
  route.bool_value = route_value;
  const uint32_t route_index = program->AddInstruction(LN_Event::OnFixedUpdate, route);

  LN_BoolExpression route_pulse;
  route_pulse.kind = LN_BoolExpressionKind::InstructionExecuted;
  route_pulse.input0 = route_index;
  const uint32_t route_pulse_index = program->AddBoolExpression(route_pulse);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = route_pulse_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateLoopScopedBranchRouteQuitProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 2);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_IntExpression loop_index;
  loop_index.kind = LN_IntExpressionKind::LoopIndex;
  loop_index.int_value = int32_t(frame_index);
  frame.loop_index_int_expr_index = program->AddIntExpression(loop_index);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  LN_BoolExpression first_iteration;
  first_iteration.kind = LN_BoolExpressionKind::ValueCompare;
  first_iteration.input0 = frame.loop_current_value_expr_index;
  first_iteration.input1 = AddIntValueConstant(*program, 0);
  first_iteration.float_compare_operation = LN_FloatCompareOperation::Equal;
  const uint32_t first_iteration_index = program->AddBoolExpression(first_iteration);

  LN_Instruction route;
  route.opcode = LN_OpCode::BranchRoute;
  route.bool_expr_index = first_iteration_index;
  route.bool_value = true;
  route.loop_frame_index = frame_index;
  const uint32_t route_index = program->AddInstruction(LN_Event::OnFixedUpdate, route);

  LN_BoolExpression route_pulse;
  route_pulse.kind = LN_BoolExpressionKind::InstructionExecuted;
  route_pulse.input0 = route_index;
  const uint32_t route_pulse_index = program->AddBoolExpression(route_pulse);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = route_pulse_index;
  quit.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateDeterministicWorkerCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Instruction set_position;
  set_position.opcode = LN_OpCode::SetTransformVector;
  set_position.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  set_position.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  set_position.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_position);

  LN_Instruction apply_impulse;
  apply_impulse.opcode = LN_OpCode::ApplyImpulse;
  apply_impulse.vector_value = MT_Vector3(0.0f, 0.0f, 8.0f);
  apply_impulse.secondary_vector_value = MT_Vector3(0.25f, 0.5f, 0.75f);
  program->AddInstruction(LN_Event::OnFixedUpdate, apply_impulse);

  LN_Instruction apply_force;
  apply_force.opcode = LN_OpCode::ApplyPhysicsVector;
  apply_force.vector_operation_mode = uint8_t(LN_VectorOperationMode::Local);
  apply_force.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Force);
  apply_force.vector_value = MT_Vector3(4.0f, 0.0f, 0.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, apply_force);

  LN_Instruction set_light_color;
  set_light_color.opcode = LN_OpCode::SetLightColor;
  set_light_color.color_value = MT_Vector4(0.2f, 0.4f, 0.6f, 1.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_light_color);

  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;
  const uint32_t property_index = program->AddGamePropertyRef(property_ref);
  LN_Instruction set_property;
  set_property.opcode = LN_OpCode::SetGameProperty;
  set_property.property_ref_index = property_index;
  set_property.property_value_type = LN_ValueType::Int;
  set_property.int_expr_index = AddIntConstant(*program, 42);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_property);

  LN_Instruction set_window_size;
  set_window_size.opcode = LN_OpCode::SetWindowSize;
  set_window_size.int_expr_index = AddIntConstant(*program, 1280);
  set_window_size.secondary_int_expr_index = AddIntConstant(*program, 720);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_window_size);

  LN_Instruction play_sound;
  play_sound.opcode = LN_OpCode::PlaySound;
  play_sound.string_expr_index = AddStringConstant(*program, "tone");
  play_sound.vector_value = MT_Vector3(0.75f, 1.25f, 0.0f);
  play_sound.bool_value = true;
  program->AddInstruction(LN_Event::OnFixedUpdate, play_sound);

  LN_Instruction send_event;
  send_event.opcode = LN_OpCode::SendEvent;
  send_event.string_expr_index = AddStringConstant(*program, "determinism");
  program->AddInstruction(LN_Event::OnFixedUpdate, send_event);

  LN_Instruction set_material_slot;
  set_material_slot.opcode = LN_OpCode::SetMaterialSlot;
  set_material_slot.secondary_value_expr_index = AddDatablockRefConstant(*program, "Material_A");
  set_material_slot.int_expr_index = AddIntConstant(*program, 1);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_material_slot);

  return program;
}

std::shared_ptr<LN_Program> CreateDeterministicMainThreadCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::Raycast;
  program->AddQueryExpression(query);

  LN_Instruction remove_parent;
  remove_parent.opcode = LN_OpCode::RemoveParent;
  program->AddInstruction(LN_Event::OnFixedUpdate, remove_parent);

  LN_Instruction remove_object;
  remove_object.opcode = LN_OpCode::RemoveObject;
  program->AddInstruction(LN_Event::OnFixedUpdate, remove_object);

  return program;
}

std::shared_ptr<LN_Program> CreateProfileCounterProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Instruction set_window_size;
  set_window_size.opcode = LN_OpCode::SetWindowSize;
  set_window_size.int_expr_index = AddIntConstant(*program, 1280);
  set_window_size.secondary_int_expr_index = AddIntConstant(*program, 720);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_window_size);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterIntSnapshotCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression collision_group;
  collision_group.kind = LN_IntExpressionKind::SnapshotCollisionGroup;
  const uint32_t collision_group_index = program->AddIntExpression(collision_group);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetCollisionGroup;
  instruction.int_expr_index = collision_group_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterCharacterIntSnapshotFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression max_jumps;
  max_jumps.kind = LN_IntExpressionKind::SnapshotCharacterMaxJumps;
  max_jumps.int_value = 3;
  const uint32_t max_jumps_index = program->AddIntExpression(max_jumps);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetCollisionGroup;
  instruction.int_expr_index = max_jumps_index;
  instruction.int_value = 3;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterIntInputCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression mouse_wheel;
  mouse_wheel.kind = LN_IntExpressionKind::MouseWheelDelta;
  const uint32_t mouse_wheel_index = program->AddIntExpression(mouse_wheel);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetCollisionGroup;
  instruction.int_expr_index = mouse_wheel_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterWindowResolutionFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression window_width;
  window_width.kind = LN_IntExpressionKind::WindowResolutionWidth;
  const uint32_t window_width_index = program->AddIntExpression(window_width);

  LN_IntExpression window_height;
  window_height.kind = LN_IntExpressionKind::WindowResolutionHeight;
  const uint32_t window_height_index = program->AddIntExpression(window_height);

  LN_VectorExpression window_resolution;
  window_resolution.kind = LN_VectorExpressionKind::WindowResolution;
  const uint32_t window_resolution_index = program->AddVectorExpression(window_resolution);

  LN_IntExpression window_vsync;
  window_vsync.kind = LN_IntExpressionKind::WindowVSyncMode;
  const uint32_t window_vsync_index = program->AddIntExpression(window_vsync);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = window_width_index;
  set_collision_group.int_value = 11;
  program->AddInstruction(LN_Event::OnInit, set_collision_group);

  LN_Instruction set_collision_group_from_height;
  set_collision_group_from_height.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_height.int_expr_index = window_height_index;
  set_collision_group_from_height.int_value = 22;
  program->AddInstruction(LN_Event::OnInit, set_collision_group_from_height);

  LN_Instruction set_world_position;
  set_world_position.opcode = LN_OpCode::SetWorldPosition;
  set_world_position.vector_expr_index = window_resolution_index;
  set_world_position.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  program->AddInstruction(LN_Event::OnInit, set_world_position);

  LN_Instruction set_vsync;
  set_vsync.opcode = LN_OpCode::SetVSync;
  set_vsync.int_expr_index = window_vsync_index;
  set_vsync.int_value = VSYNC_ADAPTIVE;
  program->AddInstruction(LN_Event::OnInit, set_vsync);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterVectorSnapshotCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression world_position;
  world_position.kind = LN_VectorExpressionKind::SnapshotWorldPosition;
  const uint32_t world_position_index = program->AddVectorExpression(world_position);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = world_position_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterCharacterVectorSnapshotFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression character_gravity;
  character_gravity.kind = LN_VectorExpressionKind::SnapshotCharacterGravity;
  character_gravity.vector_value = MT_Vector3(0.0f, 0.0f, -7.0f);
  const uint32_t character_gravity_index = program->AddVectorExpression(character_gravity);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = character_gravity_index;
  instruction.vector_value = MT_Vector3(0.0f, 0.0f, -7.0f);
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterVectorInputCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression cursor_position;
  cursor_position.kind = LN_VectorExpressionKind::CursorPosition;
  const uint32_t cursor_position_index = program->AddVectorExpression(cursor_position);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = cursor_position_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterGamepadVectorInputFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t gamepad_index = AddIntConstant(*program, 0);
  const uint32_t threshold = AddFloatConstant(*program, 0.1f);
  const uint32_t sensitivity = AddFloatConstant(*program, 1.0f);

  LN_VectorExpression gamepad_stick;
  gamepad_stick.kind = LN_VectorExpressionKind::GamepadStick;
  gamepad_stick.input0 = gamepad_index;
  gamepad_stick.input1 = threshold;
  gamepad_stick.float_expr_index = sensitivity;
  gamepad_stick.vector_value = MT_Vector3(9.0f, 8.0f, 7.0f);
  const uint32_t gamepad_stick_index = program->AddVectorExpression(gamepad_stick);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = gamepad_stick_index;
  instruction.vector_value = MT_Vector3(9.0f, 8.0f, 7.0f);
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterVectorResizeCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Constant;
  vector.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_VectorExpression resize;
  resize.kind = LN_VectorExpressionKind::Resize;
  resize.input0 = vector_index;
  resize.float_value = 2.0f;
  const uint32_t resize_index = program->AddVectorExpression(resize);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = resize_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterVectorRotateAroundAxisCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression origin;
  origin.kind = LN_VectorExpressionKind::Constant;
  origin.vector_value = MT_Vector3(1.0f, 0.0f, 0.0f);
  const uint32_t origin_index = program->AddVectorExpression(origin);

  LN_VectorExpression pivot;
  pivot.kind = LN_VectorExpressionKind::Constant;
  pivot.vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  const uint32_t pivot_index = program->AddVectorExpression(pivot);

  LN_FloatExpression angle;
  angle.kind = LN_FloatExpressionKind::Constant;
  angle.float_value = 1.57079632679f;
  const uint32_t angle_index = program->AddFloatExpression(angle);

  LN_VectorExpression rotate;
  rotate.kind = LN_VectorExpressionKind::RotateAroundAxis;
  rotate.input0 = origin_index;
  rotate.input1 = pivot_index;
  rotate.float_expr_index = angle_index;
  rotate.property_ref_index = 6;
  const uint32_t rotate_index = program->AddVectorExpression(rotate);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetWorldPosition;
  instruction.vector_expr_index = rotate_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterRandomExpressionCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression random_int;
  random_int.kind = LN_IntExpressionKind::Random;
  random_int.input0 = AddIntConstant(*program, 2);
  random_int.input1 = AddIntConstant(*program, 7);
  const uint32_t random_int_index = program->AddIntExpression(random_int);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = random_int_index;
  program->AddInstruction(LN_Event::OnInit, set_collision_group);

  LN_FloatExpression random_float;
  random_float.kind = LN_FloatExpressionKind::Random;
  random_float.input0 = AddFloatConstant(*program, 0.25f);
  random_float.input1 = AddFloatConstant(*program, 0.75f);
  const uint32_t random_float_index = program->AddFloatExpression(random_float);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = random_float_index;
  program->AddInstruction(LN_Event::OnInit, set_time_scale);

  const uint32_t axes_index = AddVectorConstant(*program, MT_Vector3(1.0f, 0.0f, 1.0f));
  LN_VectorExpression random_vector;
  random_vector.kind = LN_VectorExpressionKind::Random;
  random_vector.input0 = axes_index;
  const uint32_t random_vector_index = program->AddVectorExpression(random_vector);

  LN_Instruction set_world_position;
  set_world_position.opcode = LN_OpCode::SetWorldPosition;
  set_world_position.vector_expr_index = random_vector_index;
  program->AddInstruction(LN_Event::OnInit, set_world_position);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterNestedRandomStringCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_IntExpression random_width;
  random_width.kind = LN_IntExpressionKind::Random;
  random_width.input0 = AddIntConstant(*program, 2);
  random_width.input1 = AddIntConstant(*program, 9);
  const uint32_t random_width_index = program->AddIntExpression(random_width);

  LN_StringExpression zero_fill;
  zero_fill.kind = LN_StringExpressionKind::ZeroFill;
  zero_fill.input0 = AddStringConstant(*program, "1");
  zero_fill.int_expr_index = random_width_index;
  const uint32_t zero_fill_index = program->AddStringExpression(zero_fill);

  LN_ValueExpression value;
  value.kind = LN_ValueExpressionKind::FromString;
  value.input0 = zero_fill_index;
  const uint32_t value_index = program->AddValueExpression(value);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterRandomUpdateTimeScaleProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression random_float;
  random_float.kind = LN_FloatExpressionKind::Random;
  random_float.input0 = AddFloatConstant(*program, 0.25f);
  random_float.input1 = AddFloatConstant(*program, 0.75f);
  const uint32_t random_float_index = program->AddFloatExpression(random_float);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = random_float_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_time_scale);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterRandomPartialCombineMovementProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression random_float;
  random_float.kind = LN_FloatExpressionKind::Random;
  random_float.input0 = AddFloatConstant(*program, -0.1f);
  random_float.input1 = AddFloatConstant(*program, 0.1f);
  const uint32_t random_float_index = program->AddFloatExpression(random_float);

  LN_VectorExpression movement;
  movement.kind = LN_VectorExpressionKind::Combine;
  movement.vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  movement.input2 = random_float_index;
  const uint32_t movement_index = program->AddVectorExpression(movement);

  LN_Instruction apply_movement;
  apply_movement.opcode = LN_OpCode::ApplyMovement;
  apply_movement.vector_expr_index = movement_index;
  apply_movement.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, apply_movement);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterRuntimeTreePropertyCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  auto add_property_ref = [&](const char *name, const LN_ValueType type, const LN_Value &value) {
    LN_TreePropertyRef property_ref;
    property_ref.name = name;
    property_ref.value_type = type;
    property_ref.default_value = value;
    return program->AddTreePropertyRef(property_ref);
  };

  LN_Value bool_value;
  bool_value.type = LN_ValueType::Bool;
  bool_value.exists = true;
  bool_value.bool_value = true;
  const uint32_t bool_ref = add_property_ref("visible", LN_ValueType::Bool, bool_value);

  LN_Value int_value;
  int_value.type = LN_ValueType::Int;
  int_value.exists = true;
  int_value.int_value = 5;
  const uint32_t int_ref = add_property_ref("group", LN_ValueType::Int, int_value);

  LN_Value float_value;
  float_value.type = LN_ValueType::Float;
  float_value.exists = true;
  float_value.float_value = 0.5f;
  const uint32_t float_ref = add_property_ref("time_scale", LN_ValueType::Float, float_value);

  LN_Value vector_value;
  vector_value.type = LN_ValueType::Vector;
  vector_value.exists = true;
  vector_value.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_ref = add_property_ref("position", LN_ValueType::Vector, vector_value);

  LN_Value color_value;
  color_value.type = LN_ValueType::Color;
  color_value.exists = true;
  color_value.color_value = MT_Vector4(0.2f, 0.3f, 0.4f, 1.0f);
  const uint32_t color_ref = add_property_ref("color", LN_ValueType::Color, color_value);

  LN_Value string_value;
  string_value.type = LN_ValueType::String;
  string_value.exists = true;
  string_value.string_value = "runtime";
  const uint32_t string_ref = add_property_ref("label", LN_ValueType::String, string_value);

  LN_BoolExpression bool_property;
  bool_property.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
  bool_property.property_ref_index = bool_ref;
  const uint32_t bool_index = program->AddBoolExpression(bool_property);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = bool_index;
  program->AddInstruction(LN_Event::OnInit, set_visibility);

  LN_IntExpression int_property;
  int_property.kind = LN_IntExpressionKind::RuntimeTreeProperty;
  int_property.property_ref_index = int_ref;
  const uint32_t int_index = program->AddIntExpression(int_property);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = int_index;
  program->AddInstruction(LN_Event::OnInit, set_collision_group);

  LN_FloatExpression float_property;
  float_property.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  float_property.property_ref_index = float_ref;
  const uint32_t float_index = program->AddFloatExpression(float_property);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = float_index;
  program->AddInstruction(LN_Event::OnInit, set_time_scale);

  LN_VectorExpression vector_property;
  vector_property.kind = LN_VectorExpressionKind::RuntimeTreeProperty;
  vector_property.property_ref_index = vector_ref;
  const uint32_t vector_index = program->AddVectorExpression(vector_property);

  LN_Instruction set_world_position;
  set_world_position.opcode = LN_OpCode::SetWorldPosition;
  set_world_position.vector_expr_index = vector_index;
  program->AddInstruction(LN_Event::OnInit, set_world_position);

  LN_ColorExpression color_property;
  color_property.kind = LN_ColorExpressionKind::RuntimeTreeProperty;
  color_property.property_ref_index = color_ref;
  const uint32_t color_index = program->AddColorExpression(color_property);

  LN_Instruction set_object_color;
  set_object_color.opcode = LN_OpCode::SetObjectColor;
  set_object_color.color_expr_index = color_index;
  program->AddInstruction(LN_Event::OnInit, set_object_color);

  LN_StringExpression string_property;
  string_property.kind = LN_StringExpressionKind::RuntimeTreeProperty;
  string_property.property_ref_index = string_ref;
  const uint32_t string_index = program->AddStringExpression(string_property);

  LN_ValueExpression string_value_expression;
  string_value_expression.kind = LN_ValueExpressionKind::FromString;
  string_value_expression.input0 = string_index;
  const uint32_t string_value_index = program->AddValueExpression(string_value_expression);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = string_value_index;
  program->AddInstruction(LN_Event::OnInit, print);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterFloatSnapshotLightCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression light_power;
  light_power.kind = LN_FloatExpressionKind::SnapshotLightPower;
  const uint32_t light_power_index = program->AddFloatExpression(light_power);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTimeScale;
  instruction.float_expr_index = light_power_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterGamepadFloatInputFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t gamepad_index = AddIntConstant(*program, 0);

  LN_FloatExpression button_strength;
  button_strength.kind = LN_FloatExpressionKind::GamepadButtonStrength;
  button_strength.input0 = gamepad_index;
  button_strength.input2 = 0;
  button_strength.float_value = 0.75f;
  const uint32_t button_strength_index = program->AddFloatExpression(button_strength);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTimeScale;
  instruction.float_expr_index = button_strength_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterBoolSnapshotVisibilityCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression visibility;
  visibility.kind = LN_BoolExpressionKind::SnapshotVisibility;
  const uint32_t visibility_index = program->AddBoolExpression(visibility);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = visibility_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterCharacterBoolSnapshotFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression on_ground;
  on_ground.kind = LN_BoolExpressionKind::SnapshotCharacterOnGround;
  on_ground.bool_value = true;
  const uint32_t on_ground_index = program->AddBoolExpression(on_ground);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = on_ground_index;
  instruction.bool_value = true;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterSnapshotGamePropertyExistsCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;
  const uint32_t property_ref_index = program->AddGamePropertyRef(property_ref);

  LN_BoolExpression exists;
  exists.kind = LN_BoolExpressionKind::SnapshotGamePropertyExists;
  exists.property_ref_index = property_ref_index;
  const uint32_t exists_index = program->AddBoolExpression(exists);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = exists_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterSnapshotGamePropertyValueFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_GamePropertyRef bool_property_ref;
  bool_property_ref.name = "missing_bool";
  bool_property_ref.value_type = LN_ValueType::Bool;
  const uint32_t bool_property_ref_index = program->AddGamePropertyRef(bool_property_ref);

  LN_BoolExpression bool_property;
  bool_property.kind = LN_BoolExpressionKind::SnapshotGameProperty;
  bool_property.property_ref_index = bool_property_ref_index;
  bool_property.bool_value = true;
  const uint32_t bool_property_index = program->AddBoolExpression(bool_property);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = bool_property_index;
  set_visibility.bool_value = true;
  program->AddInstruction(LN_Event::OnInit, set_visibility);

  LN_GamePropertyRef int_property_ref;
  int_property_ref.name = "missing_int";
  int_property_ref.value_type = LN_ValueType::Int;
  const uint32_t int_property_ref_index = program->AddGamePropertyRef(int_property_ref);

  LN_IntExpression int_property;
  int_property.kind = LN_IntExpressionKind::SnapshotGameProperty;
  int_property.property_ref_index = int_property_ref_index;
  int_property.int_value = 7;
  const uint32_t int_property_index = program->AddIntExpression(int_property);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = int_property_index;
  set_collision_group.int_value = 7;
  program->AddInstruction(LN_Event::OnInit, set_collision_group);

  LN_GamePropertyRef float_property_ref;
  float_property_ref.name = "missing_float";
  float_property_ref.value_type = LN_ValueType::Float;
  const uint32_t float_property_ref_index = program->AddGamePropertyRef(float_property_ref);

  LN_FloatExpression float_property;
  float_property.kind = LN_FloatExpressionKind::SnapshotGameProperty;
  float_property.property_ref_index = float_property_ref_index;
  float_property.float_value = 0.75f;
  const uint32_t float_property_index = program->AddFloatExpression(float_property);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = float_property_index;
  program->AddInstruction(LN_Event::OnInit, set_time_scale);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterBoolInputCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression input_status;
  input_status.kind = LN_BoolExpressionKind::InputStatus;
  input_status.int_value = 1;
  input_status.secondary_int_value = 1;
  const uint32_t input_status_index = program->AddBoolExpression(input_status);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = input_status_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterGamepadBoolInputCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t gamepad_index = AddIntConstant(*program, 0);

  LN_BoolExpression gamepad_active;
  gamepad_active.kind = LN_BoolExpressionKind::GamepadActive;
  gamepad_active.input0 = gamepad_index;
  const uint32_t gamepad_active_index = program->AddBoolExpression(gamepad_active);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = gamepad_active_index;
  instruction.bool_value = true;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterMouseMovedInputCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression mouse_moved;
  mouse_moved.kind = LN_BoolExpressionKind::MouseMoved;
  mouse_moved.bool_value = false;
  const uint32_t mouse_moved_index = program->AddBoolExpression(mouse_moved);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = mouse_moved_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateExecBlockIRCommandCoverageProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Instruction set_object_color;
  set_object_color.opcode = LN_OpCode::SetObjectColor;
  set_object_color.color_value = MT_Vector4(0.15f, 0.25f, 0.35f, 1.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_object_color);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = AddBoolConstant(*program, false);
  set_visibility.secondary_bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility);

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
    instruction.vector_value = MT_Vector3(0.25f, 0.5f, 0.75f);
    instruction.secondary_vector_value = MT_Vector3(0.1f, 0.2f, 0.3f);
    instruction.bool_expr_index = AddBoolConstant(*program, true);
    instruction.secondary_bool_expr_index = AddBoolConstant(*program, false);
    instruction.float_expr_index = AddFloatConstant(*program, 1.5f);
    instruction.int_expr_index = AddIntConstant(*program, 2);
    instruction.secondary_int_expr_index = AddIntConstant(*program, 1);
    program->AddInstruction(LN_Event::OnFixedUpdate, instruction);
  }

  LN_Instruction set_camera_fov;
  set_camera_fov.opcode = LN_OpCode::SetCameraFov;
  set_camera_fov.float_expr_index = AddFloatConstant(*program, 65.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_camera_fov);

  LN_Instruction set_camera_ortho_scale;
  set_camera_ortho_scale.opcode = LN_OpCode::SetCameraOrthoScale;
  set_camera_ortho_scale.float_expr_index = AddFloatConstant(*program, 3.25f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_camera_ortho_scale);

  LN_Instruction set_active_camera;
  set_active_camera.opcode = LN_OpCode::SetActiveCamera;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_active_camera);

  LN_Instruction make_light_unique;
  make_light_unique.opcode = LN_OpCode::MakeLightUnique;
  program->AddInstruction(LN_Event::OnFixedUpdate, make_light_unique);

  LN_Instruction set_light_color;
  set_light_color.opcode = LN_OpCode::SetLightColor;
  set_light_color.color_value = MT_Vector4(0.8f, 0.7f, 0.6f, 1.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_light_color);

  LN_Instruction set_light_power;
  set_light_power.opcode = LN_OpCode::SetLightPower;
  set_light_power.vector_value = MT_Vector3(12.5f, 0.0f, 0.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_light_power);

  LN_Instruction set_light_shadow;
  set_light_shadow.opcode = LN_OpCode::SetLightShadow;
  set_light_shadow.bool_expr_index = AddBoolConstant(*program, false);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_light_shadow);

  LN_Instruction set_window_size;
  set_window_size.opcode = LN_OpCode::SetWindowSize;
  set_window_size.int_expr_index = AddIntConstant(*program, 1600);
  set_window_size.secondary_int_expr_index = AddIntConstant(*program, 900);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_window_size);

  LN_Instruction set_fullscreen;
  set_fullscreen.opcode = LN_OpCode::SetFullscreen;
  set_fullscreen.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_fullscreen);

  LN_Instruction set_vsync;
  set_vsync.opcode = LN_OpCode::SetVSync;
  set_vsync.int_expr_index = AddIntConstant(*program, 1);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_vsync);

  LN_Instruction set_show_framerate;
  set_show_framerate.opcode = LN_OpCode::SetShowFramerate;
  set_show_framerate.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_show_framerate);

  LN_Instruction set_show_profile;
  set_show_profile.opcode = LN_OpCode::SetShowProfile;
  set_show_profile.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_show_profile);

  LN_Instruction set_cursor_visibility;
  set_cursor_visibility.opcode = LN_OpCode::SetCursorVisibility;
  set_cursor_visibility.bool_expr_index = AddBoolConstant(*program, false);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_cursor_visibility);

  LN_Instruction set_cursor_position;
  set_cursor_position.opcode = LN_OpCode::SetCursorPosition;
  set_cursor_position.vector_value = MT_Vector3(0.25f, 0.75f, 0.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_cursor_position);

  LN_Instruction set_gamepad_vibration;
  set_gamepad_vibration.opcode = LN_OpCode::SetGamepadVibration;
  set_gamepad_vibration.int_expr_index = AddIntConstant(*program, 0);
  set_gamepad_vibration.vector_value = MT_Vector3(0.4f, 0.6f, 0.25f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_gamepad_vibration);

  const uint32_t sound_name_expr = AddStringConstant(*program, "engine_loop");

  LN_Instruction stop_all_sounds;
  stop_all_sounds.opcode = LN_OpCode::StopAllSounds;
  program->AddInstruction(LN_Event::OnFixedUpdate, stop_all_sounds);

  LN_Instruction play_sound;
  play_sound.opcode = LN_OpCode::PlaySound;
  play_sound.string_expr_index = sound_name_expr;
  play_sound.float_expr_index = AddFloatConstant(*program, 0.75f);
  play_sound.secondary_float_expr_index = AddFloatConstant(*program, 1.25f);
  play_sound.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, play_sound);

  LN_Instruction play_sound_3d;
  play_sound_3d.opcode = LN_OpCode::PlaySound3D;
  play_sound_3d.string_expr_index = sound_name_expr;
  play_sound_3d.float_expr_index = AddFloatConstant(*program, 0.5f);
  play_sound_3d.secondary_float_expr_index = AddFloatConstant(*program, 0.9f);
  play_sound_3d.bool_expr_index = AddBoolConstant(*program, false);
  program->AddInstruction(LN_Event::OnFixedUpdate, play_sound_3d);

  LN_Instruction pause_sound;
  pause_sound.opcode = LN_OpCode::PauseSound;
  pause_sound.string_expr_index = sound_name_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, pause_sound);

  LN_Instruction resume_sound;
  resume_sound.opcode = LN_OpCode::ResumeSound;
  resume_sound.string_expr_index = sound_name_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, resume_sound);

  LN_Instruction stop_sound;
  stop_sound.opcode = LN_OpCode::StopSound;
  stop_sound.string_expr_index = sound_name_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, stop_sound);

  LN_Instruction play_action;
  play_action.opcode = LN_OpCode::PlayAction;
  play_action.string_expr_index = AddStringConstant(*program, "Run");
  play_action.float_expr_index = AddFloatConstant(*program, 1.0f);
  play_action.secondary_float_expr_index = AddFloatConstant(*program, 24.0f);
  play_action.tertiary_float_expr_index = AddFloatConstant(*program, 0.15f);
  play_action.quaternary_float_expr_index = AddFloatConstant(*program, 1.2f);
  play_action.secondary_vector_value = MT_Vector3(0.8f, 1.2f, 0.0f);
  play_action.int_expr_index = AddIntConstant(*program, 2);
  play_action.secondary_int_expr_index = AddIntConstant(*program, 4);
  play_action.int_value = 3;
  program->AddInstruction(LN_Event::OnFixedUpdate, play_action);

  LN_Instruction stop_action;
  stop_action.opcode = LN_OpCode::StopAction;
  stop_action.int_expr_index = AddIntConstant(*program, 2);
  program->AddInstruction(LN_Event::OnFixedUpdate, stop_action);

  LN_Instruction set_action_frame;
  set_action_frame.opcode = LN_OpCode::SetActionFrame;
  set_action_frame.int_expr_index = AddIntConstant(*program, 2);
  set_action_frame.float_expr_index = AddFloatConstant(*program, 12.5f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_action_frame);

  LN_Instruction set_bone_pose_location;
  set_bone_pose_location.opcode = LN_OpCode::SetBonePoseLocation;
  set_bone_pose_location.string_expr_index = AddStringConstant(*program, "forearm.L");
  set_bone_pose_location.vector_value = MT_Vector3(0.1f, 0.2f, 0.3f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_pose_location);

  LN_Instruction set_bone_pose_rotation;
  set_bone_pose_rotation.opcode = LN_OpCode::SetBonePoseRotation;
  set_bone_pose_rotation.string_expr_index = AddStringConstant(*program, "forearm.R");
  set_bone_pose_rotation.vector_value = MT_Vector3(0.4f, 0.5f, 0.6f);
  set_bone_pose_rotation.secondary_int_value = 1;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_pose_rotation);

  LN_Instruction set_bone_pose_transform;
  set_bone_pose_transform.opcode = LN_OpCode::SetBonePoseTransform;
  set_bone_pose_transform.string_expr_index = AddStringConstant(*program, "hand.R");
  set_bone_pose_transform.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  set_bone_pose_transform.secondary_vector_value = MT_Vector3(0.7f, 0.8f, 0.9f);
  set_bone_pose_transform.int_value = int(LN_BonePoseLocationSpace::World);
  set_bone_pose_transform.secondary_int_value = 1;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_pose_transform);

  LN_Instruction set_bone_attribute;
  set_bone_attribute.opcode = LN_OpCode::SetBoneAttribute;
  set_bone_attribute.string_expr_index = AddStringConstant(*program, "spine");
  set_bone_attribute.int_value = 2;
  set_bone_attribute.secondary_int_value = 1;
  set_bone_attribute.secondary_value_expr_index = AddFloatValueConstant(*program, 0.75f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_attribute);

  LN_Instruction set_bone_constraint_influence;
  set_bone_constraint_influence.opcode = LN_OpCode::SetBoneConstraintInfluence;
  set_bone_constraint_influence.string_expr_index = AddStringConstant(*program, "spine");
  set_bone_constraint_influence.secondary_string_expr_index = AddStringConstant(*program,
                                                                                "Copy Rotation");
  set_bone_constraint_influence.float_expr_index = AddFloatConstant(*program, 0.35f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_constraint_influence);

  LN_Instruction set_bone_constraint_target;
  set_bone_constraint_target.opcode = LN_OpCode::SetBoneConstraintTarget;
  set_bone_constraint_target.string_expr_index = AddStringConstant(*program, "spine");
  set_bone_constraint_target.secondary_string_expr_index = AddStringConstant(*program, "Track To");
  set_bone_constraint_target.secondary_value_expr_index = AddIntValueConstant(*program, 8);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_constraint_target);

  LN_Instruction set_bone_constraint_attribute;
  set_bone_constraint_attribute.opcode = LN_OpCode::SetBoneConstraintAttribute;
  set_bone_constraint_attribute.string_expr_index = AddStringConstant(*program, "spine");
  set_bone_constraint_attribute.secondary_string_expr_index = AddStringConstant(*program,
                                                                                "Limit Rotation");
  set_bone_constraint_attribute.tertiary_string_expr_index = AddStringConstant(*program,
                                                                               "influence");
  set_bone_constraint_attribute.secondary_value_expr_index = AddFloatValueConstant(*program, 0.9f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_bone_constraint_attribute);

  LN_Instruction set_material_slot;
  set_material_slot.opcode = LN_OpCode::SetMaterialSlot;
  set_material_slot.secondary_value_expr_index = AddDatablockRefConstant(*program, "damage_flash");
  set_material_slot.int_expr_index = AddIntConstant(*program, 1);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_material_slot);

  LN_Instruction set_material_parameter;
  set_material_parameter.opcode = LN_OpCode::SetMaterialParameter;
  set_material_parameter.tertiary_value_expr_index = AddDatablockRefConstant(*program, "Glow");
  set_material_parameter.string_expr_index = AddStringConstant(*program, "ShaderNodeEmission");
  set_material_parameter.secondary_string_expr_index = AddStringConstant(*program, "Strength");
  set_material_parameter.secondary_value_expr_index = AddFloatValueConstant(*program, 2.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_material_parameter);

  LN_Instruction set_material_node_socket_value;
  set_material_node_socket_value.opcode = LN_OpCode::SetMaterialNodeSocketValue;
  set_material_node_socket_value.tertiary_value_expr_index = AddDatablockRefConstant(*program,
                                                                                     "Glow");
  set_material_node_socket_value.string_expr_index = AddStringConstant(*program,
                                                                       "ShaderNodeEmission");
  set_material_node_socket_value.secondary_string_expr_index = AddStringConstant(*program,
                                                                                 "Strength");
  set_material_node_socket_value.secondary_value_expr_index = AddFloatValueConstant(*program,
                                                                                    1.5f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_material_node_socket_value);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = AddIntValueConstant(*program, 17);
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_Instruction quit_game;
  quit_game.opcode = LN_OpCode::QuitGame;
  program->AddInstruction(LN_Event::OnFixedUpdate, quit_game);

  LN_Instruction restart_game;
  restart_game.opcode = LN_OpCode::RestartGame;
  program->AddInstruction(LN_Event::OnFixedUpdate, restart_game);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = AddFloatConstant(*program, 0.5f);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_time_scale);

  LN_Instruction load_blend_file;
  load_blend_file.opcode = LN_OpCode::LoadBlendFile;
  load_blend_file.string_expr_index = AddStringConstant(*program, "//next_level.blend");
  program->AddInstruction(LN_Event::OnFixedUpdate, load_blend_file);

  LN_Instruction save_game;
  save_game.opcode = LN_OpCode::SaveGame;
  save_game.int_expr_index = AddIntConstant(*program, 2);
  save_game.string_expr_index = AddStringConstant(*program, "//slot2.sav");
  program->AddInstruction(LN_Event::OnFixedUpdate, save_game);

  LN_Instruction load_game;
  load_game.opcode = LN_OpCode::LoadGame;
  load_game.int_expr_index = AddIntConstant(*program, 3);
  load_game.string_expr_index = AddStringConstant(*program, "//slot3.sav");
  program->AddInstruction(LN_Event::OnFixedUpdate, load_game);

  LN_Instruction load_scene;
  load_scene.opcode = LN_OpCode::LoadScene;
  load_scene.string_expr_index = AddStringConstant(*program, "LevelScene");
  program->AddInstruction(LN_Event::OnFixedUpdate, load_scene);

  LN_Instruction set_scene;
  set_scene.opcode = LN_OpCode::SetScene;
  set_scene.string_expr_index = AddStringConstant(*program, "MenuScene");
  program->AddInstruction(LN_Event::OnFixedUpdate, set_scene);

  LN_Instruction remove_parent;
  remove_parent.opcode = LN_OpCode::RemoveParent;
  program->AddInstruction(LN_Event::OnFixedUpdate, remove_parent);

  LN_Instruction remove_object;
  remove_object.opcode = LN_OpCode::RemoveObject;
  program->AddInstruction(LN_Event::OnFixedUpdate, remove_object);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterWindowFullscreenFallbackCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression fullscreen;
  fullscreen.kind = LN_BoolExpressionKind::WindowFullscreen;
  const uint32_t fullscreen_index = program->AddBoolExpression(fullscreen);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = fullscreen_index;
  set_visibility.bool_value = true;
  program->AddInstruction(LN_Event::OnInit, set_visibility);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterColorCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression time_scale;
  time_scale.kind = LN_FloatExpressionKind::SnapshotTimeScale;
  const uint32_t time_scale_index = program->AddFloatExpression(time_scale);

  LN_FloatExpression base_green;
  base_green.kind = LN_FloatExpressionKind::Constant;
  base_green.float_value = 0.4f;
  const uint32_t base_green_index = program->AddFloatExpression(base_green);

  LN_FloatExpression base_blue;
  base_blue.kind = LN_FloatExpressionKind::Constant;
  base_blue.float_value = 0.6f;
  const uint32_t base_blue_index = program->AddFloatExpression(base_blue);

  LN_FloatExpression base_alpha;
  base_alpha.kind = LN_FloatExpressionKind::Constant;
  base_alpha.float_value = 0.8f;
  const uint32_t base_alpha_index = program->AddFloatExpression(base_alpha);

  LN_ColorExpression base_color;
  base_color.kind = LN_ColorExpressionKind::Combine;
  base_color.input0 = time_scale_index;
  base_color.input1 = base_green_index;
  base_color.input2 = base_blue_index;
  base_color.input3 = base_alpha_index;
  const uint32_t base_color_index = program->AddColorExpression(base_color);

  LN_FloatExpression red;
  red.kind = LN_FloatExpressionKind::ColorComponent;
  red.input0 = base_color_index;
  red.component_index = 0;
  const uint32_t red_index = program->AddFloatExpression(red);

  LN_FloatExpression green_offset;
  green_offset.kind = LN_FloatExpressionKind::Constant;
  green_offset.float_value = 0.1f;
  const uint32_t green_offset_index = program->AddFloatExpression(green_offset);

  LN_FloatExpression green_component;
  green_component.kind = LN_FloatExpressionKind::ColorComponent;
  green_component.input0 = base_color_index;
  green_component.component_index = 1;
  const uint32_t green_component_index = program->AddFloatExpression(green_component);

  LN_FloatExpression green;
  green.kind = LN_FloatExpressionKind::Add;
  green.input0 = green_component_index;
  green.input1 = green_offset_index;
  const uint32_t green_index = program->AddFloatExpression(green);

  LN_FloatExpression blue;
  blue.kind = LN_FloatExpressionKind::ColorComponent;
  blue.input0 = base_color_index;
  blue.component_index = 2;
  const uint32_t blue_index = program->AddFloatExpression(blue);

  LN_FloatExpression alpha;
  alpha.kind = LN_FloatExpressionKind::ColorComponent;
  alpha.input0 = base_color_index;
  alpha.component_index = 3;
  const uint32_t alpha_index = program->AddFloatExpression(alpha);

  LN_ColorExpression combined_color;
  combined_color.kind = LN_ColorExpressionKind::Combine;
  combined_color.input0 = red_index;
  combined_color.input1 = green_index;
  combined_color.input2 = blue_index;
  combined_color.input3 = alpha_index;
  const uint32_t combined_color_index = program->AddColorExpression(combined_color);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetObjectColor;
  instruction.color_expr_index = combined_color_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterColorSnapshotCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_ColorExpression object_color;
  object_color.kind = LN_ColorExpressionKind::SnapshotObjectColor;
  const uint32_t object_color_index = program->AddColorExpression(object_color);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetObjectColor;
  instruction.color_expr_index = object_color_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterAdvancedScalarCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression time_scale;
  time_scale.kind = LN_FloatExpressionKind::SnapshotTimeScale;
  const uint32_t time_scale_index = program->AddFloatExpression(time_scale);

  LN_FloatExpression radians;
  radians.kind = LN_FloatExpressionKind::Radians;
  radians.input0 = time_scale_index;
  const uint32_t radians_index = program->AddFloatExpression(radians);

  LN_FloatExpression sine;
  sine.kind = LN_FloatExpressionKind::Sine;
  sine.input0 = radians_index;
  const uint32_t sine_index = program->AddFloatExpression(sine);

  LN_FloatExpression threshold_value;
  threshold_value.kind = LN_FloatExpressionKind::Constant;
  threshold_value.float_value = 0.5f;
  const uint32_t threshold_value_index = program->AddFloatExpression(threshold_value);

  LN_FloatExpression threshold;
  threshold.kind = LN_FloatExpressionKind::Threshold;
  threshold.input0 = time_scale_index;
  threshold.input1 = threshold_value_index;
  threshold.bool_value = true;
  threshold.threshold_operation = LN_ThresholdOperation::Greater;
  const uint32_t threshold_index = program->AddFloatExpression(threshold);

  LN_FloatExpression range_min;
  range_min.kind = LN_FloatExpressionKind::Constant;
  range_min.float_value = 0.25f;
  const uint32_t range_min_index = program->AddFloatExpression(range_min);

  LN_FloatExpression range_max;
  range_max.kind = LN_FloatExpressionKind::Constant;
  range_max.float_value = 2.0f;
  const uint32_t range_max_index = program->AddFloatExpression(range_max);

  LN_FloatExpression ranged_threshold;
  ranged_threshold.kind = LN_FloatExpressionKind::RangedThreshold;
  ranged_threshold.input0 = time_scale_index;
  ranged_threshold.input1 = range_min_index;
  ranged_threshold.input2 = range_max_index;
  ranged_threshold.range_operation = LN_RangeOperation::Inside;
  const uint32_t ranged_threshold_index = program->AddFloatExpression(ranged_threshold);

  LN_BoolExpression select_condition;
  select_condition.kind = LN_BoolExpressionKind::FloatCompare;
  select_condition.input0 = time_scale_index;
  select_condition.input1 = threshold_value_index;
  select_condition.float_compare_operation = LN_FloatCompareOperation::GreaterThan;
  const uint32_t select_condition_index = program->AddBoolExpression(select_condition);

  LN_FloatExpression select;
  select.kind = LN_FloatExpressionKind::Select;
  select.bool_expr_index = select_condition_index;
  select.input0 = sine_index;
  select.input1 = ranged_threshold_index;
  program->AddFloatExpression(select);

  LN_FloatExpression formula;
  formula.kind = LN_FloatExpressionKind::Formula;
  formula.input0 = sine_index;
  formula.input1 = threshold_index;
  formula.string_expr_index = AddStringConstant(*program, "a + b * 2");
  const uint32_t formula_index = program->AddFloatExpression(formula);

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = sine_index;
  vector.input1 = threshold_index;
  vector.input2 = formula_index;
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = vector_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterStringPredicateCommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t text_index = AddStringConstant(*program, "logic nodes logic");
  const uint32_t logic_index = AddStringConstant(*program, "logic");
  const uint32_t nodes_index = AddStringConstant(*program, "nodes");

  LN_BoolExpression contains;
  contains.kind = LN_BoolExpressionKind::StringContains;
  contains.input0 = text_index;
  contains.input1 = nodes_index;
  const uint32_t contains_index = program->AddBoolExpression(contains);

  LN_BoolExpression starts_with;
  starts_with.kind = LN_BoolExpressionKind::StringStartsWith;
  starts_with.input0 = text_index;
  starts_with.input1 = logic_index;
  const uint32_t starts_with_index = program->AddBoolExpression(starts_with);

  LN_BoolExpression visible;
  visible.kind = LN_BoolExpressionKind::And;
  visible.input0 = contains_index;
  visible.input1 = starts_with_index;
  const uint32_t visible_index = program->AddBoolExpression(visible);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = visible_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility);

  LN_IntExpression count;
  count.kind = LN_IntExpressionKind::StringCount;
  count.input0 = text_index;
  count.input1 = logic_index;
  const uint32_t count_index = program->AddIntExpression(count);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = count_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group);

  return program;
}

std::shared_ptr<LN_Program> CreateRegisterDivideSoACommandProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression time_scale;
  time_scale.kind = LN_FloatExpressionKind::SnapshotTimeScale;
  const uint32_t time_scale_index = program->AddFloatExpression(time_scale);

  const float divisors[4] = {2.0f, 0.0f, 4.0f, 1.0e-25f};
  uint32_t divisor_indices[4];
  for (uint32_t index = 0; index < 4; index++) {
    LN_FloatExpression divisor;
    divisor.kind = LN_FloatExpressionKind::Constant;
    divisor.float_value = divisors[index];
    divisor_indices[index] = program->AddFloatExpression(divisor);
  }

  uint32_t divide_indices[4];
  for (uint32_t index = 0; index < 4; index++) {
    LN_FloatExpression divide;
    divide.kind = LN_FloatExpressionKind::Divide;
    divide.input0 = time_scale_index;
    divide.input1 = divisor_indices[index];
    divide_indices[index] = program->AddFloatExpression(divide);
  }

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = divide_indices[0];
  vector.input1 = divide_indices[1];
  vector.input2 = divide_indices[3];
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = vector_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  return program;
}

std::shared_ptr<LN_Program> CreateSnapshotQueryGuardProgram(
    const LN_QueryExpressionKind query_kind = LN_QueryExpressionKind::MouseOver)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t disabled_condition = AddBoolConstant(*program, false);

  LN_QueryExpression query;
  query.kind = query_kind;
  query.condition_bool_expr_index = disabled_condition;
  const uint32_t query_index = program->AddQueryExpression(query);

  LN_BoolExpression query_hit;
  query_hit.kind = LN_BoolExpressionKind::PhysicsQueryHit;
  query_hit.input0 = query_index;
  const uint32_t query_hit_index = program->AddBoolExpression(query_hit);

  LN_Instruction set_position;
  set_position.opcode = LN_OpCode::SetTransformVector;
  set_position.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  set_position.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  set_position.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  set_position.bool_guard_expr_index = query_hit_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_position);

  return program;
}

std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> MakeDeterminismWorkItems(
    std::vector<std::unique_ptr<LN_RuntimeTree>> &trees)
{
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({5, trees[0].get()});
  work_items.push_back({2, trees[1].get()});
  work_items.push_back({4, trees[2].get()});
  return work_items;
}

std::vector<std::unique_ptr<LN_RuntimeTree>> MakeDeterminismRuntimeTrees()
{
  std::shared_ptr<LN_Program> worker_program = CreateDeterministicWorkerCommandProgram();
  std::shared_ptr<LN_Program> main_thread_program = CreateDeterministicMainThreadCommandProgram();
  std::shared_ptr<const LN_Program> transform_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(9.0f, 8.0f, 7.0f));

  EXPECT_TRUE(worker_program->IsParallelEligible());
  EXPECT_FALSE(main_thread_program->IsParallelEligible());
  EXPECT_TRUE(transform_program->IsParallelEligible());

  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  trees.push_back(
      std::make_unique<LN_RuntimeTree>(worker_program, FakeGameObjectPointer(50), 5, 0));
  trees.push_back(
      std::make_unique<LN_RuntimeTree>(main_thread_program, FakeGameObjectPointer(20), 2, 0));
  trees.push_back(
      std::make_unique<LN_RuntimeTree>(transform_program, FakeGameObjectPointer(40), 4, 0));
  return trees;
}

std::vector<LN_CommandBuffer::Command> MergeCommandLists(
    std::vector<LN_CommandBuffer::RecordedCommandList> command_lists)
{
  LN_CommandBuffer command_buffer;
  command_buffer.MergeRecordedCommandLists(std::move(command_lists));
  return command_buffer.GetCommandsForTests();
}

std::vector<LN_CommandBuffer::Command> RecordSerialCommandStream(
    std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items,
    const LN_TickContext &context)
{
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  command_lists.reserve(work_items.size());
  for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item : work_items) {
    LN_CommandBuffer::RecordedCommandList command_list;
    command_list.runtime_tree_index = item.runtime_tree_index;
    command_list.commands = RecordReadyCommands(*item.runtime_tree, context);
    command_lists.push_back(std::move(command_list));
  }
  return MergeCommandLists(std::move(command_lists));
}

std::vector<LN_CommandBuffer::Command> RecordSchedulerCommandStream(
    std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items,
    const LN_TickContext &context)
{
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> parallel_items;
  for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item : work_items) {
    const std::shared_ptr<const LN_Program> program = item.runtime_tree->GetProgram();
    if (program != nullptr && program->IsParallelEligible()) {
      parallel_items.push_back(item);
      continue;
    }

    LN_CommandBuffer::RecordedCommandList command_list;
    command_list.runtime_tree_index = item.runtime_tree_index;
    command_list.commands = RecordReadyCommands(*item.runtime_tree, context);
    command_lists.push_back(std::move(command_list));
  }

  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> parallel_command_lists =
      executor.ExecuteTreesToCommandLists(parallel_items, context);
  command_lists.insert(command_lists.end(),
                       std::make_move_iterator(parallel_command_lists.begin()),
                       std::make_move_iterator(parallel_command_lists.end()));
  return MergeCommandLists(std::move(command_lists));
}

void ExpectVectorNear(const MT_Vector3 &actual, const MT_Vector3 &expected)
{
  EXPECT_NEAR(actual.x(), expected.x(), 0.0001f);
  EXPECT_NEAR(actual.y(), expected.y(), 0.0001f);
  EXPECT_NEAR(actual.z(), expected.z(), 0.0001f);
}

void ExpectColorNear(const MT_Vector4 &actual, const MT_Vector4 &expected)
{
  EXPECT_NEAR(actual.x(), expected.x(), 0.0001f);
  EXPECT_NEAR(actual.y(), expected.y(), 0.0001f);
  EXPECT_NEAR(actual.z(), expected.z(), 0.0001f);
  EXPECT_NEAR(actual.w(), expected.w(), 0.0001f);
}

void ExpectValueEqual(const LN_Value &actual, const LN_Value &expected)
{
  EXPECT_EQ(actual.type, expected.type);
  EXPECT_EQ(actual.exists, expected.exists);
  EXPECT_EQ(actual.bool_value, expected.bool_value);
  EXPECT_EQ(actual.int_value, expected.int_value);
  EXPECT_NEAR(actual.float_value, expected.float_value, 0.0001f);
  ExpectVectorNear(actual.vector_value, expected.vector_value);
  ExpectColorNear(actual.color_value, expected.color_value);
  EXPECT_EQ(actual.string_value, expected.string_value);
}

void ExpectCommandEqual(const LN_CommandBuffer::Command &actual,
                        const LN_CommandBuffer::Command &expected)
{
  EXPECT_EQ(actual.type, expected.type);
  EXPECT_EQ(LN_CommandBuffer::GetCommandSubsystem(actual),
            LN_CommandBuffer::GetCommandSubsystem(expected));
  EXPECT_EQ(actual.object, expected.object);
  EXPECT_EQ(actual.event_target_object, expected.event_target_object);
  EXPECT_EQ(actual.runtime_tree != nullptr, expected.runtime_tree != nullptr);
  ExpectVectorNear(actual.vector_value, expected.vector_value);
  ExpectVectorNear(actual.secondary_vector_value, expected.secondary_vector_value);
  ExpectColorNear(actual.color_value, expected.color_value);
  ExpectValueEqual(actual.property_value, expected.property_value);
  EXPECT_EQ(actual.property_name, expected.property_name);
  EXPECT_EQ(actual.secondary_property_name, expected.secondary_property_name);
  EXPECT_EQ(actual.tertiary_property_name, expected.tertiary_property_name);
  EXPECT_EQ(actual.quaternary_property_name, expected.quaternary_property_name);
  EXPECT_EQ(actual.property_name_id.index, expected.property_name_id.index);
  EXPECT_NEAR(actual.scalar_value, expected.scalar_value, 0.0001f);
  EXPECT_EQ(actual.int_value, expected.int_value);
  EXPECT_EQ(actual.secondary_int_value, expected.secondary_int_value);
  EXPECT_EQ(actual.property_ref_index, expected.property_ref_index);
  EXPECT_EQ(actual.bool_value, expected.bool_value);
  EXPECT_EQ(actual.secondary_bool_value, expected.secondary_bool_value);
  EXPECT_EQ(actual.sort_key, expected.sort_key);
  EXPECT_EQ(actual.source_ref_index, expected.source_ref_index);
  EXPECT_EQ(actual.animation_flags, expected.animation_flags);
}

void ExpectCommandStreamsEqual(const std::vector<LN_CommandBuffer::Command> &actual,
                               const std::vector<LN_CommandBuffer::Command> &expected)
{
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t index = 0; index < actual.size(); index++) {
    SCOPED_TRACE(index);
    ExpectCommandEqual(actual[index], expected[index]);
  }
}

TEST(LN_RuntimeTree, ExecutesHardcodedOnInitOnce)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  KX_GameObject *fake_game_object = FakeGameObjectPointer(1);
  LN_RuntimeTree runtime_tree(program, fake_game_object, 4, 7);
  LN_CommandBuffer command_buffer;
  LN_TickContext context;
  context.use_fixed_timestep = true;

  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  ASSERT_EQ(command_buffer.Size(), 1);
  const LN_CommandBuffer::Command &command = command_buffer.GetCommandsForTests().front();
  EXPECT_EQ(command.type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(command.object, fake_game_object);
  EXPECT_LT((command.vector_value - MT_Vector3(1.0f, 2.0f, 3.0f)).length(), 0.0001f);

  command_buffer.Clear();
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  EXPECT_EQ(command_buffer.Size(), 0);
}

TEST(LN_RuntimeTree, StaticGamePropertyCommandUsesPropertyRefWithoutNameCopy)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_GamePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;
  const uint32_t property_index = program->AddGamePropertyRef(property_ref);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetGameProperty;
  instruction.property_ref_index = property_index;
  instruction.property_value_type = LN_ValueType::Int;
  instruction.int_expr_index = AddIntConstant(*program, 17);
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(60), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetGameProperty);
  EXPECT_EQ(commands[0].runtime_tree, &runtime_tree);
  EXPECT_EQ(commands[0].property_ref_index, property_index);
  EXPECT_TRUE(commands[0].property_name.empty());
  EXPECT_TRUE(commands[0].property_name_id.IsValid());
  ASSERT_NE(commands[0].property_name_ptr, nullptr);
  EXPECT_EQ(*commands[0].property_name_ptr, "score");
  EXPECT_EQ(program->GetString(commands[0].property_name_id), "score");
}

TEST(LN_RuntimeTree, StaticEventCommandUsesStringIdWithoutNameCopy)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t subject_index = AddStringConstant(*program, "round_started");

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SendEvent;
  instruction.string_expr_index = subject_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  ASSERT_LT(subject_index, ir.string_expression_registers.size());
  EXPECT_NE(ir.string_expression_registers[subject_index], LN_INVALID_INDEX);
  EXPECT_EQ(ir.string_id_register_count, 1u);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(61), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  context.use_register_expression_evaluator = true;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SendEvent);
  EXPECT_EQ(commands[0].runtime_tree, &runtime_tree);
  EXPECT_TRUE(commands[0].property_name.empty());
  EXPECT_TRUE(commands[0].property_name_id.IsValid());
  EXPECT_EQ(program->GetString(commands[0].property_name_id), "round_started");
}

TEST(LN_RuntimeTree, TargetedSendEventWithNullDynamicTargetDoesNotBroadcast)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t subject_index = AddStringConstant(*program, "round_started");
  const uint32_t null_target_index = AddNoneValueConstant(*program);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SendEvent;
  instruction.string_expr_index = subject_index;
  instruction.int_expr_index = null_target_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(61), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  EXPECT_TRUE(commands.empty());
}

TEST(LN_RuntimeTree, TargetedReceiveEventWithNullDynamicTargetDoesNotMatchBroadcast)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t subject_index = AddStringConstant(*program, "damage");
  const uint32_t null_target_index = AddNoneValueConstant(*program);

  LN_BoolExpression received;
  received.kind = LN_BoolExpressionKind::EventReceived;
  received.input0 = subject_index;
  received.input1 = null_target_index;
  received.bool_value = true;
  const uint32_t received_index = program->AddBoolExpression(received);

  LN_ValueExpression content;
  content.kind = LN_ValueExpressionKind::EventContent;
  content.input0 = subject_index;
  content.input1 = null_target_index;
  content.value.exists = true;
  content.value.bool_value = true;
  const uint32_t content_index = program->AddValueExpression(content);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = received_index;
  print.value_expr_index = content_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  KX_GameObject *owner = FakeGameObjectPointer(0x70);
  LN_RuntimeTree runtime_tree(program, owner, 0, 0);
  runtime_tree.SetLogicManager(&manager);

  LN_EventEntry event;
  event.subject = "damage";
  event.content.type = LN_ValueType::Int;
  event.content.exists = true;
  event.content.int_value = 42;
  manager.PushEvent(event);
  const_cast<LN_EventBus &>(manager.GetEventBus()).BeginTick();

  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  EXPECT_TRUE(commands.empty());
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentTypedValueCommand)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_ValueExpression value;
  value.kind = LN_ValueExpressionKind::FromInt;
  value.input0 = AddIntConstant(*program, 23);
  const uint32_t value_index = program->AddValueExpression(value);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  ASSERT_LT(value_index, ir.value_expression_registers.size());
  EXPECT_NE(ir.value_expression_registers[value_index], LN_INVALID_INDEX);
  EXPECT_EQ(ir.generic_value_register_count, 1u);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(62), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  context.use_register_expression_evaluator = true;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "23");
}

TEST(LN_RuntimeTree, ValueChangedToFiresWhenElapsedTimeCrossesTarget)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression elapsed_time;
  elapsed_time.kind = LN_FloatExpressionKind::SnapshotElapsedTime;
  const uint32_t elapsed_time_index = program->AddFloatExpression(elapsed_time);

  LN_ValueExpression elapsed_value;
  elapsed_value.kind = LN_ValueExpressionKind::FromFloat;
  elapsed_value.input0 = elapsed_time_index;
  const uint32_t elapsed_value_index = program->AddValueExpression(elapsed_value);

  const uint32_t target_index = AddFloatValueConstant(*program, 1.0f);

  LN_BoolExpression changed_to;
  changed_to.kind = LN_BoolExpressionKind::ValueChangedTo;
  changed_to.input0 = elapsed_value_index;
  changed_to.input1 = target_index;
  const uint32_t changed_to_index = program->AddBoolExpression(changed_to);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = changed_to_index;
  print.value_expr_index = elapsed_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  std::unique_ptr<KX_GameObject> owner = std::make_unique<KX_GameObject>();
  LN_RuntimeTree runtime_tree(program, owner.get(), 0, 0);

  LN_TickContext context;
  context.tick_index = 1;
  LN_TickReadContext read_context;
  read_context.has_timing = true;
  read_context.elapsed_time = 0.95f;
  read_context.tick_index = context.tick_index;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, context.tick_index);
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 2;
  read_context.tick_index = context.tick_index;
  read_context.elapsed_time = 1.05f;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, context.tick_index);
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "1.05");
}

TEST(LN_RuntimeTree, ValueChangedOldNewAreLatchedAfterGuardEvaluation)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Value initial_value;
  initial_value.type = LN_ValueType::Float;
  initial_value.exists = true;
  initial_value.float_value = 0.5f;

  LN_TreePropertyRef property_ref;
  property_ref.name = "watched";
  property_ref.value_type = LN_ValueType::Float;
  property_ref.default_value = initial_value;
  const uint32_t property_ref_index = program->AddTreePropertyRef(property_ref);

  LN_ValueExpression property_value;
  property_value.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  property_value.property_ref_index = property_ref_index;
  const uint32_t property_value_index = program->AddValueExpression(property_value);

  const uint32_t target_index = AddFloatValueConstant(*program, 1.0f);

  LN_BoolExpression changed_to;
  changed_to.kind = LN_BoolExpressionKind::ValueChangedTo;
  changed_to.input0 = property_value_index;
  changed_to.input1 = target_index;
  const uint32_t changed_to_index = program->AddBoolExpression(changed_to);

  LN_ValueExpression old_value;
  old_value.kind = LN_ValueExpressionKind::ValueChangedOld;
  old_value.input0 = changed_to_index;
  const uint32_t old_value_index = program->AddValueExpression(old_value);

  LN_Instruction print_old;
  print_old.opcode = LN_OpCode::Print;
  print_old.bool_guard_expr_index = changed_to_index;
  print_old.value_expr_index = old_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print_old);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(65), 0, 0);

  LN_TickContext context;
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  LN_Value updated_value = initial_value;
  updated_value.float_value = 1.2f;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, updated_value));

  context.tick_index = 2;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "0.5");
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentRuntimeTreeValuePropertyCommand)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Value score_value;
  score_value.type = LN_ValueType::Int;
  score_value.exists = true;
  score_value.int_value = 7;

  LN_Value label_value;
  label_value.type = LN_ValueType::String;
  label_value.exists = true;
  label_value.string_value = "runtime";

  LN_Value property_value;
  property_value.type = LN_ValueType::Dict;
  property_value.exists = true;
  property_value.dict_value["score"] = score_value;
  property_value.dict_value["label"] = label_value;

  LN_TreePropertyRef property_ref;
  property_ref.name = "payload";
  property_ref.value_type = LN_ValueType::Dict;
  property_ref.default_value = property_value;
  const uint32_t property_ref_index = program->AddTreePropertyRef(property_ref);

  LN_ValueExpression property_expression;
  property_expression.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  property_expression.property_ref_index = property_ref_index;
  const uint32_t property_expression_index = program->AddValueExpression(property_expression);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = property_expression_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_TickContext reference_context;
  reference_context.tick_index = 1;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(63), 0, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.tick_index = 1;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(63), 0, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentGenericValueConsumerCommands)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t int_value_index = AddIntValueConstant(*program, 23);
  const uint32_t none_value_index = AddNoneValueConstant(*program);
  const uint32_t float_value_index = AddFloatValueConstant(*program, 0.5f);

  LN_ValueExpression vector_value;
  vector_value.kind = LN_ValueExpressionKind::Constant;
  vector_value.value.type = LN_ValueType::Vector;
  vector_value.value.exists = true;
  vector_value.value.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_value_index = program->AddValueExpression(vector_value);

  LN_BoolExpression bool_from_value;
  bool_from_value.kind = LN_BoolExpressionKind::FromGenericValue;
  bool_from_value.input0 = int_value_index;
  const uint32_t bool_from_value_index = program->AddBoolExpression(bool_from_value);

  LN_Instruction set_visibility_from_value;
  set_visibility_from_value.opcode = LN_OpCode::SetVisibility;
  set_visibility_from_value.bool_expr_index = bool_from_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility_from_value);

  LN_BoolExpression value_is_none;
  value_is_none.kind = LN_BoolExpressionKind::ValueIsNone;
  value_is_none.input0 = none_value_index;
  const uint32_t value_is_none_index = program->AddBoolExpression(value_is_none);

  LN_Instruction set_visibility_from_none;
  set_visibility_from_none.opcode = LN_OpCode::SetVisibility;
  set_visibility_from_none.bool_expr_index = value_is_none_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility_from_none);

  LN_BoolExpression value_compare;
  value_compare.kind = LN_BoolExpressionKind::ValueCompare;
  value_compare.input0 = int_value_index;
  value_compare.input1 = int_value_index;
  value_compare.float_compare_operation = LN_FloatCompareOperation::Equal;
  const uint32_t value_compare_index = program->AddBoolExpression(value_compare);

  LN_Instruction set_visibility_from_compare;
  set_visibility_from_compare.opcode = LN_OpCode::SetVisibility;
  set_visibility_from_compare.bool_expr_index = value_compare_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility_from_compare);

  LN_IntExpression int_from_value;
  int_from_value.kind = LN_IntExpressionKind::FromGenericValue;
  int_from_value.input0 = int_value_index;
  const uint32_t int_from_value_index = program->AddIntExpression(int_from_value);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = int_from_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group);

  LN_FloatExpression float_from_value;
  float_from_value.kind = LN_FloatExpressionKind::FromGenericValue;
  float_from_value.input0 = float_value_index;
  const uint32_t float_from_value_index = program->AddFloatExpression(float_from_value);

  LN_Instruction set_time_scale;
  set_time_scale.opcode = LN_OpCode::SetTimeScale;
  set_time_scale.float_expr_index = float_from_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_time_scale);

  LN_VectorExpression vector_from_value;
  vector_from_value.kind = LN_VectorExpressionKind::FromGenericValue;
  vector_from_value.input0 = vector_value_index;
  const uint32_t vector_from_value_index = program->AddVectorExpression(vector_from_value);

  LN_Instruction set_world_position;
  set_world_position.opcode = LN_OpCode::SetWorldPosition;
  set_world_position.vector_expr_index = vector_from_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_world_position);

  LN_ColorExpression color_from_value;
  color_from_value.kind = LN_ColorExpressionKind::FromGenericValue;
  color_from_value.input0 = vector_value_index;
  const uint32_t color_from_value_index = program->AddColorExpression(color_from_value);

  LN_Instruction set_object_color;
  set_object_color.opcode = LN_OpCode::SetObjectColor;
  set_object_color.color_expr_index = color_from_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_object_color);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_NE(ir.bool_expression_registers[value_is_none_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.bool_expression_registers[bool_from_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.bool_expression_registers[value_compare_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[int_from_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.float_expression_registers[float_from_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.vector_expression_registers[vector_from_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.color_expression_registers[color_from_value_index], LN_INVALID_INDEX);

  LN_TickContext reference_context;
  reference_context.tick_index = 1;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(63), 0, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context = reference_context;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(63), 0, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 7u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_TRUE(register_commands[0].bool_value);
  EXPECT_TRUE(register_commands[1].bool_value);
  EXPECT_TRUE(register_commands[2].bool_value);
  EXPECT_EQ(register_commands[3].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[3].int_value, 23);
  EXPECT_EQ(register_commands[4].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_NEAR(register_commands[4].scalar_value, 0.5f, 0.0001f);
  EXPECT_EQ(register_commands[5].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  ExpectVectorNear(register_commands[5].vector_value, MT_Vector3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(register_commands[6].type, LN_CommandBuffer::CommandType::SetObjectColor);
  ExpectColorNear(register_commands[6].color_value, MT_Vector4(1.0f, 2.0f, 3.0f, 1.0f));
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentPureValueProducerCommands)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t int_value_index = AddIntValueConstant(*program, 23);
  const uint32_t float_value_index = AddFloatValueConstant(*program, 0.5f);
  const uint32_t none_value_index = AddNoneValueConstant(*program);
  const uint32_t bool_index = AddBoolConstant(*program, true);
  const uint32_t vector_index = AddVectorConstant(*program, MT_Vector3(0.1f, 0.2f, 0.3f));

  LN_ValueExpression select_value;
  select_value.kind = LN_ValueExpressionKind::Select;
  select_value.bool_expr_index = bool_index;
  select_value.input0 = int_value_index;
  select_value.input1 = float_value_index;
  const uint32_t select_value_index = program->AddValueExpression(select_value);

  LN_ValueExpression rotation_value;
  rotation_value.kind = LN_ValueExpressionKind::FromRotation;
  rotation_value.input0 = vector_index;
  const uint32_t rotation_value_index = program->AddValueExpression(rotation_value);

  const uint32_t string_key_index = AddStringConstant(*program, "logic");
  LN_ValueExpression string_value;
  string_value.kind = LN_ValueExpressionKind::FromString;
  string_value.input0 = string_key_index;
  const uint32_t string_value_index = program->AddValueExpression(string_value);

  const uint32_t float_index = AddFloatConstant(*program, 0.25f);
  LN_ValueExpression vector4_value;
  vector4_value.kind = LN_ValueExpressionKind::CombineVector4;
  vector4_value.input_indices = {float_index, float_index, float_index, float_index};
  const uint32_t vector4_value_index = program->AddValueExpression(vector4_value);

  LN_ValueExpression vector_value;
  vector_value.kind = LN_ValueExpressionKind::FromVector;
  vector_value.input0 = vector_index;
  const uint32_t vector_value_index = program->AddValueExpression(vector_value);

  LN_ValueExpression resized_value;
  resized_value.kind = LN_ValueExpressionKind::ResizeVectorValue;
  resized_value.input0 = vector_value_index;
  resized_value.property_ref_index = 2;
  const uint32_t resized_value_index = program->AddValueExpression(resized_value);

  LN_ValueExpression matrix_value;
  matrix_value.kind = LN_ValueExpressionKind::EulerToMatrix;
  matrix_value.input0 = vector_index;
  const uint32_t matrix_value_index = program->AddValueExpression(matrix_value);

  LN_ValueExpression euler_value;
  euler_value.kind = LN_ValueExpressionKind::MatrixToEuler;
  euler_value.input0 = matrix_value_index;
  euler_value.value.type = LN_ValueType::Vector;
  const uint32_t euler_value_index = program->AddValueExpression(euler_value);

  LN_ValueExpression rotation_constant;
  rotation_constant.kind = LN_ValueExpressionKind::Constant;
  rotation_constant.value.type = LN_ValueType::Rotation;
  rotation_constant.value.exists = true;
  rotation_constant.value.rotation_euler_value = MT_Vector3(0.1f, 0.2f, 0.3f);
  const uint32_t rotation_constant_index = program->AddValueExpression(rotation_constant);

  LN_ValueExpression vector4_constant;
  vector4_constant.kind = LN_ValueExpressionKind::Constant;
  vector4_constant.value.type = LN_ValueType::Vector4;
  vector4_constant.value.exists = true;
  vector4_constant.value.vector4_value = MT_Vector4(1.0f, 2.0f, 3.0f, 4.0f);
  const uint32_t vector4_constant_index = program->AddValueExpression(vector4_constant);

  LN_ValueExpression matrix_constant;
  matrix_constant.kind = LN_ValueExpressionKind::Constant;
  matrix_constant.value.type = LN_ValueType::Matrix;
  matrix_constant.value.exists = true;
  matrix_constant.value.matrix_value = MT_Matrix3x3(MT_Vector3(0.1f, 0.2f, 0.3f));
  const uint32_t matrix_constant_index = program->AddValueExpression(matrix_constant);

  LN_ValueExpression empty_dict;
  empty_dict.kind = LN_ValueExpressionKind::EmptyDict;
  const uint32_t empty_dict_index = program->AddValueExpression(empty_dict);

  LN_ValueExpression make_dict;
  make_dict.kind = LN_ValueExpressionKind::MakeDict;
  make_dict.input0 = string_key_index;
  make_dict.input1 = int_value_index;
  const uint32_t make_dict_index = program->AddValueExpression(make_dict);

  LN_ValueExpression dict_get_key;
  dict_get_key.kind = LN_ValueExpressionKind::DictGetKey;
  dict_get_key.input0 = make_dict_index;
  dict_get_key.input1 = string_key_index;
  dict_get_key.input2 = float_value_index;
  const uint32_t dict_get_key_index = program->AddValueExpression(dict_get_key);

  LN_ValueExpression dict_set_key;
  dict_set_key.kind = LN_ValueExpressionKind::DictSetKey;
  dict_set_key.input0 = make_dict_index;
  dict_set_key.input1 = string_key_index;
  dict_set_key.input2 = float_value_index;
  const uint32_t dict_set_key_index = program->AddValueExpression(dict_set_key);

  LN_ValueExpression dict_remove_key;
  dict_remove_key.kind = LN_ValueExpressionKind::DictRemoveKey;
  dict_remove_key.input0 = dict_set_key_index;
  dict_remove_key.input1 = string_key_index;
  const uint32_t dict_remove_key_index = program->AddValueExpression(dict_remove_key);

  LN_ValueExpression dict_remove_key_value;
  dict_remove_key_value.kind = LN_ValueExpressionKind::DictRemoveKeyValue;
  dict_remove_key_value.input0 = dict_set_key_index;
  dict_remove_key_value.input1 = string_key_index;
  const uint32_t dict_remove_key_value_index = program->AddValueExpression(dict_remove_key_value);

  LN_ValueExpression dict_merge;
  dict_merge.kind = LN_ValueExpressionKind::DictMerge;
  dict_merge.input0 = empty_dict_index;
  dict_merge.input1 = make_dict_index;
  const uint32_t dict_merge_index = program->AddValueExpression(dict_merge);

  LN_ValueExpression dict_get_keys;
  dict_get_keys.kind = LN_ValueExpressionKind::DictGetKeys;
  dict_get_keys.input0 = dict_merge_index;
  const uint32_t dict_get_keys_index = program->AddValueExpression(dict_get_keys);

  const uint32_t list_length_source_index = AddIntConstant(*program, 2);
  LN_ValueExpression empty_list;
  empty_list.kind = LN_ValueExpressionKind::EmptyList;
  empty_list.input0 = list_length_source_index;
  const uint32_t empty_list_index = program->AddValueExpression(empty_list);

  LN_ValueExpression make_list;
  make_list.kind = LN_ValueExpressionKind::MakeList;
  make_list.input0 = int_value_index;
  make_list.input1 = float_value_index;
  make_list.input2 = empty_dict_index;
  const uint32_t make_list_index = program->AddValueExpression(make_list);

  LN_ValueExpression list_from_items;
  list_from_items.kind = LN_ValueExpressionKind::ListFromItems;
  list_from_items.input_indices = {
      int_value_index,
      float_value_index,
      empty_dict_index,
      empty_list_index,
      make_list_index,
  };
  const uint32_t list_from_items_index = program->AddValueExpression(list_from_items);

  LN_ValueExpression list_duplicate;
  list_duplicate.kind = LN_ValueExpressionKind::ListDuplicate;
  list_duplicate.input0 = empty_list_index;
  const uint32_t list_duplicate_index = program->AddValueExpression(list_duplicate);

  LN_ValueExpression list_extend;
  list_extend.kind = LN_ValueExpressionKind::ListExtend;
  list_extend.input0 = list_duplicate_index;
  list_extend.input1 = empty_list_index;
  const uint32_t list_extend_index = program->AddValueExpression(list_extend);

  LN_ValueExpression list_append;
  list_append.kind = LN_ValueExpressionKind::ListAppend;
  list_append.input0 = list_extend_index;
  list_append.input1 = int_value_index;
  const uint32_t list_append_index = program->AddValueExpression(list_append);

  const uint32_t negative_index_index = AddIntConstant(*program, -1);
  LN_ValueExpression list_remove_index;
  list_remove_index.kind = LN_ValueExpressionKind::ListRemoveIndex;
  list_remove_index.input0 = list_append_index;
  list_remove_index.input1 = negative_index_index;
  const uint32_t list_remove_index_index = program->AddValueExpression(list_remove_index);

  LN_ValueExpression list_remove_value;
  list_remove_value.kind = LN_ValueExpressionKind::ListRemoveValue;
  list_remove_value.input0 = list_remove_index_index;
  list_remove_value.input1 = none_value_index;
  const uint32_t list_remove_value_index = program->AddValueExpression(list_remove_value);

  LN_ValueExpression list_set_index;
  list_set_index.kind = LN_ValueExpressionKind::ListSetIndex;
  list_set_index.input0 = list_remove_value_index;
  list_set_index.input1 = negative_index_index;
  list_set_index.input2 = int_value_index;
  const uint32_t list_set_index_index = program->AddValueExpression(list_set_index);

  const uint32_t first_list_index_index = AddIntConstant(*program, 0);
  LN_ValueExpression list_element;
  list_element.kind = LN_ValueExpressionKind::ListElement;
  list_element.input0 = list_set_index_index;
  list_element.input1 = first_list_index_index;
  const uint32_t list_element_index = program->AddValueExpression(list_element);

  LN_ValueExpression list_random_item;
  list_random_item.kind = LN_ValueExpressionKind::ListRandomItem;
  list_random_item.input0 = list_set_index_index;
  const uint32_t list_random_item_index = program->AddValueExpression(list_random_item);

  LN_ValueExpression value_switch_list;
  value_switch_list.kind = LN_ValueExpressionKind::ValueSwitchList;
  value_switch_list.input_indices = {
      bool_index,
      int_value_index,
      LN_INVALID_INDEX,
      float_value_index,
  };
  const uint32_t value_switch_list_index = program->AddValueExpression(value_switch_list);

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
  const uint32_t value_switch_compare_index = program->AddValueExpression(value_switch_compare);

  LN_IntExpression dict_length;
  dict_length.kind = LN_IntExpressionKind::DictLength;
  dict_length.input0 = empty_dict_index;
  const uint32_t dict_length_index = program->AddIntExpression(dict_length);

  LN_IntExpression make_dict_length;
  make_dict_length.kind = LN_IntExpressionKind::DictLength;
  make_dict_length.input0 = make_dict_index;
  const uint32_t make_dict_length_index = program->AddIntExpression(make_dict_length);

  LN_IntExpression dict_set_key_length;
  dict_set_key_length.kind = LN_IntExpressionKind::DictLength;
  dict_set_key_length.input0 = dict_set_key_index;
  const uint32_t dict_set_key_length_index = program->AddIntExpression(dict_set_key_length);

  LN_IntExpression dict_remove_key_length;
  dict_remove_key_length.kind = LN_IntExpressionKind::DictLength;
  dict_remove_key_length.input0 = dict_remove_key_index;
  const uint32_t dict_remove_key_length_index = program->AddIntExpression(dict_remove_key_length);

  LN_IntExpression dict_merge_length;
  dict_merge_length.kind = LN_IntExpressionKind::DictLength;
  dict_merge_length.input0 = dict_merge_index;
  const uint32_t dict_merge_length_index = program->AddIntExpression(dict_merge_length);

  LN_IntExpression dict_get_keys_length;
  dict_get_keys_length.kind = LN_IntExpressionKind::ListLength;
  dict_get_keys_length.input0 = dict_get_keys_index;
  const uint32_t dict_get_keys_length_index = program->AddIntExpression(dict_get_keys_length);

  LN_IntExpression list_length;
  list_length.kind = LN_IntExpressionKind::ListLength;
  list_length.input0 = empty_list_index;
  const uint32_t list_length_index = program->AddIntExpression(list_length);

  LN_IntExpression make_list_length;
  make_list_length.kind = LN_IntExpressionKind::ListLength;
  make_list_length.input0 = make_list_index;
  const uint32_t make_list_length_index = program->AddIntExpression(make_list_length);

  LN_IntExpression list_from_items_length;
  list_from_items_length.kind = LN_IntExpressionKind::ListLength;
  list_from_items_length.input0 = list_from_items_index;
  const uint32_t list_from_items_length_index = program->AddIntExpression(list_from_items_length);

  LN_IntExpression list_duplicate_length;
  list_duplicate_length.kind = LN_IntExpressionKind::ListLength;
  list_duplicate_length.input0 = list_duplicate_index;
  const uint32_t list_duplicate_length_index = program->AddIntExpression(list_duplicate_length);

  LN_IntExpression list_extend_length;
  list_extend_length.kind = LN_IntExpressionKind::ListLength;
  list_extend_length.input0 = list_extend_index;
  const uint32_t list_extend_length_index = program->AddIntExpression(list_extend_length);

  LN_IntExpression list_append_length;
  list_append_length.kind = LN_IntExpressionKind::ListLength;
  list_append_length.input0 = list_append_index;
  const uint32_t list_append_length_index = program->AddIntExpression(list_append_length);

  LN_IntExpression list_remove_index_length;
  list_remove_index_length.kind = LN_IntExpressionKind::ListLength;
  list_remove_index_length.input0 = list_remove_index_index;
  const uint32_t list_remove_index_length_index = program->AddIntExpression(
      list_remove_index_length);

  LN_IntExpression list_remove_value_length;
  list_remove_value_length.kind = LN_IntExpressionKind::ListLength;
  list_remove_value_length.input0 = list_remove_value_index;
  const uint32_t list_remove_value_length_index = program->AddIntExpression(
      list_remove_value_length);

  LN_IntExpression list_set_index_length;
  list_set_index_length.kind = LN_IntExpressionKind::ListLength;
  list_set_index_length.input0 = list_set_index_index;
  const uint32_t list_set_index_length_index = program->AddIntExpression(list_set_index_length);

  const uint32_t value_indices[] = {
      select_value_index,         rotation_value_index,
      string_value_index,         vector4_value_index,
      resized_value_index,        matrix_value_index,
      euler_value_index,          rotation_constant_index,
      vector4_constant_index,     matrix_constant_index,
      empty_dict_index,           make_dict_index,
      dict_get_key_index,         dict_set_key_index,
      dict_remove_key_index,      dict_remove_key_value_index,
      dict_merge_index,           dict_get_keys_index,
      empty_list_index,           make_list_index,
      list_from_items_index,      list_duplicate_index,
      list_extend_index,          list_append_index,
      list_remove_index_index,    list_remove_value_index,
      list_set_index_index,       list_element_index,
      list_random_item_index,     value_switch_list_index,
      value_switch_compare_index,
  };
  for (const uint32_t value_index : value_indices) {
    LN_Instruction print;
    print.opcode = LN_OpCode::Print;
    print.value_expr_index = value_index;
    program->AddInstruction(LN_Event::OnFixedUpdate, print);
  }

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = dict_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group);

  LN_Instruction set_collision_group_from_make_dict;
  set_collision_group_from_make_dict.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_make_dict.int_expr_index = make_dict_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_make_dict);

  LN_Instruction set_collision_group_from_dict_set;
  set_collision_group_from_dict_set.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_dict_set.int_expr_index = dict_set_key_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_dict_set);

  LN_Instruction set_collision_group_from_dict_remove;
  set_collision_group_from_dict_remove.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_dict_remove.int_expr_index = dict_remove_key_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_dict_remove);

  LN_Instruction set_collision_group_from_dict_merge;
  set_collision_group_from_dict_merge.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_dict_merge.int_expr_index = dict_merge_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_dict_merge);

  LN_Instruction set_collision_group_from_dict_keys;
  set_collision_group_from_dict_keys.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_dict_keys.int_expr_index = dict_get_keys_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_dict_keys);

  LN_Instruction set_collision_group_from_list_length;
  set_collision_group_from_list_length.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_list_length.int_expr_index = list_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_list_length);

  LN_Instruction set_collision_group_from_make_list;
  set_collision_group_from_make_list.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_make_list.int_expr_index = make_list_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_make_list);

  LN_Instruction set_collision_group_from_items;
  set_collision_group_from_items.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_items.int_expr_index = list_from_items_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_items);

  LN_Instruction set_collision_group_from_duplicate;
  set_collision_group_from_duplicate.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_duplicate.int_expr_index = list_duplicate_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_duplicate);

  LN_Instruction set_collision_group_from_extend;
  set_collision_group_from_extend.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_extend.int_expr_index = list_extend_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_extend);

  LN_Instruction set_collision_group_from_append;
  set_collision_group_from_append.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_append.int_expr_index = list_append_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_append);

  LN_Instruction set_collision_group_from_remove_index;
  set_collision_group_from_remove_index.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_remove_index.int_expr_index = list_remove_index_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_remove_index);

  LN_Instruction set_collision_group_from_remove_value;
  set_collision_group_from_remove_value.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_remove_value.int_expr_index = list_remove_value_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_remove_value);

  LN_Instruction set_collision_group_from_set_index;
  set_collision_group_from_set_index.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group_from_set_index.int_expr_index = list_set_index_length_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group_from_set_index);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_NE(ir.value_expression_registers[select_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[rotation_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[string_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[vector4_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[resized_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[matrix_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[euler_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[rotation_constant_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[vector4_constant_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[matrix_constant_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[empty_dict_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[make_dict_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_get_key_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_set_key_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_remove_key_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_remove_key_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_merge_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[dict_get_keys_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[empty_list_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[make_list_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_from_items_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_duplicate_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_extend_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_append_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_remove_index_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_remove_value_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_set_index_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_element_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[list_random_item_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[value_switch_list_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.value_expression_registers[value_switch_compare_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[dict_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[make_dict_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[dict_set_key_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[dict_remove_key_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[dict_merge_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[dict_get_keys_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[make_list_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_from_items_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_duplicate_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_extend_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_append_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_remove_index_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_remove_value_length_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[list_set_index_length_index], LN_INVALID_INDEX);

  LN_TickContext reference_context;
  reference_context.tick_index = 1;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(64), 0, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context = reference_context;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(64), 0, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 46u);
  for (size_t index = 0; index < 31u; index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
    EXPECT_EQ(register_commands[index].type, LN_CommandBuffer::CommandType::Print);
  }
  const std::array<std::pair<LN_CommandBuffer::CommandType, int>, 15> expected_int_commands = {{
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 0},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 1},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 1},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 0},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 1},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 1},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 2},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 3},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 5},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 2},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 4},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 5},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 4},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 3},
      {LN_CommandBuffer::CommandType::SetCollisionGroup, 3},
  }};
  for (size_t offset = 0; offset < expected_int_commands.size(); offset++) {
    const size_t command_index = 31u + offset;
    ExpectCommandEqual(register_commands[command_index], reference_commands[command_index]);
    EXPECT_EQ(register_commands[command_index].type, expected_int_commands[offset].first);
    EXPECT_EQ(register_commands[command_index].int_value, expected_int_commands[offset].second);
  }
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, BranchRouteEmitsSelectedPulseToGuardedCommand)
{
  std::shared_ptr<LN_Program> program = CreateBranchRouteQuitProgram(true, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(62), 0, 0);

  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, BranchRouteSkipsUnselectedPulse)
{
  std::shared_ptr<LN_Program> program = CreateBranchRouteQuitProgram(false, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(63), 0, 0);

  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  EXPECT_TRUE(commands.empty());
}

TEST(LN_RuntimeTree, BranchRoutePulseIsScopedPerLoopIteration)
{
  std::shared_ptr<LN_Program> program = CreateLoopScopedBranchRouteQuitProgram();
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(64), 0, 0);

  LN_TickContext context;
  context.tick_index = 1;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, OuterInstructionExecutedPulseTriggersEveryLoopIteration)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Instruction route;
  route.opcode = LN_OpCode::BranchRoute;
  route.bool_expr_index = AddBoolConstant(*program, true);
  route.bool_value = true;
  const uint32_t route_index = program->AddInstruction(LN_Event::OnFixedUpdate, route);

  LN_BoolExpression route_pulse;
  route_pulse.kind = LN_BoolExpressionKind::InstructionExecuted;
  route_pulse.input0 = route_index;

  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = program->AddBoolExpression(route_pulse);
  frame.count_int_expr_index = AddIntConstant(*program, 3);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_BoolExpression loop_active;
  loop_active.kind = LN_BoolExpressionKind::LoopActive;
  loop_active.int_value = int32_t(frame_index);
  frame.loop_active_bool_expr_index = program->AddBoolExpression(loop_active);

  LN_IntExpression loop_index;
  loop_index.kind = LN_IntExpressionKind::LoopIndex;
  loop_index.int_value = int32_t(frame_index);
  frame.loop_index_int_expr_index = program->AddIntExpression(loop_index);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  AddGuardedPrint(*program, frame.loop_active_bool_expr_index, 7, frame_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(274), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 3u);
  for (const LN_CommandBuffer::Command &command : commands) {
    EXPECT_EQ(command.type, LN_CommandBuffer::CommandType::Print);
    EXPECT_EQ(command.property_name, "7");
  }
}

TEST(LN_RuntimeTree, DoOncePulseIsStableForEveryConsumerInExecutionScope)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t once_index = AddOncePulse(*program);
  AddGuardedPrint(*program, once_index, 1);
  AddGuardedPrint(*program, once_index, 2);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(264), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;

  const std::vector<LN_CommandBuffer::Command> first_commands = RecordReadyCommands(runtime_tree,
                                                                                    context);
  ASSERT_EQ(first_commands.size(), 2u);
  EXPECT_EQ(first_commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(first_commands[1].type, LN_CommandBuffer::CommandType::Print);

  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, DoOnceStateIsIndependentForEveryRuntimeTreeInstance)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t once_index = AddOncePulse(*program);
  AddGuardedPrint(*program, once_index, 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree first_tree(program, FakeGameObjectPointer(265), 0, 0);
  LN_RuntimeTree second_tree(program, FakeGameObjectPointer(266), 1, 0);
  LN_TickContext context;
  context.tick_index = 0;

  EXPECT_EQ(RecordReadyCommands(first_tree, context).size(), 1u);
  EXPECT_EQ(RecordReadyCommands(second_tree, context).size(), 1u);

  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(first_tree, context).empty());
  EXPECT_TRUE(RecordReadyCommands(second_tree, context).empty());
}

TEST(LN_RuntimeTree, DoOnceAttemptIsScopedPerLoopIteration)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 2);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_IntExpression loop_index;
  loop_index.kind = LN_IntExpressionKind::LoopIndex;
  loop_index.int_value = int32_t(frame_index);
  frame.loop_index_int_expr_index = program->AddIntExpression(loop_index);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  const uint32_t once_index = AddOncePulse(
      *program, AddBoolConstant(*program, true), LN_INVALID_INDEX, frame_index);
  AddGuardedPrint(*program, once_index, 1, frame_index);
  AddGuardedPrint(*program, once_index, 2, frame_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(265), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::Print);
}

TEST(LN_RuntimeTree, DoOnceRearmsOnlyAfterResetAndGivesResetPriority)
{
  PropertyDrivenOnceProgram refs = CreatePropertyDrivenOnceProgram();
  ASSERT_TRUE(refs.program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(refs.program, FakeGameObjectPointer(266), 0, 0);

  auto set_inputs = [&](const bool flow, const bool reset) {
    ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, refs.flow_ref_index, flow));
    ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, refs.reset_ref_index, reset));
  };

  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  set_inputs(true, false);
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  set_inputs(false, false);
  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  set_inputs(true, false);
  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  set_inputs(false, true);
  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  set_inputs(true, false);
  context.tick_index = 5;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  /* Reset is processed before Flow when both arrive in the same scope. */
  set_inputs(true, true);
  context.tick_index = 6;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  set_inputs(true, false);
  context.tick_index = 7;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, DoOnceClosedStateSurvivesDisableResume)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t once_index = AddOncePulse(*program);
  AddGuardedPrint(*program, once_index, 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(268), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  runtime_tree.SetEnabled(false);
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, BooleanEdgeEmitsEachTransitionOnceWithStableFanout)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t condition_ref = LN_INVALID_INDEX;
  const uint32_t condition = AddBoolTreePropertyExpression(
      *program, "condition", false, condition_ref);
  const BooleanEdgeExpressionRefs edge = AddBooleanEdgeExpressions(*program, condition);
  AddGuardedPrint(*program, edge.rising, 1);
  AddGuardedPrint(*program, edge.rising, 2);
  AddGuardedPrint(*program, edge.falling, 3);
  AddGuardedPrint(*program, edge.falling, 4);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(269), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, condition_ref, true));
  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> rising = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(rising.size(), 2u);
  EXPECT_EQ(rising[0].property_name, "1");
  EXPECT_EQ(rising[1].property_name, "2");

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, condition_ref, false));
  context.tick_index = 3;
  const std::vector<LN_CommandBuffer::Command> falling = RecordReadyCommands(runtime_tree,
                                                                             context);
  ASSERT_EQ(falling.size(), 2u);
  EXPECT_EQ(falling[0].property_name, "3");
  EXPECT_EQ(falling[1].property_name, "4");

  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, BooleanEdgeInitialTrueRisesAndIsSampledOncePerTick)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 2);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_IntExpression loop_index;
  loop_index.kind = LN_IntExpressionKind::LoopIndex;
  loop_index.int_value = int32_t(frame_index);
  frame.loop_index_int_expr_index = program->AddIntExpression(loop_index);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  const BooleanEdgeExpressionRefs edge = AddBooleanEdgeExpressions(
      *program, AddBoolConstant(*program, true));
  AddGuardedPrint(*program, edge.rising, 1, frame_index);
  AddGuardedPrint(*program, edge.falling, 2, frame_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(270), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "1");
}

TEST(LN_RuntimeTree, CooldownAcceptsBlocksAndCompletesWithoutRestarting)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddCooldownControl(*program, cooldown, flow, reset, 0.3f);
  AddGuardedPrint(*program, cooldown.accepted, 1);
  AddGuardedPrint(*program, cooldown.accepted, 2);
  AddGuardedPrint(*program, cooldown.blocked, 3);
  AddGuardedPrint(*program, cooldown.completed, 4);
  AddGuardedPrint(*program, cooldown.completed, 5);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(271), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 2u);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, true));
  context.tick_index = 2;
  const std::vector<LN_CommandBuffer::Command> blocked = RecordReadyCommands(runtime_tree,
                                                                             context);
  ASSERT_EQ(blocked.size(), 1u);
  EXPECT_EQ(blocked[0].property_name, "3");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 3;
  const std::vector<LN_CommandBuffer::Command> completed = RecordReadyCommands(runtime_tree,
                                                                               context);
  ASSERT_EQ(completed.size(), 2u);
  EXPECT_EQ(completed[0].property_name, "4");
  EXPECT_EQ(completed[1].property_name, "5");

  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, IdleCooldownStateOutputsAreReadableWithoutControlFlow)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddFloatPrint(*program, cooldown.remaining);
  AddFloatPrint(*program, cooldown.progress);
  AddGuardedPrint(*program, cooldown.ready, 1);
  AddGuardedPrint(*program, cooldown.accepted, 2);
  AddGuardedPrint(*program, cooldown.blocked, 3);
  AddGuardedPrint(*program, cooldown.completed, 4);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(278), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 1.0;

  for (uint64_t tick = 0; tick < 2; tick++) {
    context.tick_index = tick;
    const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                context);
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_FLOAT_EQ(std::stof(commands[0].property_name), 0.0f);
    EXPECT_FLOAT_EQ(std::stof(commands[1].property_name), 1.0f);
    EXPECT_EQ(commands[2].property_name, "1");
  }
}

TEST(LN_RuntimeTree, CooldownResetPrecedesAttemptAndClearsCompletion)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddCooldownControl(*program, cooldown, flow, reset, 0.1f);
  AddGuardedPrint(*program, cooldown.accepted, 1);
  AddGuardedPrint(*program, cooldown.blocked, 2);
  AddGuardedPrint(*program, cooldown.completed, 3);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(272), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  /* The old cooldown expires before this event; Reset clears that completion, then Flow starts a
   * fresh cooldown instead of being blocked. */
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> reset_and_attempt = RecordReadyCommands(
      runtime_tree, context);
  ASSERT_EQ(reset_and_attempt.size(), 1u);
  EXPECT_EQ(reset_and_attempt[0].property_name, "1");
}

TEST(LN_RuntimeTree, CooldownExpiryAndAttemptCanEmitCompletedAndAcceptedTogether)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddCooldownControl(*program, cooldown, AddBoolConstant(*program, true), LN_INVALID_INDEX, 0.1f);
  AddGuardedPrint(*program, cooldown.accepted, 1);
  AddGuardedPrint(*program, cooldown.blocked, 2);
  AddGuardedPrint(*program, cooldown.completed, 3);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(273), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "3");
}

TEST(LN_RuntimeTree, CooldownInvalidDurationsBypassWithoutCompleting)
{
  const std::array<float, 5> durations = {
      0.0f,
      -1.0f,
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
  };

  for (const float duration : durations) {
    std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
    const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
    AddCooldownControl(
        *program, cooldown, AddBoolConstant(*program, true), LN_INVALID_INDEX, duration);
    AddGuardedPrint(*program, cooldown.accepted, 1);
    AddGuardedPrint(*program, cooldown.blocked, 2);
    AddGuardedPrint(*program, cooldown.completed, 3);
    AddGuardedPrint(*program, cooldown.ready, 4);
    ASSERT_TRUE(program->ValidateInstructionPayloads());

    LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(274), 0, 0);
    LN_TickContext context;
    context.use_fixed_timestep = true;
    context.fixed_dt = 1.0;
    for (uint64_t tick = 0; tick < 2; tick++) {
      context.tick_index = tick;
      const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                  context);
      ASSERT_EQ(commands.size(), 2u) << duration << ", tick " << tick;
      EXPECT_EQ(commands[0].property_name, "1");
      EXPECT_EQ(commands[1].property_name, "4");
    }
  }
}

TEST(LN_RuntimeTree, CooldownTinyPositiveDurationWaitsForAPositiveStep)
{
  constexpr float tiny_duration = 5.0e-7f;

  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddCooldownControl(*program, cooldown, flow, LN_INVALID_INDEX, tiny_duration);
  AddFloatPrint(*program, cooldown.remaining);
  AddFloatPrint(*program, cooldown.progress);
  AddGuardedPrint(*program, cooldown.accepted, 1);
  AddGuardedPrint(*program, cooldown.ready, 2);
  AddGuardedPrint(*program, cooldown.completed, 3);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(277), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = double(tiny_duration) * 0.5;
  context.tick_index = 0;

  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_GT(std::stof(commands[0].property_name), 0.0f);
  EXPECT_FLOAT_EQ(std::stof(commands[1].property_name), 0.0f);
  EXPECT_EQ(commands[2].property_name, "1");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_FLOAT_EQ(std::stof(commands[0].property_name), 0.0f);
  EXPECT_FLOAT_EQ(std::stof(commands[1].property_name), 1.0f);
  EXPECT_EQ(commands[2].property_name, "2");
  EXPECT_EQ(commands[3].property_name, "3");
}

TEST(LN_RuntimeTree, CooldownValuesUseSampledUnscaledTimeAndPauseWhileDisabled)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);
  AddCooldownControl(*program, cooldown, flow, reset, 0.4f, true);
  AddFloatPrint(*program, cooldown.remaining);
  AddFloatPrint(*program, cooldown.progress);
  AddGuardedPrint(*program, cooldown.ready, 9);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(275), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.01;
  context.unscaled_dt = 0.1;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_NEAR(std::stof(commands[0].property_name), 0.4f, 0.0001f);
  EXPECT_NEAR(std::stof(commands[1].property_name), 0.0f, 0.0001f);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_NEAR(std::stof(commands[0].property_name), 0.3f, 0.0001f);
  EXPECT_NEAR(std::stof(commands[1].property_name), 0.25f, 0.0001f);

  runtime_tree.SetEnabled(false);
  context.tick_index = 2;
  context.unscaled_dt = 10.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 3;
  context.unscaled_dt = 0.1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_NEAR(std::stof(commands[0].property_name), 0.2f, 0.0001f);
  EXPECT_NEAR(std::stof(commands[1].property_name), 0.5f, 0.0001f);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 4;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_NEAR(std::stof(commands[0].property_name), 0.0f, 0.0001f);
  EXPECT_NEAR(std::stof(commands[1].property_name), 1.0f, 0.0001f);
  EXPECT_EQ(commands[2].property_name, "9");
}

TEST(LN_RuntimeTree, CooldownSamplesDurationAndClockOnlyWhenAccepted)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t duration_ref = LN_INVALID_INDEX;
  uint32_t ignore_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t duration = AddFloatTreePropertyExpression(
      *program, "duration", 0.3f, duration_ref);
  const uint32_t ignore = AddBoolTreePropertyExpression(
      *program, "ignore_timescale", false, ignore_ref);
  const CooldownExpressionRefs cooldown = AddCooldownExpressions(*program);

  LN_Instruction attempt;
  attempt.opcode = LN_OpCode::TryCooldown;
  attempt.bool_guard_expr_index = flow;
  attempt.float_expr_index = duration;
  attempt.secondary_bool_expr_index = ignore;
  attempt.int_value = int32_t(cooldown.state);
  program->AddInstruction(LN_Event::OnFixedUpdate, attempt);
  AddGuardedPrint(*program, cooldown.completed, 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(276), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.unscaled_dt = 1.0;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  ASSERT_TRUE(SetFloatTreeProperty(runtime_tree, duration_ref, 0.2f));
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, ignore_ref, true));
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 3;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  /* The next accepted attempt samples the new duration and unscaled clock policy. */
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, true));
  context.fixed_dt = 0.01;
  context.unscaled_dt = 0.0;
  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.unscaled_dt = 0.2;
  context.tick_index = 5;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, TimerArmedOnInitEmitsContinuationAfterFixedDelta)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(65), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> elapsed_commands = RecordReadyCommands(runtime_tree,
                                                                                      context);
  ASSERT_EQ(elapsed_commands.size(), 1u);
  EXPECT_EQ(elapsed_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, TimerCanIgnoreTimescaleStep)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(165), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.05;
  context.unscaled_dt = 0.2;

  context.tick_index = 0;
  const std::vector<LN_CommandBuffer::Command> elapsed_commands = RecordReadyCommands(runtime_tree,
                                                                                      context);
  ASSERT_EQ(elapsed_commands.size(), 1u);
  EXPECT_EQ(elapsed_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);

  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, ZeroDurationTimerEmitsContinuationOnArmTick)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.0f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(63), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, TweenValueFloatResultAndFactorAdvanceFromResultSocket)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(
      *program, AddOncePulse(*program), AddBoolConstant(*program, false), 0.2f);
  const uint32_t result_property = AddTweenTestPropertyRef(
      *program, "tween_result", LN_ValueType::Float);
  const uint32_t factor_property = AddTweenTestPropertyRef(
      *program, "tween_factor", LN_ValueType::Float);
  AddSetFloatPropertyInstruction(*program, result_property, tween.float_result_index);
  AddSetFloatPropertyInstruction(*program, factor_property, tween.factor_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(200), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  const LN_CommandBuffer::Command *result_command = FindPropertyCommand(commands, result_property);
  const LN_CommandBuffer::Command *factor_command = FindPropertyCommand(commands, factor_property);
  ASSERT_NE(result_command, nullptr);
  ASSERT_NE(factor_command, nullptr);
  EXPECT_NEAR(result_command->property_value.float_value, 15.0f, 0.0001f);
  EXPECT_NEAR(factor_command->property_value.float_value, 0.5f, 0.0001f);

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  result_command = FindPropertyCommand(commands, result_property);
  factor_command = FindPropertyCommand(commands, factor_property);
  ASSERT_NE(result_command, nullptr);
  ASSERT_NE(factor_command, nullptr);
  EXPECT_NEAR(result_command->property_value.float_value, 20.0f, 0.0001f);
  EXPECT_NEAR(factor_command->property_value.float_value, 1.0f, 0.0001f);
}

TEST(LN_RuntimeTree, TweenValueDoneAndReachedPulseAtCompletionIndependentOfCurveEndpoint)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(*program,
                                                        AddOncePulse(*program),
                                                        AddBoolConstant(*program, false),
                                                        0.2f,
                                                        false,
                                                        false,
                                                        0.5f);
  const uint32_t done_property = AddTweenTestPropertyRef(
      *program, "tween_done", LN_ValueType::Bool);
  const uint32_t reached_property = AddTweenTestPropertyRef(
      *program, "tween_reached", LN_ValueType::Bool);
  AddSetBoolPropertyInstruction(*program, done_property, true, tween.tween_index);
  AddSetBoolPropertyInstruction(*program, reached_property, true, tween.reached_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(201), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  EXPECT_NE(FindPropertyCommand(commands, done_property), nullptr);
  EXPECT_EQ(FindPropertyCommand(commands, reached_property), nullptr);

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  EXPECT_NE(FindPropertyCommand(commands, done_property), nullptr);
  EXPECT_NE(FindPropertyCommand(commands, reached_property), nullptr);

  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  EXPECT_EQ(FindPropertyCommand(commands, done_property), nullptr);
  EXPECT_EQ(FindPropertyCommand(commands, reached_property), nullptr);
}

TEST(LN_RuntimeTree, TweenValueVectorResultReversesFromBackPulse)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(
      *program, AddOncePulse(*program), AddDelayedPulse(*program, 0.15f), 0.2f);
  const uint32_t vector_property = AddTweenTestPropertyRef(
      *program, "tween_vector", LN_ValueType::Vector);
  AddSetVectorPropertyInstruction(*program, vector_property, tween.vector_result_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(202), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  auto expect_vector = [&](const uint64_t tick, const double time, const MT_Vector3 &expected) {
    context.tick_index = tick;
    context.current_time = time;
    const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                context);
    const LN_CommandBuffer::Command *command = FindPropertyCommand(commands, vector_property);
    ASSERT_NE(command, nullptr);
    EXPECT_NEAR(command->property_value.vector_value.x(), expected.x(), 0.0001f);
    EXPECT_NEAR(command->property_value.vector_value.y(), expected.y(), 0.0001f);
    EXPECT_NEAR(command->property_value.vector_value.z(), expected.z(), 0.0001f);
  };

  expect_vector(0, 0.0, MT_Vector3(5.0f, 10.0f, 15.0f));
  expect_vector(1, 0.1, MT_Vector3(10.0f, 20.0f, 30.0f));
  expect_vector(2, 0.2, MT_Vector3(5.0f, 10.0f, 15.0f));
  expect_vector(3, 0.3, MT_Vector3(0.0f, 0.0f, 0.0f));
}

TEST(LN_RuntimeTree, TweenValueOnDemandInstantResetRestartsAfterSkippedTick)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(
      *program, LN_INVALID_INDEX, LN_INVALID_INDEX, 0.2f, true, true);
  const uint32_t result_property = AddTweenTestPropertyRef(
      *program, "tween_on_demand", LN_ValueType::Float);
  AddSetFloatPropertyInstruction(*program, result_property, tween.float_result_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(203), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  const LN_CommandBuffer::Command *command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 15.0f, 0.0001f);

  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 15.0f, 0.0001f);
}

TEST(LN_RuntimeTree, TweenValueZeroDurationReachesOnceWhileForwardIsHeld)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(
      *program, AddBoolConstant(*program, true), AddBoolConstant(*program, false), 0.0f);
  const uint32_t result_property = AddTweenTestPropertyRef(
      *program, "tween_zero_result", LN_ValueType::Float);
  const uint32_t reached_property = AddTweenTestPropertyRef(
      *program, "tween_zero_reached", LN_ValueType::Bool);
  AddSetFloatPropertyInstruction(*program, result_property, tween.float_result_index);
  AddSetBoolPropertyInstruction(*program, reached_property, true, tween.reached_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(204), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  const LN_CommandBuffer::Command *result_command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(result_command, nullptr);
  EXPECT_NEAR(result_command->property_value.float_value, 20.0f, 0.0001f);
  EXPECT_NE(FindPropertyCommand(commands, reached_property), nullptr);

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  result_command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(result_command, nullptr);
  EXPECT_NEAR(result_command->property_value.float_value, 20.0f, 0.0001f);
  EXPECT_EQ(FindPropertyCommand(commands, reached_property), nullptr);
}

TEST(LN_RuntimeTree, TweenValueSanitizesNonFiniteDurationAndCurveFactor)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(*program,
                                                        AddBoolConstant(*program, true),
                                                        AddBoolConstant(*program, false),
                                                        std::numeric_limits<float>::infinity(),
                                                        false,
                                                        false,
                                                        std::numeric_limits<float>::infinity());
  const uint32_t result_property = AddTweenTestPropertyRef(
      *program, "tween_non_finite_result", LN_ValueType::Float);
  const uint32_t factor_property = AddTweenTestPropertyRef(
      *program, "tween_non_finite_factor", LN_ValueType::Float);
  AddSetFloatPropertyInstruction(*program, result_property, tween.float_result_index);
  AddSetFloatPropertyInstruction(*program, factor_property, tween.factor_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(205), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  const LN_CommandBuffer::Command *result_command = FindPropertyCommand(commands, result_property);
  const LN_CommandBuffer::Command *factor_command = FindPropertyCommand(commands, factor_property);
  ASSERT_NE(result_command, nullptr);
  ASSERT_NE(factor_command, nullptr);
  EXPECT_TRUE(std::isfinite(result_command->property_value.float_value));
  EXPECT_TRUE(std::isfinite(factor_command->property_value.float_value));
  EXPECT_NEAR(result_command->property_value.float_value, 20.0f, 0.0001f);
  EXPECT_NEAR(factor_command->property_value.float_value, 1.0f, 0.0001f);
}

TEST(LN_RuntimeTree, RetriggerBeforeElapsedResetsTimer)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnFixedUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(64), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  for (uint64_t tick = 0; tick < 6; tick++) {
    context.tick_index = tick;
    EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty()) << tick;
  }
}

TEST(LN_RuntimeTree, DisabledTimerPausesUntilEnabled)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(65), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(false);
  context.tick_index = 1;
  context.fixed_dt = 1.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 2;
  context.fixed_dt = 0.1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, MultipleTimersElapsedSameTickRecordInInstructionOrder)
{
  std::shared_ptr<LN_Program> program = CreateTwoTimerSameTickProgram();
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(66), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::RestartGame);
}

TEST(LN_RuntimeTree, IncompatibleProgramSwapCancelsActiveTimer)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(67), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetProgram(LN_Program::CreateDebugSetWorldPosition(MT_Vector3(4.0f, 5.0f, 6.0f)));

  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
}

TEST(LN_RuntimeTree, DelayArmedOnInitEmitsDoneAfterFixedDelta)
{
  std::shared_ptr<LN_Program> program = CreateDelayQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(68), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> done_commands = RecordReadyCommands(runtime_tree,
                                                                                   context);
  ASSERT_EQ(done_commands.size(), 1u);
  EXPECT_EQ(done_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, DelayCanIgnoreTimescaleStep)
{
  std::shared_ptr<LN_Program> program = CreateDelayQuitProgram(0.2f, LN_Event::OnInit, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(168), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.05;
  context.unscaled_dt = 0.2;

  context.tick_index = 0;
  const std::vector<LN_CommandBuffer::Command> done_commands = RecordReadyCommands(runtime_tree,
                                                                                   context);
  ASSERT_EQ(done_commands.size(), 1u);
  EXPECT_EQ(done_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);

  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, DelayQueuesRetriggersWhileActive)
{
  std::shared_ptr<LN_Program> program = CreateDelayQuitProgram(0.2f, LN_Event::OnFixedUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(69), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 2;
  const std::vector<LN_CommandBuffer::Command> done_commands = RecordReadyCommands(runtime_tree,
                                                                                   context);
  ASSERT_EQ(done_commands.size(), 1u);
  EXPECT_EQ(done_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
  context.tick_index = 3;
  const std::vector<LN_CommandBuffer::Command> queued_commands = RecordReadyCommands(runtime_tree,
                                                                                     context);
  ASSERT_EQ(queued_commands.size(), 1u);
  EXPECT_EQ(queued_commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, PulsifyEmitsFirstPulseThenFixedIntervalPulses)
{
  std::shared_ptr<LN_Program> program = CreatePulsifyQuitProgram(0.2f, LN_Event::OnFixedUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(70), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, PulsifyCanIgnoreTimescaleStep)
{
  std::shared_ptr<LN_Program> program = CreatePulsifyQuitProgram(
      0.2f, LN_Event::OnFixedUpdate, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(170), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.05;
  context.unscaled_dt = 0.2;

  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, PulsifyStopsWhenFlowIsAbsent)
{
  std::shared_ptr<LN_Program> program = CreatePulsifyQuitProgram(0.1f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(71), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, BarrierEmitsAfterConditionStaysTrueForDuration)
{
  std::shared_ptr<LN_Program> program = CreateBarrierQuitProgram(0.2f, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(72), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 3;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, BarrierCanIgnoreTimescaleStep)
{
  std::shared_ptr<LN_Program> program = CreateBarrierQuitProgram(0.2f, true, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(172), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.05;
  context.unscaled_dt = 0.2;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, BarrierDoesNotEmitWhenConditionIsFalse)
{
  std::shared_ptr<LN_Program> program = CreateBarrierQuitProgram(0.0f, false);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(73), 0, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.fixed_dt = 0.1;

  for (uint64_t tick = 0; tick < 3; tick++) {
    context.tick_index = tick;
    EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty()) << tick;
  }
}

TEST(LN_RuntimeTree, DisabledTreeDoesNotRecordCommands)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(2), 1, 0);
  runtime_tree.SetEnabled(false);
  LN_CommandBuffer command_buffer;
  LN_TickContext context;
  context.use_fixed_timestep = true;

  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  EXPECT_EQ(command_buffer.Size(), 0);
}

TEST(LN_RuntimeTree, InactiveRuntimeTreeDoesNotRecordCommands)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject *fake_game_object = FakeGameObjectPointer(7);
  LN_RuntimeTree runtime_tree(program, fake_game_object, 1, 0);
  runtime_tree.SetRuntimeActive(false);
  LN_CommandBuffer command_buffer;
  LN_TickContext context;
  context.use_fixed_timestep = true;

  EXPECT_FALSE(runtime_tree.CanTick());

  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  EXPECT_EQ(command_buffer.Size(), 0);
}

TEST(LN_RuntimeTree, DetachedTreeDoesNotRecordCommands)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject *fake_game_object = FakeGameObjectPointer(5);
  LN_RuntimeTree runtime_tree(program, fake_game_object, 1, 0);
  EXPECT_TRUE(runtime_tree.OwnsGameObject(fake_game_object));

  runtime_tree.DetachGameObject();
  EXPECT_FALSE(runtime_tree.IsAttached());
  EXPECT_FALSE(runtime_tree.OwnsGameObject(fake_game_object));
  EXPECT_FALSE(runtime_tree.CanTick());

  LN_CommandBuffer command_buffer;
  LN_TickContext context;
  context.use_fixed_timestep = true;

  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  EXPECT_EQ(command_buffer.Size(), 0);
}

TEST(LN_RuntimeTree, DisabledCanTickDoesNotTouchGameObject)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(6), 1, 0);
  runtime_tree.SetEnabled(false);

  EXPECT_FALSE(runtime_tree.CanTick());
}

TEST(LN_RuntimeTree, DisabledSnapshotTreeSkipsNormalCaptureButAllowsForcedRunCapture)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterVectorSnapshotCommandProgram();
  KX_GameObject test_game_object;
  LN_RuntimeTree runtime_tree(program, &test_game_object, 1, 0);

  ASSERT_TRUE(runtime_tree.ShouldCaptureSnapshot());
  ASSERT_TRUE(runtime_tree.ShouldCaptureSnapshotForForcedUpdate());

  runtime_tree.SetEnabled(false);

  EXPECT_FALSE(runtime_tree.ShouldCaptureSnapshot());
  EXPECT_TRUE(runtime_tree.ShouldCaptureSnapshotForForcedUpdate());

  runtime_tree.SetRuntimeActive(false);

  EXPECT_FALSE(runtime_tree.ShouldCaptureSnapshot());
  EXPECT_FALSE(runtime_tree.ShouldCaptureSnapshotForForcedUpdate());
}

TEST(LN_RuntimeTree, InputOnlyProgramCapturesSharedInputSnapshot)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetDependencySummary().input_channels & LN_DEP_INPUT_MOUSE,
            uint32_t(LN_DEP_INPUT_MOUSE));
  ASSERT_EQ(program->GetDependencySummary().snapshot_channels, uint32_t(LN_DEP_SNAPSHOT_NONE));

  KX_GameObject test_game_object;
  LN_RuntimeTree runtime_tree(program, &test_game_object, 1, 0);

  ASSERT_TRUE(runtime_tree.ShouldCaptureSnapshot());
  ASSERT_TRUE(runtime_tree.ShouldCaptureSnapshotForForcedUpdate());

  LN_InputSnapshot input_snapshot;
  input_snapshot.Capture(nullptr, nullptr);

  LN_TickReadContext read_context;
  read_context.input = &input_snapshot;
  read_context.input_channels = LN_DEP_INPUT_MOUSE;
  read_context.tick_index = 7;
  read_context.has_input = true;

  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  EXPECT_TRUE(runtime_tree.GetSnapshot().GetCaptureStats().used_shared_input);
}

TEST(LN_RuntimeTree, SortKeyOrdersSceneObjectBeforeAppliedTree)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(0.0f, 0.0f, 1.0f));
  LN_RuntimeTree later_scene_tree(program, FakeGameObjectPointer(3), 2, 0);
  LN_RuntimeTree earlier_scene_tree(program, FakeGameObjectPointer(4), 1, 9);
  LN_CommandBuffer command_buffer;
  LN_TickContext context;
  context.use_fixed_timestep = true;

  command_buffer.BeginRecording();
  later_scene_tree.ExecuteReady(command_buffer, context);
  earlier_scene_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  std::vector<LN_CommandBuffer::Command> commands = command_buffer.GetCommandsForTests();
  LN_CommandBuffer::SortCommands(commands);

  ASSERT_EQ(commands.size(), 2);
  EXPECT_EQ(commands[0].object, FakeGameObjectPointer(4));
  EXPECT_EQ(commands[1].object, FakeGameObjectPointer(3));
}

TEST(LN_RuntimeTree, ParallelExecutorRecordsWorkerTreesIntoLocalCommandLists)
{
  std::shared_ptr<const LN_Program> first_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  std::shared_ptr<const LN_Program> second_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(2.0f, 0.0f, 0.0f));
  std::shared_ptr<const LN_Program> empty_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(3.0f, 0.0f, 0.0f));

  LN_RuntimeTree later_tree(first_program, FakeGameObjectPointer(20), 2, 0);
  LN_RuntimeTree earlier_tree(second_program, FakeGameObjectPointer(10), 1, 0);
  LN_RuntimeTree empty_tree(empty_program, FakeGameObjectPointer(30), 3, 0);
  empty_tree.SetEnabled(false);

  LN_TickContext context;
  context.use_fixed_timestep = true;

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({5, &later_tree});
  work_items.push_back({3, &earlier_tree});
  work_items.push_back({4, &empty_tree});

  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists =
      executor.ExecuteTreesToCommandLists(work_items, context);

  ASSERT_EQ(command_lists.size(), 3u);
  EXPECT_EQ(command_lists[0].runtime_tree_index, 3u);
  EXPECT_EQ(command_lists[1].runtime_tree_index, 4u);
  EXPECT_EQ(command_lists[2].runtime_tree_index, 5u);
  ASSERT_EQ(command_lists[0].commands.size(), 1u);
  EXPECT_TRUE(command_lists[1].commands.empty());
  ASSERT_EQ(command_lists[2].commands.size(), 1u);
  EXPECT_EQ(command_lists[0].commands.front().object, FakeGameObjectPointer(10));
  EXPECT_EQ(command_lists[2].commands.front().object, FakeGameObjectPointer(20));
}

TEST(LN_RuntimeTree, ParallelExecutorKeepsEmptySlotsForNullWorkItems)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(40), 4, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({7, nullptr});
  work_items.push_back({4, &runtime_tree});

  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists =
      executor.ExecuteTreesToCommandLists(work_items, context);

  ASSERT_EQ(command_lists.size(), 2u);
  EXPECT_EQ(command_lists[0].runtime_tree_index, 4u);
  EXPECT_EQ(command_lists[1].runtime_tree_index, 7u);
  ASSERT_EQ(command_lists[0].commands.size(), 1u);
  EXPECT_TRUE(command_lists[1].commands.empty());
  EXPECT_EQ(command_lists[0].commands.front().object, FakeGameObjectPointer(40));
}

TEST(LN_RuntimeTree, ParallelExecutorReportsRuntimeProfileCounters)
{
  std::shared_ptr<const LN_Program> program = CreateProfileCounterProgram();

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(40), 4, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({4, &runtime_tree});

  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists =
      executor.ExecuteTreesToCommandLists(work_items, context, true);

  ASSERT_EQ(command_lists.size(), 1u);
  EXPECT_EQ(command_lists[0].profile_counters.instruction_dispatch_count, 1u);
  EXPECT_EQ(command_lists[0].profile_counters.expression_evaluation_count, 2u);
  EXPECT_EQ(command_lists[0].profile_counters.fallback_path_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsSupportedVectorCommandDirectly)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(4.0f, 5.0f, 6.0f));

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(45), 4, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;

  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(commands[0].object, FakeGameObjectPointer(45));
  EXPECT_EQ(counters.instruction_dispatch_count, 1u);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
  EXPECT_FALSE(runtime_tree.GetRuntimeFallbackReport().optimized_execution_partial);
  EXPECT_EQ(runtime_tree.GetRuntimeFallbackReport().fallback_instruction_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRExecutesLoopBlocksDirectly)
{
  std::shared_ptr<LN_Program> program = CreateLoopScopedBranchRouteQuitProgram();
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(64), 4, 0);
  LN_TickContext context;
  context.tick_index = 23;

  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 4u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
  const LN_RuntimeTree::RuntimeFallbackReport &report = runtime_tree.GetRuntimeFallbackReport();
  EXPECT_FALSE(report.optimized_execution_partial);
  EXPECT_EQ(report.tick_index, 23u);
  EXPECT_EQ(report.fallback_block_count, 0u);
  EXPECT_EQ(report.fallback_instruction_count, 0u);
  EXPECT_TRUE(report.hits.empty());
}

TEST(LN_RuntimeTree, ExecBlockIRDoesNotBuildFallbackDiagnosticsWithoutProfiling)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_SourceRef source_ref;
  source_ref.node_idname = "LNTestDrawLine";
  source_ref.node_name = "Draw Line";
  const uint32_t source_ref_index = program->AddSourceRef(source_ref);

  LN_Instruction draw_line;
  draw_line.opcode = LN_OpCode::DrawLine;
  draw_line.source_ref_index = source_ref_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, draw_line);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(64), 4, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 24;

  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();

  const LN_RuntimeTree::RuntimeFallbackReport &report = runtime_tree.GetRuntimeFallbackReport();
  EXPECT_FALSE(report.optimized_execution_partial);
  EXPECT_EQ(report.tick_index, 24u);
  EXPECT_EQ(report.fallback_block_count, 0u);
  EXPECT_EQ(report.fallback_instruction_count, 0u);
  EXPECT_TRUE(report.hits.empty());
}

TEST(LN_RuntimeTree, ExecBlockIRExecutesTimeFlowControlDirectly)
{
  const LN_OpCode time_flow_opcodes[] = {
      LN_OpCode::ArmTimer,
      LN_OpCode::ArmDelay,
      LN_OpCode::UpdatePulsify,
      LN_OpCode::UpdateBarrier,
  };

  for (const LN_OpCode opcode : time_flow_opcodes) {
    std::shared_ptr<const LN_Program> program = CreateImmediateTimeFlowQuitProgram(opcode);
    ASSERT_TRUE(program->ValidateInstructionPayloads());

    LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(46), 4, 0);
    LN_TickContext context;
    context.tick_index = 1;
    context.use_fixed_timestep = true;

    LN_CommandBuffer command_buffer;
    LN_RuntimeProfileCounters counters;
    command_buffer.BeginRecording();
    runtime_tree.ExecuteReady(command_buffer, context, &counters);
    command_buffer.EndRecording();
    const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
    EXPECT_EQ(counters.exec_block_count, 1u);
    EXPECT_EQ(counters.exec_direct_instruction_count, 2u);
    EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
    EXPECT_EQ(counters.exec_fallback_block_count, 0u);
  }
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsDebugSetWorldPositionDeterministically)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(7.0f, 8.0f, 9.0f));

  LN_TickContext context;
  context.use_fixed_timestep = true;
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(46), 4, 0);
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(commands[0].object, FakeGameObjectPointer(46));
  EXPECT_EQ(commands[0].vector_value.x(), 7.0f);
  EXPECT_EQ(commands[0].vector_value.y(), 8.0f);
  EXPECT_EQ(commands[0].vector_value.z(), 9.0f);
  EXPECT_EQ(commands[0].source_ref_index, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsObjectReferenceCommandsDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(80);
  KX_GameObject *source = FakeGameObjectPointer(81);
  KX_GameObject *target = FakeGameObjectPointer(82);
  KX_GameObject *mesh_object = FakeGameObjectPointer(83);
  KX_GameObject *follow_target = FakeGameObjectPointer(84);

  LN_RuntimeTree reference_tree(program, owner, 4, 0);
  LN_RuntimeTree ir_tree(program, owner, 4, 0);
  const LN_RuntimeRef source_ref = reference_tree.MakeObjectRef(source, "source");
  const LN_RuntimeRef target_ref = reference_tree.MakeObjectRef(target, "target");
  const LN_RuntimeRef mesh_ref = reference_tree.MakeObjectRef(mesh_object, "mesh");
  const LN_RuntimeRef follow_ref = reference_tree.MakeObjectRef(follow_target, "follow");
  EXPECT_EQ(ir_tree.MakeObjectRef(source, "source").slot, source_ref.slot);
  EXPECT_EQ(ir_tree.MakeObjectRef(target, "target").slot, target_ref.slot);
  EXPECT_EQ(ir_tree.MakeObjectRef(mesh_object, "mesh").slot, mesh_ref.slot);
  EXPECT_EQ(ir_tree.MakeObjectRef(follow_target, "follow").slot, follow_ref.slot);

  const uint32_t source_expr = AddObjectRefConstant(*program, source_ref);
  const uint32_t target_expr = AddObjectRefConstant(*program, target_ref);
  const uint32_t mesh_expr = AddObjectRefConstant(*program, mesh_ref);
  const uint32_t follow_expr = AddObjectRefConstant(*program, follow_ref);
  const uint32_t factor_expr = AddFloatConstant(*program, 0.25f);
  const uint32_t property_expr = AddStringConstant(*program, "health");

  LN_Instruction slow_follow;
  slow_follow.opcode = LN_OpCode::SlowFollow;
  slow_follow.secondary_value_expr_index = follow_expr;
  slow_follow.float_expr_index = factor_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, slow_follow);

  LN_Instruction replace_mesh;
  replace_mesh.opcode = LN_OpCode::ReplaceMesh;
  replace_mesh.secondary_value_expr_index = mesh_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, replace_mesh);

  LN_Instruction copy_property;
  copy_property.opcode = LN_OpCode::CopyProperty;
  copy_property.value_expr_index = source_expr;
  copy_property.secondary_value_expr_index = target_expr;
  copy_property.string_expr_index = property_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, copy_property);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), reference_commands.size());
  ASSERT_EQ(ir_commands.size(), 3u);
  for (size_t index = 0; index < ir_commands.size(); index++) {
    ExpectCommandEqual(ir_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(ir_commands[0].type, LN_CommandBuffer::CommandType::SlowFollow);
  EXPECT_EQ(ir_commands[0].object, owner);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(ir_commands[0].runtime_tree), follow_target);
  EXPECT_NEAR(ir_commands[0].scalar_value, 0.25f, 0.0001f);
  EXPECT_EQ(ir_commands[1].type, LN_CommandBuffer::CommandType::ReplaceMesh);
  EXPECT_EQ(ir_commands[1].object, owner);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(ir_commands[1].runtime_tree), mesh_object);
  EXPECT_EQ(ir_commands[2].type, LN_CommandBuffer::CommandType::CopyProperty);
  EXPECT_EQ(ir_commands[2].object, source);
  EXPECT_EQ(reinterpret_cast<KX_GameObject *>(ir_commands[2].runtime_tree), target);
  EXPECT_EQ(ir_commands[2].property_name, "health");
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 3u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRClearsAddObjectResultWhenTargetIsMissing)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(85);
  KX_GameObject *source = FakeGameObjectPointer(86);

  LN_TreePropertyRef added_object_ref;
  added_object_ref.name = "__added_object";
  added_object_ref.value_type = LN_ValueType::ObjectRef;
  added_object_ref.default_value.type = LN_ValueType::ObjectRef;
  added_object_ref.default_value.exists = true;
  added_object_ref.default_value.reference_name = "stale";
  const uint32_t added_object_ref_index = program->AddTreePropertyRef(added_object_ref);

  LN_RuntimeTree reference_tree(program, owner, 4, 0);
  LN_RuntimeTree ir_tree(program, owner, 4, 0);
  const LN_RuntimeRef source_ref = reference_tree.MakeObjectRef(source, "source");
  EXPECT_EQ(ir_tree.MakeObjectRef(source, "source").slot, source_ref.slot);

  const uint32_t source_expr = AddObjectRefConstant(*program, source_ref);
  const uint32_t missing_target_expr = AddNoneValueConstant(*program);
  const uint32_t life_expr = AddFloatConstant(*program, 0.0f);
  const uint32_t full_copy_expr = AddBoolConstant(*program, false);

  LN_Instruction add_object;
  add_object.opcode = LN_OpCode::AddObject;
  add_object.value_expr_index = missing_target_expr;
  add_object.secondary_value_expr_index = source_expr;
  add_object.float_expr_index = life_expr;
  add_object.bool_expr_index = full_copy_expr;
  add_object.property_ref_index = added_object_ref_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, add_object);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  EXPECT_TRUE(RecordReadyCommands(reference_tree, reference_context).empty());

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  EXPECT_TRUE(command_buffer.TakeRecordedCommands().empty());

  const LN_Value *reference_result = reference_tree.GetTreePropertyValue(added_object_ref_index);
  const LN_Value *ir_result = ir_tree.GetTreePropertyValue(added_object_ref_index);
  ASSERT_NE(reference_result, nullptr);
  ASSERT_NE(ir_result, nullptr);
  EXPECT_EQ(reference_result->type, LN_ValueType::ObjectRef);
  EXPECT_EQ(ir_result->type, LN_ValueType::ObjectRef);
  EXPECT_FALSE(reference_result->exists);
  EXPECT_FALSE(ir_result->exists);
  EXPECT_TRUE(ir_result->reference_name.empty());
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsSetParentFromRuntimeRefDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(86);
  KX_GameObject *parent_object = FakeGameObjectPointer(87);

  LN_RuntimeTree ir_tree(program, owner, 4, 0);
  const LN_RuntimeRef parent_ref = ir_tree.MakeObjectRef(parent_object, "parent");
  const uint32_t parent_expr = AddObjectRefConstant(*program, parent_ref);
  const uint32_t compound_expr = AddBoolConstant(*program, false);
  const uint32_t ghost_expr = AddBoolConstant(*program, true);

  LN_Instruction set_parent;
  set_parent.opcode = LN_OpCode::SetParent;
  set_parent.secondary_value_expr_index = parent_expr;
  set_parent.bool_expr_index = compound_expr;
  set_parent.secondary_bool_expr_index = ghost_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_parent);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), 1u);
  EXPECT_EQ(ir_commands[0].type, LN_CommandBuffer::CommandType::SetParentFromRef);
  EXPECT_EQ(ir_commands[0].object, owner);
  EXPECT_EQ(ir_commands[0].runtime_tree, &ir_tree);
  EXPECT_EQ(ir_commands[0].runtime_ref.kind, LN_RuntimeRefKind::Object);
  EXPECT_EQ(ir_commands[0].runtime_ref.slot, parent_ref.slot);
  EXPECT_EQ(ir_tree.ResolveObjectRef(ir_commands[0].runtime_ref), parent_object);
  EXPECT_FALSE(ir_commands[0].bool_value);
  EXPECT_TRUE(ir_commands[0].secondary_bool_value);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRExecutesInputMotionNoOpDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(88);

  LN_RuntimeTree reference_tree(program, owner, 4, 0);
  LN_RuntimeTree ir_tree(program, owner, 4, 0);
  const LN_RuntimeRef owner_ref = reference_tree.MakeObjectRef(owner, "owner");
  EXPECT_EQ(ir_tree.MakeObjectRef(owner, "owner").slot, owner_ref.slot);
  const uint32_t owner_expr = AddObjectRefConstant(*program, owner_ref);
  const uint32_t zero_stick_expr = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));

  LN_Instruction gamepad_look;
  gamepad_look.opcode = LN_OpCode::GamepadLook;
  gamepad_look.property_ref_index = owner_expr;
  gamepad_look.vector_expr_index = zero_stick_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, gamepad_look);

  LN_Instruction mouse_look;
  mouse_look.opcode = LN_OpCode::MouseLook;
  mouse_look.property_ref_index = owner_expr;
  mouse_look.bool_value = false;
  program->AddInstruction(LN_Event::OnFixedUpdate, mouse_look);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  EXPECT_TRUE(reference_commands.empty());
  EXPECT_TRUE(ir_commands.empty());
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 2u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, MouseLookRecordsRotationFromCapturedMouseInput)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(false);
  std::vector<std::string> validation_errors;
  ASSERT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << testing::PrintToString(validation_errors);

  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  LN_TickReadContext read_context;
  read_context.input = &input_snapshot;
  read_context.input_channels = LN_DEP_INPUT_MOUSE;
  read_context.has_input = true;

  KX_GameObject reference_owner;
  LN_RuntimeTree reference_tree(program, &reference_owner, 4, 0);
  CaptureMousePosition(input_device, input_snapshot, 0, 0);
  read_context.tick_index = 10;
  reference_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.tick_index = read_context.tick_index;
  EXPECT_TRUE(RecordReadyCommands(reference_tree, reference_context).empty());

  CaptureMousePosition(input_device, input_snapshot, 100, 0);
  ASSERT_TRUE(input_snapshot.GetMouse().has_position);
  ASSERT_EQ(input_snapshot.GetMouse().delta_x, 100);
  ASSERT_EQ(input_snapshot.GetMouse().delta_y, 0);

  read_context.tick_index = 11;
  reference_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
  reference_context.tick_index = read_context.tick_index;
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  ASSERT_EQ(reference_commands.size(), 1u);
  EXPECT_EQ(reference_commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(reference_commands[0].object, &reference_owner);
  EXPECT_NEAR(reference_commands[0].vector_value.x(), 0.0f, 0.00001f);
  EXPECT_NEAR(reference_commands[0].vector_value.y(), 0.0f, 0.00001f);
  EXPECT_NEAR(reference_commands[0].vector_value.z(), -0.2f, 0.00001f);
  EXPECT_FALSE(reference_commands[0].bool_value);

  KX_GameObject ir_owner;
  LN_RuntimeTree ir_tree(program, &ir_owner, 4, 0);
  CaptureMousePosition(input_device, input_snapshot, 0, 0);
  read_context.tick_index = 20;
  ir_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  ir_context.tick_index = read_context.tick_index;
  EXPECT_TRUE(RecordReadyCommands(ir_tree, ir_context).empty());

  CaptureMousePosition(input_device, input_snapshot, 100, 0);
  read_context.tick_index = 21;
  ir_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
  ir_context.tick_index = read_context.tick_index;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), reference_commands.size());
  EXPECT_EQ(ir_commands[0].type, reference_commands[0].type);
  EXPECT_EQ(ir_commands[0].object, &ir_owner);
  EXPECT_NEAR(ir_commands[0].vector_value.x(), reference_commands[0].vector_value.x(), 0.00001f);
  EXPECT_NEAR(ir_commands[0].vector_value.y(), reference_commands[0].vector_value.y(), 0.00001f);
  EXPECT_NEAR(ir_commands[0].vector_value.z(), reference_commands[0].vector_value.z(), 0.00001f);
  EXPECT_EQ(ir_commands[0].bool_value, reference_commands[0].bool_value);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, MouseLookUsesEqualPixelScaleOnWidescreenCanvas)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  DEV_InputDevice input_device;
  TestCanvas canvas(1920, 1080);
  LN_InputSnapshot input_snapshot;
  LN_TickReadContext read_context;
  read_context.input = &input_snapshot;
  read_context.input_channels = LN_DEP_INPUT_MOUSE;
  read_context.has_input = true;

  KX_GameObject owner;
  LN_RuntimeTree runtime_tree(program, &owner, 4, 0);

  CaptureMousePosition(input_device, input_snapshot, 960, 540, &canvas);
  read_context.tick_index = 22;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = read_context.tick_index;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  /* Equal raw pixel offsets must produce equal rotation on a non-square canvas. */
  input_device.ClearInputs();
  input_device.ConvertMoveEvent(1060, 640);
  LN_InputSnapshot offset_snapshot;
  CaptureCurrentMousePositionWithoutFrameDelta(input_device, offset_snapshot, &canvas);
  ASSERT_TRUE(offset_snapshot.GetMouse().has_position);
  ASSERT_EQ(offset_snapshot.GetMouse().x, 1060);
  ASSERT_EQ(offset_snapshot.GetMouse().y, 640);
  ASSERT_EQ(offset_snapshot.GetMouse().delta_x, 0);
  ASSERT_EQ(offset_snapshot.GetMouse().delta_y, 0);

  read_context.input = &offset_snapshot;
  read_context.tick_index = 23;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
  context.tick_index = read_context.tick_index;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(commands[0].object, &owner);
  EXPECT_NEAR(commands[0].vector_value.x(), 0.0f, 0.00001f);
  EXPECT_NEAR(commands[0].vector_value.y(), 0.0f, 0.00001f);
  EXPECT_NEAR(commands[0].vector_value.z(), -0.2f, 0.00001f);
  EXPECT_FALSE(commands[0].bool_value);

  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(commands[1].object, &owner);
  EXPECT_NEAR(commands[1].vector_value.x(), -0.2f, 0.00001f);
  EXPECT_NEAR(commands[1].vector_value.y(), 0.0f, 0.00001f);
  EXPECT_NEAR(commands[1].vector_value.z(), 0.0f, 0.00001f);
  EXPECT_TRUE(commands[1].bool_value);
}

TEST(LN_RuntimeTree, MouseLookUsesNodePreviousPositionWhenSnapshotDeltaIsZero)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(false);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  auto run_case = [&]() {
    DEV_InputDevice input_device;
    LN_InputSnapshot input_snapshot;
    LN_TickReadContext read_context;
    read_context.input = &input_snapshot;
    read_context.input_channels = LN_DEP_INPUT_MOUSE;
    read_context.has_input = true;

    KX_GameObject owner;
    LN_RuntimeTree runtime_tree(program, &owner, 4, 0);

    CaptureMousePosition(input_device, input_snapshot, 0, 0);
    read_context.tick_index = 30;
    runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

    LN_TickContext context;
    context.use_fixed_timestep = true;
    context.tick_index = read_context.tick_index;
    EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

    input_device.ClearInputs();
    input_device.ConvertMoveEvent(100, 0);
    LN_InputSnapshot zero_delta_snapshot;
    CaptureCurrentMousePositionWithoutFrameDelta(input_device, zero_delta_snapshot);
    ASSERT_TRUE(zero_delta_snapshot.GetMouse().has_position);
    ASSERT_EQ(zero_delta_snapshot.GetMouse().x, 100);
    ASSERT_EQ(zero_delta_snapshot.GetMouse().delta_x, 0);
    ASSERT_EQ(zero_delta_snapshot.GetMouse().delta_y, 0);

    read_context.input = &zero_delta_snapshot;
    read_context.tick_index = 31;
    runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
    context.tick_index = read_context.tick_index;
    const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                context);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
    EXPECT_EQ(commands[0].object, &owner);
    EXPECT_NEAR(commands[0].vector_value.x(), 0.0f, 0.00001f);
    EXPECT_NEAR(commands[0].vector_value.y(), 0.0f, 0.00001f);
    EXPECT_NEAR(commands[0].vector_value.z(), -0.2f, 0.00001f);
    EXPECT_FALSE(commands[0].bool_value);
  };
  run_case();
}

TEST(LN_RuntimeTree, MouseLookFlushAppliesRotationToOwner)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(false);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  LN_TickReadContext read_context;
  read_context.input = &input_snapshot;
  read_context.input_channels = LN_DEP_INPUT_MOUSE;
  read_context.has_input = true;

  KX_GameObject owner;
  SG_Callbacks callbacks;
  SG_Node sg_node(&owner, nullptr, callbacks);
  sg_node.SetParentRelation(new TestRootParentRelation());
  owner.SetSGNode(&sg_node);
  owner.NodeUpdateGS(0.0);

  LN_RuntimeTree runtime_tree(program, &owner, 4, 0);
  CaptureMousePosition(input_device, input_snapshot, 0, 0);
  read_context.tick_index = 16;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = read_context.tick_index;

  {
    LN_CommandBuffer command_buffer;
    command_buffer.SetTypedCommandStreamsEnabled(true);
    command_buffer.BeginRecording();
    runtime_tree.ExecuteReady(command_buffer, context);
    command_buffer.EndRecording();
    command_buffer.Flush();
  }

  input_device.ClearInputs();
  input_device.ConvertMoveEvent(100, 0);
  LN_InputSnapshot zero_delta_snapshot;
  CaptureCurrentMousePositionWithoutFrameDelta(input_device, zero_delta_snapshot);
  ASSERT_TRUE(zero_delta_snapshot.GetMouse().has_position);
  ASSERT_EQ(zero_delta_snapshot.GetMouse().x, 100);
  ASSERT_EQ(zero_delta_snapshot.GetMouse().delta_x, 0);
  ASSERT_EQ(zero_delta_snapshot.GetMouse().delta_y, 0);

  read_context.input = &zero_delta_snapshot;
  read_context.tick_index = 17;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
  context.tick_index = read_context.tick_index;

  {
    LN_CommandBuffer command_buffer;
    command_buffer.SetTypedCommandStreamsEnabled(true);
    command_buffer.BeginRecording();
    runtime_tree.ExecuteReady(command_buffer, context);
    command_buffer.EndRecording();
    command_buffer.Flush();
  }

  MT_Scalar yaw = 0.0f;
  MT_Scalar pitch = 0.0f;
  MT_Scalar roll = 0.0f;
  owner.NodeGetLocalOrientation().getEuler(yaw, pitch, roll);
  EXPECT_NEAR(roll, -0.2f, 0.00001f);

  owner.SetSGNode(nullptr);
}

TEST(LN_RuntimeTree, CenterMouseLookSkipsInitialCenterTickThenRecordsRotation)
{
  std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;
  input_device.ConvertMoveEvent(0, 0);
  input_snapshot.Capture(&input_device, nullptr);
  input_device.ClearInputs();
  input_device.ConvertMoveEvent(100, 0);
  input_snapshot.Capture(&input_device, nullptr);

  LN_TickReadContext read_context;
  read_context.input = &input_snapshot;
  read_context.input_channels = LN_DEP_INPUT_MOUSE;
  read_context.tick_index = 21;
  read_context.has_input = true;

  KX_GameObject owner;
  LN_RuntimeTree runtime_tree(program, &owner, 4, 0);
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);

  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = read_context.tick_index;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  input_device.ClearInputs();
  input_device.ConvertMoveEvent(200, 0);
  input_device.ConvertMoveEvent(100, 0);
  input_snapshot.Capture(&input_device, nullptr);
  read_context.tick_index = 22;
  runtime_tree.CaptureSnapshot(&read_context, 1.0f / 60.0f, read_context.tick_index, false);
  context.tick_index = read_context.tick_index;

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(commands[0].object, &owner);
  EXPECT_NEAR(commands[0].vector_value.z(), -0.2f, 0.00001f);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsApplyPhysicsVectorDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t local_expr = AddBoolConstant(*program, true);

  LN_Instruction apply_physics_vector;
  apply_physics_vector.opcode = LN_OpCode::ApplyPhysicsVector;
  apply_physics_vector.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
  apply_physics_vector.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Force);
  apply_physics_vector.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  apply_physics_vector.bool_expr_index = local_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, apply_physics_vector);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(89), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_RuntimeTree ir_tree(program, FakeGameObjectPointer(89), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), reference_commands.size());
  ASSERT_EQ(ir_commands.size(), 1u);
  ExpectCommandEqual(ir_commands[0], reference_commands[0]);
  EXPECT_EQ(ir_commands[0].type, LN_CommandBuffer::CommandType::ApplyForce);
  EXPECT_TRUE(ir_commands[0].bool_value);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsApplyTransformVectorDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t local_expr = AddBoolConstant(*program, true);

  LN_Instruction apply_transform_vector;
  apply_transform_vector.opcode = LN_OpCode::ApplyTransformVector;
  apply_transform_vector.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
  apply_transform_vector.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Rotation);
  apply_transform_vector.vector_value = MT_Vector3(0.25f, 0.5f, 0.75f);
  apply_transform_vector.bool_expr_index = local_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, apply_transform_vector);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(90), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_RuntimeTree ir_tree(program, FakeGameObjectPointer(90), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), reference_commands.size());
  ASSERT_EQ(ir_commands.size(), 1u);
  ExpectCommandEqual(ir_commands[0], reference_commands[0]);
  EXPECT_EQ(ir_commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_TRUE(ir_commands[0].bool_value);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRExecutesDeferredTreeControlNoTargetDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t none_target = AddNoneValueConstant(*program);
  const uint32_t tree_name = AddStringConstant(*program, "Tree");
  const uint32_t initial_enabled = AddBoolConstant(*program, true);

  LN_Instruction set_enabled;
  set_enabled.opcode = LN_OpCode::SetLogicTreeEnabled;
  set_enabled.value_expr_index = none_target;
  set_enabled.string_expr_index = tree_name;
  set_enabled.bool_value = true;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_enabled);

  LN_Instruction install_tree;
  install_tree.opcode = LN_OpCode::InstallLogicTree;
  install_tree.value_expr_index = none_target;
  install_tree.string_expr_index = tree_name;
  install_tree.bool_expr_index = initial_enabled;
  program->AddInstruction(LN_Event::OnFixedUpdate, install_tree);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(91), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_RuntimeTree ir_tree(program, FakeGameObjectPointer(91), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  EXPECT_TRUE(reference_commands.empty());
  EXPECT_TRUE(ir_commands.empty());
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 2u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsAdditionalCommandFamiliesDirectly)
{
  std::shared_ptr<const LN_Program> program = CreateExecBlockIRCommandCoverageProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(48), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext ir_context;
  ir_context.use_fixed_timestep = true;
  LN_RuntimeTree ir_tree(program, FakeGameObjectPointer(48), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  ir_tree.ExecuteReady(command_buffer, ir_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> ir_commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(ir_commands.size(), reference_commands.size());
  ASSERT_EQ(ir_commands.size(), 72u);
  for (size_t index = 0; index < ir_commands.size(); index++) {
    ExpectCommandEqual(ir_commands[index], reference_commands[index]);
  }
  const LN_CommandBuffer::CommandType expected_types[] = {
      LN_CommandBuffer::CommandType::SetObjectColor,
      LN_CommandBuffer::CommandType::SetVisibility,
      LN_CommandBuffer::CommandType::ApplyMovement,
      LN_CommandBuffer::CommandType::ApplyRotation,
      LN_CommandBuffer::CommandType::ApplyForce,
      LN_CommandBuffer::CommandType::ApplyTorque,
      LN_CommandBuffer::CommandType::ApplyImpulse,
      LN_CommandBuffer::CommandType::ApplyMovement,
      LN_CommandBuffer::CommandType::MoveToward,
      LN_CommandBuffer::CommandType::AlignAxisToVector,
      LN_CommandBuffer::CommandType::RotateToward,
      LN_CommandBuffer::CommandType::SetCollisionGroup,
      LN_CommandBuffer::CommandType::SetPhysics,
      LN_CommandBuffer::CommandType::SetDynamics,
      LN_CommandBuffer::CommandType::RebuildCollisionShape,
      LN_CommandBuffer::CommandType::SetRigidBodyAttribute,
      LN_CommandBuffer::CommandType::SetGravity,
      LN_CommandBuffer::CommandType::CharacterJump,
      LN_CommandBuffer::CommandType::SetCharacterGravity,
      LN_CommandBuffer::CommandType::SetCharacterJumpSpeed,
      LN_CommandBuffer::CommandType::SetCharacterMaxJumps,
      LN_CommandBuffer::CommandType::SetCharacterWalkDirection,
      LN_CommandBuffer::CommandType::SetCharacterVelocity,
      LN_CommandBuffer::CommandType::VehicleControl,
      LN_CommandBuffer::CommandType::VehicleApplyEngineForce,
      LN_CommandBuffer::CommandType::VehicleApplyBraking,
      LN_CommandBuffer::CommandType::VehicleApplySteering,
      LN_CommandBuffer::CommandType::SetVehicleSuspensionCompression,
      LN_CommandBuffer::CommandType::SetVehicleSuspensionStiffness,
      LN_CommandBuffer::CommandType::SetVehicleSuspensionDamping,
      LN_CommandBuffer::CommandType::SetVehicleWheelFriction,
      LN_CommandBuffer::CommandType::MakeLightUnique,
      LN_CommandBuffer::CommandType::SetLightColor,
      LN_CommandBuffer::CommandType::SetLightPower,
      LN_CommandBuffer::CommandType::SetLightShadow,
      LN_CommandBuffer::CommandType::SetWindowSize,
      LN_CommandBuffer::CommandType::SetFullscreen,
      LN_CommandBuffer::CommandType::SetVSync,
      LN_CommandBuffer::CommandType::SetShowFramerate,
      LN_CommandBuffer::CommandType::SetShowProfile,
      LN_CommandBuffer::CommandType::SetCursorVisibility,
      LN_CommandBuffer::CommandType::SetGamepadVibration,
      LN_CommandBuffer::CommandType::StopAllSounds,
      LN_CommandBuffer::CommandType::PlaySound,
      LN_CommandBuffer::CommandType::PlaySound3D,
      LN_CommandBuffer::CommandType::PauseSound,
      LN_CommandBuffer::CommandType::ResumeSound,
      LN_CommandBuffer::CommandType::StopSound,
      LN_CommandBuffer::CommandType::PlayAction,
      LN_CommandBuffer::CommandType::StopAction,
      LN_CommandBuffer::CommandType::SetActionFrame,
      LN_CommandBuffer::CommandType::SetBonePoseLocation,
      LN_CommandBuffer::CommandType::SetBonePoseRotation,
      LN_CommandBuffer::CommandType::SetBonePoseTransform,
      LN_CommandBuffer::CommandType::SetBoneAttribute,
      LN_CommandBuffer::CommandType::SetBoneConstraintInfluence,
      LN_CommandBuffer::CommandType::SetBoneConstraintTarget,
      LN_CommandBuffer::CommandType::SetBoneConstraintAttribute,
      LN_CommandBuffer::CommandType::SetMaterialSlot,
      LN_CommandBuffer::CommandType::SetMaterialParameter,
      LN_CommandBuffer::CommandType::SetMaterialNodeSocketValue,
      LN_CommandBuffer::CommandType::Print,
      LN_CommandBuffer::CommandType::QuitGame,
      LN_CommandBuffer::CommandType::RestartGame,
      LN_CommandBuffer::CommandType::SetTimeScale,
      LN_CommandBuffer::CommandType::LoadBlendFile,
      LN_CommandBuffer::CommandType::SaveGame,
      LN_CommandBuffer::CommandType::LoadGame,
      LN_CommandBuffer::CommandType::LoadScene,
      LN_CommandBuffer::CommandType::SetScene,
      LN_CommandBuffer::CommandType::RemoveParent,
      LN_CommandBuffer::CommandType::RemoveObject,
  };
  ASSERT_EQ(std::size(expected_types), ir_commands.size());
  for (size_t index = 0; index < ir_commands.size(); index++) {
    EXPECT_EQ(ir_commands[index].type, expected_types[index]) << index;
  }
  const auto rotation_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetBonePoseRotation;
      });
  ASSERT_NE(rotation_command, ir_commands.end());
  EXPECT_EQ(rotation_command->secondary_int_value, 1);
  const auto transform_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetBonePoseTransform;
      });
  ASSERT_NE(transform_command, ir_commands.end());
  EXPECT_EQ(transform_command->vector_value.z(), 3.0f);
  EXPECT_EQ(transform_command->secondary_vector_value.y(), 0.8f);
  EXPECT_EQ(transform_command->int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(transform_command->secondary_int_value, 1);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count,
            program->GetExecBlockProgram(LN_Event::OnFixedUpdate).direct_instruction_count);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentMathCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterMathPositionProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(47), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(47), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  EXPECT_EQ(register_commands[0].type, reference_commands[0].type);
  EXPECT_EQ(register_commands[0].object, reference_commands[0].object);
  EXPECT_EQ(register_commands[0].vector_value.x(), reference_commands[0].vector_value.x());
  EXPECT_EQ(register_commands[0].vector_value.y(), reference_commands[0].vector_value.y());
  EXPECT_EQ(register_commands[0].vector_value.z(), reference_commands[0].vector_value.z());
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_GT(counters.register_scalar_op_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentColorCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterColorCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(52), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(52), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetObjectColor);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentColorSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterColorSnapshotCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(62), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(62), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetObjectColor);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentAdvancedScalarCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterAdvancedScalarCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(53), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(53), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentStringPredicateCommands)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterStringPredicateCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(54), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(54), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 2u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  ExpectCommandEqual(register_commands[1], reference_commands[1]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[1].int_value, 2);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentPureStringValueCommands)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  const uint32_t logic_index = AddStringConstant(*program, "logic");
  const uint32_t nodes_index = AddStringConstant(*program, " nodes");
  const uint32_t replacement_index = AddStringConstant(*program, "UPBGE");
  const uint32_t width_index = AddIntConstant(*program, 8);

  LN_StringExpression join;
  join.kind = LN_StringExpressionKind::Join;
  join.input0 = logic_index;
  join.input1 = nodes_index;
  const uint32_t join_index = program->AddStringExpression(join);

  LN_StringExpression replace;
  replace.kind = LN_StringExpressionKind::Replace;
  replace.input0 = join_index;
  replace.input1 = logic_index;
  replace.input2 = replacement_index;
  const uint32_t replace_index = program->AddStringExpression(replace);

  LN_StringExpression uppercase;
  uppercase.kind = LN_StringExpressionKind::ToUppercase;
  uppercase.input0 = replace_index;
  const uint32_t uppercase_index = program->AddStringExpression(uppercase);

  LN_StringExpression lowercase;
  lowercase.kind = LN_StringExpressionKind::ToLowercase;
  lowercase.input0 = replace_index;
  const uint32_t lowercase_index = program->AddStringExpression(lowercase);

  LN_StringExpression zerofill;
  zerofill.kind = LN_StringExpressionKind::ZeroFill;
  zerofill.input0 = logic_index;
  zerofill.int_expr_index = width_index;
  const uint32_t zerofill_index = program->AddStringExpression(zerofill);

  const uint32_t format_text_index = AddStringConstant(*program, "A:{} B:{} C:{} D:{}");

  LN_StringExpression format;
  format.kind = LN_StringExpressionKind::Format;
  format.input0 = format_text_index;
  format.input1 = replace_index;
  format.input2 = uppercase_index;
  format.input3 = lowercase_index;
  format.input4 = zerofill_index;
  const uint32_t format_index = program->AddStringExpression(format);

  LN_ValueExpression width_value;
  width_value.kind = LN_ValueExpressionKind::FromInt;
  width_value.input0 = width_index;
  const uint32_t width_value_index = program->AddValueExpression(width_value);

  LN_StringExpression from_value;
  from_value.kind = LN_StringExpressionKind::FromGenericValue;
  from_value.value_expr_index = width_value_index;
  const uint32_t from_value_index = program->AddStringExpression(from_value);

  const std::array<uint32_t, 7> string_indices = {join_index,
                                                  replace_index,
                                                  uppercase_index,
                                                  lowercase_index,
                                                  zerofill_index,
                                                  format_index,
                                                  from_value_index};
  for (const uint32_t string_index : string_indices) {
    LN_ValueExpression value;
    value.kind = LN_ValueExpressionKind::FromString;
    value.input0 = string_index;
    const uint32_t value_index = program->AddValueExpression(value);

    LN_Instruction print;
    print.opcode = LN_OpCode::Print;
    print.value_expr_index = value_index;
    program->AddInstruction(LN_Event::OnFixedUpdate, print);
  }

  LN_BoolExpression contains_upbge;
  contains_upbge.kind = LN_BoolExpressionKind::StringContains;
  contains_upbge.input0 = replace_index;
  contains_upbge.input1 = replacement_index;
  const uint32_t contains_upbge_index = program->AddBoolExpression(contains_upbge);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.bool_expr_index = contains_upbge_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility);

  LN_IntExpression count_nodes;
  count_nodes.kind = LN_IntExpressionKind::StringCount;
  count_nodes.input0 = replace_index;
  count_nodes.input1 = nodes_index;
  const uint32_t count_nodes_index = program->AddIntExpression(count_nodes);

  LN_Instruction set_collision_group;
  set_collision_group.opcode = LN_OpCode::SetCollisionGroup;
  set_collision_group.int_expr_index = count_nodes_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_collision_group);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.string_register_count, 6u);
  EXPECT_EQ(ir.fallback_expression_count, 0u);

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(55), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(55), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), string_indices.size() + 2u);
  for (size_t index = 0; index < string_indices.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
    EXPECT_EQ(register_commands[index].type, LN_CommandBuffer::CommandType::Print);
  }
  ExpectCommandEqual(register_commands[7], reference_commands[7]);
  ExpectCommandEqual(register_commands[8], reference_commands[8]);
  EXPECT_EQ(register_commands[0].property_name, "logic nodes");
  EXPECT_EQ(register_commands[1].property_name, "UPBGE nodes");
  EXPECT_EQ(register_commands[2].property_name, "UPBGE NODES");
  EXPECT_EQ(register_commands[3].property_name, "upbge nodes");
  EXPECT_EQ(register_commands[4].property_name, "000logic");
  EXPECT_EQ(register_commands[5].property_name,
            "A:UPBGE nodes B:UPBGE NODES C:upbge nodes D:000logic");
  EXPECT_EQ(register_commands[6].property_name, "8");
  EXPECT_EQ(register_commands[7].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_TRUE(register_commands[7].bool_value);
  EXPECT_EQ(register_commands[8].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[8].int_value, 1);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentIntSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterIntSnapshotCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(54), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(54), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesCharacterSnapshotIntFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterCharacterIntSnapshotFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(69), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(69), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentIntInputCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterIntInputCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(55), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(55), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesWindowResolutionFallbacks)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterWindowResolutionFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(77), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(77), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 4u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[2].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(register_commands[3].type, LN_CommandBuffer::CommandType::SetVSync);
  EXPECT_EQ(register_commands[2].vector_value.x(), 1.0f);
  EXPECT_EQ(register_commands[2].vector_value.y(), 2.0f);
  EXPECT_EQ(register_commands[2].vector_value.z(), 3.0f);
  EXPECT_EQ(register_commands[3].int_value, VSYNC_OFF);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesWindowFullscreenFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterWindowFullscreenFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(79), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(79), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_TRUE(register_commands[0].bool_value);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentVectorSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterVectorSnapshotCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(56), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(56), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesCharacterSnapshotVectorFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterCharacterVectorSnapshotFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(68), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(68), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentVectorInputCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterVectorInputCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(60), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(60), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesGamepadVectorInputFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterGamepadVectorInputFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(71), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(71), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentVectorResizeCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterVectorResizeCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(65), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(65), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentVectorRotateAroundAxisCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterVectorRotateAroundAxisCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(66), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(66), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentRandomCommands)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterRandomExpressionCommandProgram();

  LN_TickContext reference_context;
  reference_context.tick_index = 19;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(74), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.tick_index = reference_context.tick_index;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(74), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 3u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(register_commands[2].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesTickContextForNestedStringExpressions)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterNestedRandomStringCommandProgram();
  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);

  std::string first_message;
  bool saw_tick_dependent_message = false;
  for (const uint64_t tick_index : {1u, 2u, 5u, 13u, 21u, 34u}) {
    LN_TickContext reference_context;
    reference_context.tick_index = tick_index;
    reference_context.use_fixed_timestep = true;
    reference_context.use_register_expression_evaluator = false;
    LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(75), 4, 0);
    const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
        reference_tree, reference_context);

    LN_TickContext register_context;
    register_context.tick_index = tick_index;
    register_context.use_fixed_timestep = true;
    register_context.use_register_expression_evaluator = true;
    LN_RuntimeTree register_tree(program, FakeGameObjectPointer(75), 4, 0);
    LN_CommandBuffer command_buffer;
    LN_RuntimeProfileCounters counters;
    command_buffer.BeginRecording();
    register_tree.ExecuteReady(command_buffer, register_context, &counters);
    command_buffer.EndRecording();
    const std::vector<LN_CommandBuffer::Command> register_commands =
        command_buffer.TakeRecordedCommands();

    ASSERT_EQ(reference_commands.size(), 1u);
    ASSERT_EQ(register_commands.size(), reference_commands.size());
    ExpectCommandEqual(register_commands[0], reference_commands[0]);
    EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::Print);
    EXPECT_GT(counters.register_expression_hit_count, 0u);
    EXPECT_EQ(counters.register_expression_fallback_count, 0u);

    if (first_message.empty()) {
      first_message = register_commands[0].property_name;
    }
    else if (register_commands[0].property_name != first_message) {
      saw_tick_dependent_message = true;
    }
  }

  EXPECT_TRUE(saw_tick_dependent_message);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorUsesDefaultCombineComponentsForMovement)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterRandomPartialCombineMovementProgram();
  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);

  LN_TickContext reference_context;
  reference_context.tick_index = 37;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(76), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.tick_index = reference_context.tick_index;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(76), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::ApplyMovement);
  EXPECT_TRUE(register_commands[0].bool_value);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.x(), 0.0f);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), 0.0f);
  EXPECT_GT(counters.exec_direct_instruction_count, 0u);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorInvalidatesCachedRegistersByGeneration)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterRandomUpdateTimeScaleProgram();

  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(75), 4, 0);
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(75), 4, 0);

  auto record_tick = [](LN_RuntimeTree &tree,
                        const uint64_t tick_index,
                        const bool use_register_evaluator,
                        LN_RuntimeProfileCounters *counters = nullptr) {
    LN_TickContext context;
    context.tick_index = tick_index;
    context.use_fixed_timestep = true;
    context.use_register_expression_evaluator = use_register_evaluator;

    LN_CommandBuffer command_buffer;
    command_buffer.BeginRecording();
    tree.ExecuteReady(command_buffer, context, counters);
    command_buffer.EndRecording();
    return command_buffer.TakeRecordedCommands();
  };

  const std::vector<LN_CommandBuffer::Command> legacy_tick_10 = record_tick(
      reference_tree, 10, false);
  const std::vector<LN_CommandBuffer::Command> legacy_tick_11 = record_tick(
      reference_tree, 11, false);

  LN_RuntimeProfileCounters counters;
  const std::vector<LN_CommandBuffer::Command> register_tick_10 = record_tick(
      register_tree, 10, true, &counters);
  const std::vector<LN_CommandBuffer::Command> register_tick_11 = record_tick(
      register_tree, 11, true, &counters);

  ASSERT_EQ(legacy_tick_10.size(), 1u);
  ASSERT_EQ(legacy_tick_11.size(), 1u);
  ASSERT_EQ(register_tick_10.size(), 1u);
  ASSERT_EQ(register_tick_11.size(), 1u);

  ExpectCommandEqual(register_tick_10[0], legacy_tick_10[0]);
  ExpectCommandEqual(register_tick_11[0], legacy_tick_11[0]);
  EXPECT_EQ(register_tick_10[0].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(register_tick_11[0].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_NE(register_tick_10[0].scalar_value, register_tick_11[0].scalar_value);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentRuntimeTreePropertyCommands)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterRuntimeTreePropertyCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(75), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(75), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 6u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[2].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(register_commands[3].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(register_commands[4].type, LN_CommandBuffer::CommandType::SetObjectColor);
  EXPECT_EQ(register_commands[5].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentFloatSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterFloatSnapshotLightCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(57), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(57), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesGamepadFloatInputFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterGamepadFloatInputFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(73), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(73), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentBoolSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterBoolSnapshotVisibilityCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(58), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(58), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesCharacterSnapshotBoolFallback)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterCharacterBoolSnapshotFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(70), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(70), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentPropertyExistsSnapshotCommand)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterSnapshotGamePropertyExistsCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(64), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(64), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorPreservesSnapshotGamePropertyValueFallbacks)
{
  std::shared_ptr<const LN_Program> program =
      CreateRegisterSnapshotGamePropertyValueFallbackCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(76), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(76), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 3u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[2].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_GT(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentBoolInputCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterBoolInputCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(59), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(59), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentGamepadBoolInputCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterGamepadBoolInputCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(72), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(72), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentMouseMovedInputCommand)
{
  std::shared_ptr<const LN_Program> program = CreateRegisterMouseMovedInputCommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(63), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(63), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  ExpectCommandEqual(register_commands[0], reference_commands[0]);
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_GT(counters.register_expression_hit_count, 0u);
  EXPECT_EQ(counters.register_expression_fallback_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorMatchesTinyNormalizeFallback)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_VectorExpression tiny_vector;
  tiny_vector.kind = LN_VectorExpressionKind::Constant;
  tiny_vector.vector_value = MT_Vector3(-1.0e-20f, 0.0f, 0.0f);
  const uint32_t tiny_vector_index = program->AddVectorExpression(tiny_vector);

  LN_VectorExpression absolute;
  absolute.kind = LN_VectorExpressionKind::Absolute;
  absolute.input0 = tiny_vector_index;
  const uint32_t absolute_index = program->AddVectorExpression(absolute);

  LN_VectorExpression normalized;
  normalized.kind = LN_VectorExpressionKind::Normalize;
  normalized.input0 = absolute_index;
  const uint32_t normalized_index = program->AddVectorExpression(normalized);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = normalized_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(49), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(49), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  EXPECT_FLOAT_EQ(reference_commands[0].vector_value.x(), 0.0f);
  EXPECT_FLOAT_EQ(reference_commands[0].vector_value.y(), 0.0f);
  EXPECT_FLOAT_EQ(reference_commands[0].vector_value.z(), 0.0f);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.x(), reference_commands[0].vector_value.x());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), reference_commands[0].vector_value.y());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.z(), reference_commands[0].vector_value.z());
  EXPECT_GT(counters.register_expression_hit_count, 0u);
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorExecutesFloatSoASimdBatch)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_FloatExpression frame_delta;
  frame_delta.kind = LN_FloatExpressionKind::SnapshotFrameDelta;
  const uint32_t frame_delta_index = program->AddFloatExpression(frame_delta);

  uint32_t constant_indices[4];
  for (uint32_t index = 0; index < 4; index++) {
    LN_FloatExpression constant;
    constant.kind = LN_FloatExpressionKind::Constant;
    constant.float_value = float(index + 2);
    constant_indices[index] = program->AddFloatExpression(constant);
  }

  uint32_t add_indices[4];
  for (uint32_t index = 0; index < 4; index++) {
    LN_FloatExpression add;
    add.kind = LN_FloatExpressionKind::Add;
    add.input0 = frame_delta_index;
    add.input1 = constant_indices[index];
    add_indices[index] = program->AddFloatExpression(add);
  }

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = add_indices[0];
  vector.input1 = add_indices[1];
  vector.input2 = add_indices[2];
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = vector_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(50), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(50), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  EXPECT_EQ(register_commands[0].type, reference_commands[0].type);
  EXPECT_EQ(register_commands[0].object, reference_commands[0].object);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.x(), reference_commands[0].vector_value.x());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), reference_commands[0].vector_value.y());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.z(), reference_commands[0].vector_value.z());
  EXPECT_GE(counters.register_simd_candidate_lane_count, 4u);
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
  EXPECT_GT(counters.register_simd_batch_count, 0u);
  EXPECT_GE(counters.register_simd_lane_count, 4u);
#endif
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorExecutesFloatDivideSoASimdBatch)
{
  std::shared_ptr<LN_Program> program = CreateRegisterDivideSoACommandProgram();

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(54), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(54), 4, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  EXPECT_EQ(register_commands[0].type, reference_commands[0].type);
  EXPECT_EQ(register_commands[0].object, reference_commands[0].object);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.x(), reference_commands[0].vector_value.x());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), reference_commands[0].vector_value.y());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.z(), reference_commands[0].vector_value.z());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), 0.0f);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.z(), 0.0f);
  EXPECT_GE(counters.register_simd_candidate_lane_count, 4u);
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
  EXPECT_GT(counters.register_simd_batch_count, 0u);
  EXPECT_GE(counters.register_simd_lane_count, 4u);
#endif
}

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorFallsBackForDynamicExpressions)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_FloatExpression dynamic_float;
  dynamic_float.kind = LN_FloatExpressionKind::FromGenericValue;
  const uint32_t dynamic_index = program->AddFloatExpression(dynamic_float);

  LN_VectorExpression vector;
  vector.kind = LN_VectorExpressionKind::Combine;
  vector.input0 = dynamic_index;
  vector.input1 = dynamic_index;
  vector.input2 = dynamic_index;
  const uint32_t vector_index = program->AddVectorExpression(vector);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetTransformVector;
  instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  instruction.vector_expr_index = vector_index;
  program->AddInstruction(LN_Event::OnInit, instruction);

  LN_TickContext reference_context;
  reference_context.use_fixed_timestep = true;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(48), 4, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context;
  register_context.use_fixed_timestep = true;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(48), 4, 0);

  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 1u);
  EXPECT_EQ(register_commands[0].type, reference_commands[0].type);
  EXPECT_EQ(register_commands[0].object, reference_commands[0].object);
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.x(), reference_commands[0].vector_value.x());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), reference_commands[0].vector_value.y());
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.z(), reference_commands[0].vector_value.z());
  EXPECT_GT(counters.register_expression_fallback_count, 0u);

  const LN_RuntimeTree::RuntimeFallbackReport &report = register_tree.GetRuntimeFallbackReport();
  EXPECT_TRUE(report.optimized_execution_partial);
  EXPECT_GT(report.expression_fallback_count, 0u);
  ASSERT_EQ(report.expression_hits.size(), 1u);
  EXPECT_EQ(report.expression_hits[0].family, LN_RuntimeExpressionFamily::Float);
  EXPECT_EQ(report.expression_hits[0].kind, uint32_t(LN_FloatExpressionKind::FromGenericValue));
  EXPECT_EQ(report.expression_hits[0].expression_index, dynamic_index);
  EXPECT_EQ(report.expression_hits[0].reason, LN_RuntimeFallbackReason::DynamicLookup);
  EXPECT_GE(report.expression_hits[0].count, 1u);
  ASSERT_NE(report.expression_hits[0].expression_name, nullptr);
  EXPECT_STREQ(report.expression_hits[0].expression_name, "FromGenericValue");
  ASSERT_NE(report.expression_hits[0].profile_counter_name, nullptr);
  EXPECT_STREQ(report.expression_hits[0].profile_counter_name,
               "register_expression_fallback_count");
  ASSERT_NE(report.expression_hits[0].removal_condition, nullptr);
  EXPECT_STRNE(report.expression_hits[0].removal_condition, "");
  EXPECT_NE(report.expression_reason_mask &
                (1u << uint8_t(LN_RuntimeFallbackReason::DynamicLookup)),
            0u);
}

TEST(LN_RuntimeTree, WarmQueryCacheStabilizesSnapshotQueryReads)
{
  std::shared_ptr<LN_Program> program = CreateSnapshotQueryGuardProgram();
  ASSERT_TRUE(program->IsParallelEligible());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(41), 1, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 21;

  EXPECT_EQ(runtime_tree.WarmQueryCache(context), 1u);
  EXPECT_EQ(runtime_tree.WarmQueryCache(context), 0u);

  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  EXPECT_TRUE(commands.empty());
}

TEST(LN_RuntimeTree, WarmQueryCacheSkipsRaycastsThatReadLiveTransforms)
{
  std::shared_ptr<LN_Program> program = CreateSnapshotQueryGuardProgram(
      LN_QueryExpressionKind::Raycast);
  ASSERT_FALSE(program->IsParallelEligible());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(41), 1, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 22;

  EXPECT_EQ(runtime_tree.WarmQueryCache(context), 0u);
  EXPECT_EQ(runtime_tree.WarmQueryCache(context), 0u);

  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  EXPECT_TRUE(commands.empty());
}

TEST(LN_RuntimeTree, QueryDiagnosticsDistinguishInvalidTargetAndMissingWorld)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);

  const LN_PhysicsQueryResult invalid_target = runtime_tree.Raycast(
      MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 0.0f), LN_RuntimeRef(), 0u);
  EXPECT_EQ(invalid_target.diagnostic_status, LN_QueryDiagnosticStatus::InvalidTarget);
  EXPECT_NE(LN_DescribePhysicsQueryResult(invalid_target).find("status=InvalidTarget"),
            std::string::npos);

  const LN_PhysicsQueryResult missing_world = runtime_tree.Raycast(
      MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 1.0f), LN_RuntimeRef(), 0u);
  EXPECT_EQ(missing_world.diagnostic_status, LN_QueryDiagnosticStatus::MissingPhysicsWorld);
  EXPECT_NE(LN_DescribePhysicsQueryResult(missing_world).find("status=MissingPhysicsWorld"),
            std::string::npos);

  EXPECT_STREQ(LN_QueryDiagnosticStatusName(LN_QueryDiagnosticStatus::NoHit), "NoHit");
  EXPECT_STREQ(LN_QueryDiagnosticStatusName(LN_QueryDiagnosticStatus::InvalidFilter),
               "InvalidFilter");
  EXPECT_STREQ(
      LN_QueryDiagnosticStatusName(LN_QueryDiagnosticStatus::UnavailableUnsnapshottedData),
      "UnavailableUnsnapshottedData");
}

TEST(LN_RuntimeTree, ParallelExecutorPrewarmsQueryCachesBeforeWorkers)
{
  std::shared_ptr<LN_Program> program = CreateSnapshotQueryGuardProgram();
  ASSERT_TRUE(program->IsParallelEligible());

  LN_RuntimeTree first_tree(program, FakeGameObjectPointer(42), 1, 0);
  LN_RuntimeTree second_tree(program, FakeGameObjectPointer(43), 2, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 22;

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({2, &second_tree});
  work_items.push_back({1, &first_tree});

  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists =
      executor.ExecuteTreesToCommandLists(work_items, context);

  ASSERT_EQ(command_lists.size(), 2u);
  EXPECT_EQ(command_lists[0].runtime_tree_index, 1u);
  EXPECT_EQ(command_lists[1].runtime_tree_index, 2u);
  EXPECT_TRUE(command_lists[0].commands.empty());
  EXPECT_TRUE(command_lists[1].commands.empty());
  EXPECT_EQ(first_tree.WarmQueryCache(context), 0u);
  EXPECT_EQ(second_tree.WarmQueryCache(context), 0u);
}

TEST(LN_RuntimeTree, MixedSerialAndParallelCommandListsAggregateCanonically)
{
  std::shared_ptr<LN_Program> serial_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  LN_BoolExpression collision;
  collision.kind = LN_BoolExpressionKind::CollisionDetected;
  serial_program->AddBoolExpression(collision);
  ASSERT_FALSE(serial_program->IsParallelEligible());

  std::shared_ptr<const LN_Program> worker_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(2.0f, 0.0f, 0.0f));
  ASSERT_TRUE(worker_program->IsParallelEligible());

  LN_RuntimeTree serial_tree(serial_program, FakeGameObjectPointer(2), 2, 0);
  LN_RuntimeTree worker_tree(worker_program, FakeGameObjectPointer(1), 1, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;

  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  LN_CommandBuffer::RecordedCommandList serial_commands;
  serial_commands.runtime_tree_index = 2;
  serial_commands.commands = RecordReadyCommands(serial_tree, context);
  command_lists.push_back(std::move(serial_commands));

  LN_ParallelTreeExecutor executor;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> worker_items;
  worker_items.push_back({1, &worker_tree});
  std::vector<LN_CommandBuffer::RecordedCommandList> worker_command_lists =
      executor.ExecuteTreesToCommandLists(worker_items, context);
  command_lists.insert(command_lists.end(),
                       std::make_move_iterator(worker_command_lists.begin()),
                       std::make_move_iterator(worker_command_lists.end()));

  LN_CommandBuffer aggregate_buffer;
  aggregate_buffer.MergeRecordedCommandLists(std::move(command_lists));

  ASSERT_EQ(aggregate_buffer.Size(), 2u);
  const std::vector<LN_CommandBuffer::Command> &commands = aggregate_buffer.GetCommandsForTests();
  EXPECT_EQ(commands[0].object, FakeGameObjectPointer(1));
  EXPECT_EQ(commands[1].object, FakeGameObjectPointer(2));
}

TEST(LN_RuntimeTree, SchedulerPlannerBatchesCompatibleWorkerTrees)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 0.0f, 0.0f));
  ASSERT_TRUE(program->IsParallelEligible());

  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  for (size_t index = 0; index < 20; index++) {
    trees.push_back(std::make_unique<LN_RuntimeTree>(
        program, FakeGameObjectPointer(uint32_t(index + 1)), uint32_t(index), 0));
    work_items.push_back({index, trees.back().get()});
  }

  const LN_ParallelTreeExecutor::SchedulerPlan plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(
      work_items, true);
  const LN_ParallelTreeExecutor::SchedulerPlannerStats stats =
      LN_ParallelTreeExecutor::StatsForPlan(plan);

  EXPECT_EQ(stats.planned_tree_count, 20u);
  EXPECT_EQ(stats.worker_tree_count, 20u);
  EXPECT_EQ(stats.main_thread_tree_count, 0u);
  EXPECT_GT(stats.worker_batch_count, 0u);
  EXPECT_LT(stats.worker_batch_count, stats.worker_tree_count);
  EXPECT_GT(stats.max_trees_per_worker_batch, 1u);
  EXPECT_NE(stats.command_resource_classes & LN_DEP_COMMAND_TRANSFORM, 0u);
  EXPECT_EQ(stats.command_record_only_tree_count, 20u);
  EXPECT_TRUE(stats.immutable_worker_inputs);
  EXPECT_TRUE(stats.worker_metrics_are_local);
  EXPECT_TRUE(stats.deterministic_merge_order);
  EXPECT_TRUE(stats.main_thread_flush_isolated);
  EXPECT_TRUE(LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(plan, stats, false));
  EXPECT_FALSE(LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(plan, stats, true));
}

TEST(LN_RuntimeTree, SchedulerPlannerKeepsMainThreadOnlyTreesOutOfWorkerBatches)
{
  std::shared_ptr<LN_Program> main_thread_program = CreateDeterministicMainThreadCommandProgram();
  ASSERT_FALSE(main_thread_program->IsParallelEligible());
  std::shared_ptr<const LN_Program> worker_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(3.0f, 0.0f, 0.0f));
  ASSERT_TRUE(worker_program->IsParallelEligible());

  LN_RuntimeTree main_thread_tree(main_thread_program, FakeGameObjectPointer(10), 10, 0);
  LN_RuntimeTree worker_tree(worker_program, FakeGameObjectPointer(11), 11, 0);

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({10, &main_thread_tree});
  work_items.push_back({11, &worker_tree});

  const LN_ParallelTreeExecutor::SchedulerPlan plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(
      work_items, true);
  const LN_ParallelTreeExecutor::SchedulerPlannerStats stats =
      LN_ParallelTreeExecutor::StatsForPlan(plan);

  ASSERT_EQ(plan.main_thread_items.size(), 1u);
  EXPECT_EQ(plan.main_thread_items.front().runtime_tree, &main_thread_tree);
  EXPECT_EQ(stats.worker_tree_count, 1u);
  EXPECT_EQ(stats.main_thread_tree_count, 1u);
  EXPECT_EQ(stats.main_thread_fallback_count, 1u);
  EXPECT_NE(stats.main_thread_resource_access & LN_SCHEDULER_RESOURCE_SCENE, 0u);
}

TEST(LN_RuntimeTree, SchedulerPlannerKeepsMouseLookOnMainThread)
{
  std::shared_ptr<LN_Program> mouse_look_program = CreateMouseLookInputOnlyProgram(true);
  ASSERT_FALSE(mouse_look_program->IsParallelEligible());
  EXPECT_EQ(mouse_look_program->GetMainThreadOnlyReason(),
            LN_MainThreadOnlyReason::InputDeviceState);

  LN_RuntimeTree mouse_look_tree(mouse_look_program, FakeGameObjectPointer(12), 12, 0);

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  work_items.push_back({12, &mouse_look_tree});

  const LN_ParallelTreeExecutor::SchedulerPlan plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(
      work_items, true);
  const LN_ParallelTreeExecutor::SchedulerPlannerStats stats =
      LN_ParallelTreeExecutor::StatsForPlan(plan);

  ASSERT_EQ(plan.main_thread_items.size(), 1u);
  EXPECT_EQ(plan.main_thread_items.front().runtime_tree, &mouse_look_tree);
  EXPECT_EQ(stats.worker_tree_count, 0u);
  EXPECT_EQ(stats.main_thread_tree_count, 1u);
  EXPECT_NE(stats.main_thread_resource_access & LN_SCHEDULER_RESOURCE_INPUT, 0u);
}

TEST(LN_RuntimeTree, SchedulerDirectSerialAllowsMostlyCommandOnlyMixedSmallTrees)
{
  std::shared_ptr<const LN_Program> command_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(3.0f, 0.0f, 0.0f));
  std::shared_ptr<LN_Program> main_thread_program = CreateDeterministicMainThreadCommandProgram();

  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  for (size_t index = 0; index < 96; index++) {
    trees.push_back(std::make_unique<LN_RuntimeTree>(
        command_program, FakeGameObjectPointer(uint32_t(index + 1)), uint32_t(index), 0));
    work_items.push_back({index, trees.back().get()});
  }
  for (size_t index = 0; index < 4; index++) {
    const size_t tree_index = 96 + index;
    trees.push_back(
        std::make_unique<LN_RuntimeTree>(main_thread_program,
                                         FakeGameObjectPointer(uint32_t(tree_index + 1)),
                                         uint32_t(tree_index),
                                         0));
    work_items.push_back({tree_index, trees.back().get()});
  }

  const LN_ParallelTreeExecutor::SchedulerPlan plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(
      work_items, true);
  const LN_ParallelTreeExecutor::SchedulerPlannerStats stats =
      LN_ParallelTreeExecutor::StatsForPlan(plan);

  EXPECT_EQ(stats.planned_tree_count, 100u);
  EXPECT_EQ(stats.command_record_only_tree_count, 96u);
  EXPECT_EQ(stats.main_thread_tree_count, 4u);
  EXPECT_TRUE(LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(plan, stats, false));
}

TEST(LN_RuntimeTree, SchedulerDirectSerialRejectsSmallMixedCommandOnlyWorkloads)
{
  std::shared_ptr<const LN_Program> command_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(3.0f, 0.0f, 0.0f));
  std::shared_ptr<LN_Program> main_thread_program = CreateDeterministicMainThreadCommandProgram();

  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  for (size_t index = 0; index < 20; index++) {
    trees.push_back(std::make_unique<LN_RuntimeTree>(
        command_program, FakeGameObjectPointer(uint32_t(index + 1)), uint32_t(index), 0));
    work_items.push_back({index, trees.back().get()});
  }
  for (size_t index = 0; index < 4; index++) {
    const size_t tree_index = 20 + index;
    trees.push_back(
        std::make_unique<LN_RuntimeTree>(main_thread_program,
                                         FakeGameObjectPointer(uint32_t(tree_index + 1)),
                                         uint32_t(tree_index),
                                         0));
    work_items.push_back({tree_index, trees.back().get()});
  }

  const LN_ParallelTreeExecutor::SchedulerPlan plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(
      work_items, true);
  const LN_ParallelTreeExecutor::SchedulerPlannerStats stats =
      LN_ParallelTreeExecutor::StatsForPlan(plan);

  EXPECT_EQ(stats.planned_tree_count, 24u);
  EXPECT_EQ(stats.command_record_only_tree_count, 20u);
  EXPECT_FALSE(LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(plan, stats, false));
}

TEST(LN_RuntimeTree, SchedulerExecutorReportsBatchedPlannerStats)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(2.0f, 0.0f, 0.0f));
  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> work_items;
  for (size_t index = 0; index < 12; index++) {
    trees.push_back(std::make_unique<LN_RuntimeTree>(
        program, FakeGameObjectPointer(uint32_t(index + 20)), uint32_t(index), 0));
    work_items.push_back({index, trees.back().get()});
  }

  LN_TickContext context;
  context.use_fixed_timestep = true;
  LN_ParallelTreeExecutor executor;
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists =
      executor.ExecuteTreesToCommandLists(work_items, context, true);

  const LN_ParallelTreeExecutor::SchedulerPlannerStats &stats = executor.GetLastPlannerStats();
  EXPECT_EQ(command_lists.size(), 12u);
  EXPECT_EQ(stats.worker_tree_count, 12u);
  EXPECT_GT(stats.worker_batch_count, 0u);
  EXPECT_LT(stats.worker_batch_count, stats.worker_tree_count);
  EXPECT_GT(stats.average_trees_per_worker_batch_x100, 100u);
  for (size_t index = 0; index < command_lists.size(); index++) {
    EXPECT_EQ(command_lists[index].runtime_tree_index, index);
    ASSERT_EQ(command_lists[index].commands.size(), 1u);
    EXPECT_EQ(command_lists[index].commands.front().type,
              LN_CommandBuffer::CommandType::SetWorldPosition);
  }
}

TEST(LN_RuntimeTree, SerialAndSchedulerCommandStreamsMatch)
{
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 12;

  std::vector<std::unique_ptr<LN_RuntimeTree>> serial_trees = MakeDeterminismRuntimeTrees();
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> serial_items =
      MakeDeterminismWorkItems(serial_trees);
  std::vector<LN_CommandBuffer::Command> serial_commands = RecordSerialCommandStream(serial_items,
                                                                                     context);

  std::vector<std::unique_ptr<LN_RuntimeTree>> scheduler_trees = MakeDeterminismRuntimeTrees();
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> scheduler_items =
      MakeDeterminismWorkItems(scheduler_trees);
  std::vector<LN_CommandBuffer::Command> scheduler_commands = RecordSchedulerCommandStream(
      scheduler_items, context);

  ExpectCommandStreamsEqual(scheduler_commands, serial_commands);
}

TEST(LN_RuntimeTree, SerialAndSchedulerForcedRunOnceStreamsMatch)
{
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 21;

  std::shared_ptr<LN_Program> program = CreateDeterministicWorkerCommandProgram();
  KX_GameObject test_game_object;
  LN_RuntimeTree serial_tree(program, &test_game_object, 5, 0);
  LN_RuntimeTree scheduler_tree(program, &test_game_object, 5, 0);

  std::vector<LN_CommandBuffer::Command> serial_commands = RecordForcedCommands(serial_tree,
                                                                                context);
  std::vector<LN_CommandBuffer::Command> scheduler_commands = RecordForcedCommands(scheduler_tree,
                                                                                   context);

  LN_CommandBuffer::SortCommands(serial_commands);
  LN_CommandBuffer::SortCommands(scheduler_commands);
  ExpectCommandStreamsEqual(scheduler_commands, serial_commands);
}

TEST(LN_RuntimeTree, ForcedRunOnceExecutionRecordsThroughLocalBuffer)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(4.0f, 0.0f, 0.0f));
  KX_GameObject test_game_object;
  LN_RuntimeTree runtime_tree(program, &test_game_object, 4, 0);

  LN_TickContext context;
  context.use_fixed_timestep = true;

  std::vector<LN_CommandBuffer::Command> commands = RecordForcedCommands(runtime_tree, context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands.front().type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(commands.front().object, &test_game_object);
}

TEST(LN_RuntimeTree, TreePropertiesUseDefaultsAndCanBeUpdated)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_TreePropertyRef property_ref;
  property_ref.name = "score";
  property_ref.value_type = LN_ValueType::Int;
  property_ref.default_value.int_value = 4;
  const uint32_t property_index = program->AddTreePropertyRef(property_ref);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(8), 1, 0);
  const LN_Value *initial_value = runtime_tree.GetTreePropertyValue(property_index);
  ASSERT_NE(initial_value, nullptr);
  EXPECT_TRUE(initial_value->exists);
  EXPECT_EQ(initial_value->type, LN_ValueType::Int);
  EXPECT_EQ(initial_value->int_value, 4);

  LN_Value updated_value;
  updated_value.type = LN_ValueType::Int;
  updated_value.exists = true;
  updated_value.int_value = 11;
  EXPECT_TRUE(runtime_tree.SetTreePropertyValue(property_index, updated_value));

  const LN_Value *stored_value = runtime_tree.GetTreePropertyValue(property_index);
  ASSERT_NE(stored_value, nullptr);
  EXPECT_EQ(stored_value->int_value, 11);
  EXPECT_EQ(runtime_tree.GetTreePropertyValue(42), nullptr);
  EXPECT_FALSE(runtime_tree.SetTreePropertyValue(42, updated_value));
}

TEST(LN_RuntimeTree, CompatibleProgramSwapPreservesTreeProperties)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_TreePropertyRef property_ref;
  property_ref.name = "accumulator";
  property_ref.value_type = LN_ValueType::Float;
  property_ref.default_value.float_value = 1.0f;
  const uint32_t property_index = program->AddTreePropertyRef(property_ref);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(9), 1, 0);
  LN_Value updated_value;
  updated_value.type = LN_ValueType::Float;
  updated_value.exists = true;
  updated_value.float_value = 8.5f;
  EXPECT_TRUE(runtime_tree.SetTreePropertyValue(property_index, updated_value));

  std::shared_ptr<LN_Program> compatible_program = std::make_shared<LN_Program>();
  compatible_program->AddTreePropertyRef(property_ref);
  runtime_tree.SetProgram(compatible_program);

  const LN_Value *stored_value = runtime_tree.GetTreePropertyValue(property_index);
  ASSERT_NE(stored_value, nullptr);
  EXPECT_EQ(stored_value->type, LN_ValueType::Float);
  EXPECT_NEAR(stored_value->float_value, 8.5f, 0.0001f);
}

TEST(LN_RuntimeTree, ObjectRefsInvalidateWithGeneration)
{
  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(0.0f, 0.0f, 0.0f));
  KX_GameObject *fake_game_object = FakeGameObjectPointer(10);
  LN_RuntimeTree runtime_tree(program, fake_game_object, 1, 0);

  const LN_RuntimeRef first_ref = runtime_tree.MakeObjectRef(fake_game_object, "owner");
  EXPECT_TRUE(first_ref.IsValid());
  EXPECT_EQ(runtime_tree.ResolveObjectRef(first_ref), fake_game_object);

  runtime_tree.InvalidateObjectRef(fake_game_object);
  EXPECT_EQ(runtime_tree.ResolveObjectRef(first_ref), nullptr);

  const LN_RuntimeRef second_ref = runtime_tree.MakeObjectRef(fake_game_object, "owner");
  EXPECT_TRUE(second_ref.IsValid());
  EXPECT_NE(second_ref.generation, first_ref.generation);
  EXPECT_EQ(runtime_tree.ResolveObjectRef(second_ref), fake_game_object);

  runtime_tree.DetachGameObject();
  EXPECT_EQ(runtime_tree.ResolveObjectRef(second_ref), nullptr);
}

TEST(LN_RuntimeTree, CollisionExitPayloadUsesLifetimeSafeObjectRefs)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  KX_GameObject hit_object;
  LN_RuntimeTree runtime_tree(program, &owner, 1, 0);

  constexpr uint64_t hit_tick = 10;
  constexpr uint64_t exit_tick = 11;
  LN_RuntimeTreeTestAccess::StoreCollisionPayload(
      runtime_tree, &owner, &hit_object, hit_tick);
  EXPECT_FALSE(
      LN_RuntimeTreeTestAccess::PersistentPayloadContainsRawObjectPointers(runtime_tree));

  LN_RuntimeTreeTestAccess::MarkCollisionExit(runtime_tree, &owner, exit_tick);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObject(runtime_tree, &owner, exit_tick),
            &hit_object);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObjectCount(runtime_tree, &owner, exit_tick), 1u);

  runtime_tree.InvalidateObjectRef(&hit_object);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObject(runtime_tree, &owner, exit_tick), nullptr);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObjectCount(runtime_tree, &owner, exit_tick), 0u);
}

TEST(LN_RuntimeTree, CollisionExitPayloadPromotesRemainingValidHitObject)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  KX_GameObject first_hit_object;
  KX_GameObject second_hit_object;
  LN_RuntimeTree runtime_tree(program, &owner, 1, 0);

  constexpr uint64_t hit_tick = 20;
  constexpr uint64_t exit_tick = 21;
  LN_RuntimeTreeTestAccess::StoreCollisionPayloads(
      runtime_tree, &owner, {&first_hit_object, &second_hit_object}, hit_tick);
  runtime_tree.InvalidateObjectRef(&first_hit_object);
  LN_RuntimeTreeTestAccess::MarkCollisionExit(runtime_tree, &owner, exit_tick);

  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObject(runtime_tree, &owner, exit_tick),
            &second_hit_object);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObjectCount(runtime_tree, &owner, exit_tick), 1u);
}

TEST(LN_RuntimeTree, RuntimeFallbackReportRecordsStaleObjectRefDuringExecution)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(11);
  KX_GameObject *target = FakeGameObjectPointer(12);
  LN_RuntimeTree runtime_tree(program, owner, 1, 0);

  const LN_RuntimeRef target_ref = runtime_tree.MakeObjectRef(target, "target");
  const uint32_t target_expr = AddObjectRefConstant(*program, target_ref);
  const uint32_t position_expr = AddVectorConstant(*program, MT_Vector3(1.0f, 2.0f, 3.0f));

  LN_Instruction set_position;
  set_position.opcode = LN_OpCode::SetWorldPosition;
  set_position.value_expr_index = target_expr;
  set_position.vector_expr_index = position_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_position);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  runtime_tree.InvalidateObjectRef(target);

  LN_TickContext context;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, &counters);
  command_buffer.EndRecording();

  EXPECT_TRUE(command_buffer.TakeRecordedCommands().empty());
  const LN_RuntimeTree::RuntimeFallbackReport &report = runtime_tree.GetRuntimeFallbackReport();
  EXPECT_TRUE(report.optimized_execution_partial);
  EXPECT_EQ(report.system_fallback_count, 1u);
  EXPECT_NE(report.system_reason_mask & (1u << uint8_t(LN_RuntimeFallbackReason::StaleHandle)),
            0u);
  ASSERT_EQ(report.system_hits.size(), 1u);
  EXPECT_EQ(report.system_hits[0].reason, LN_RuntimeFallbackReason::StaleHandle);
  EXPECT_EQ(report.system_hits[0].debug_name, "target");
  EXPECT_STREQ(report.system_hits[0].profile_counter_name, "fallback_paths");
  EXPECT_NE(report.system_hits[0].removal_condition, nullptr);
  EXPECT_EQ(counters.fallback_path_count, 1u);
}

TEST(LN_RuntimeTree, RuntimeFallbackReportRecordsMissingSnapshotChannelDuringExecution)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(13);
  LN_RuntimeTree runtime_tree(program, owner, 1, 0);
  const uint32_t owner_expr = AddObjectRefConstant(*program,
                                                   runtime_tree.MakeObjectRef(owner, "owner"));

  LN_BoolExpression visible;
  visible.kind = LN_BoolExpressionKind::SnapshotVisibility;
  const uint32_t visible_expr = program->AddBoolExpression(visible);

  LN_Instruction set_visibility;
  set_visibility.opcode = LN_OpCode::SetVisibility;
  set_visibility.value_expr_index = owner_expr;
  set_visibility.bool_expr_index = visible_expr;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility);

  LN_TickContext context;
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, &counters);
  command_buffer.EndRecording();

  const LN_RuntimeTree::RuntimeFallbackReport &report = runtime_tree.GetRuntimeFallbackReport();
  EXPECT_TRUE(report.optimized_execution_partial);
  EXPECT_EQ(report.system_fallback_count, 1u);
  EXPECT_NE(report.system_reason_mask &
                (1u << uint8_t(LN_RuntimeFallbackReason::MissingSnapshotChannel)),
            0u);
  ASSERT_EQ(report.system_hits.size(), 1u);
  EXPECT_EQ(report.system_hits[0].reason, LN_RuntimeFallbackReason::MissingSnapshotChannel);
  EXPECT_NE(report.system_hits[0].debug_name.find("missing_snapshot_channels=0x"),
            std::string::npos);
  EXPECT_STREQ(report.system_hits[0].profile_counter_name, "fallback_paths");
  EXPECT_NE(report.system_hits[0].removal_condition, nullptr);
  EXPECT_EQ(counters.fallback_path_count, 1u);
}

TEST(LN_RuntimeTree, ValuesCarryNativeContainersAndRefs)
{
  LN_Value list_value;
  list_value.type = LN_ValueType::List;
  LN_Value int_value;
  int_value.type = LN_ValueType::Int;
  int_value.int_value = 3;
  list_value.list_value.push_back(int_value);

  LN_Value dict_value;
  dict_value.type = LN_ValueType::Dict;
  dict_value.dict_value["items"] = list_value;

  ASSERT_EQ(dict_value.dict_value.count("items"), 1u);
  const LN_Value &stored_list = dict_value.dict_value["items"];
  ASSERT_EQ(stored_list.list_value.size(), 1u);
  EXPECT_EQ(stored_list.list_value.front().int_value, 3);
}

TEST(LN_Program, GroupCallFramesDeduplicateByTreeAndInterface)
{
  LN_Program program;
  LN_GroupCallFrame call_frame;
  call_frame.group_name = "Movement Group";
  call_frame.source_tree_name = "LN Movement";
  call_frame.interface_key = "flow:vec3";
  call_frame.input_value_count = 2;
  call_frame.output_value_count = 1;
  call_frame.owns_state = true;

  const uint32_t first_index = program.AddGroupCallFrame(call_frame);
  const uint32_t second_index = program.AddGroupCallFrame(call_frame);

  EXPECT_EQ(first_index, second_index);
  ASSERT_EQ(program.GetGroupCallFrames().size(), 1u);
  EXPECT_EQ(program.GetGroupCallFrames().front().input_value_count, 2u);
  EXPECT_TRUE(program.GetGroupCallFrames().front().owns_state);
}

}  // namespace
