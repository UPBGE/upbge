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
#include <tuple>
#include <utility>
#include <vector>

#include "DEV_InputDevice.h"
#include "EXP_IntValue.h"
#include "KX_GameObject.h"
#include "LN_BonePoseRotation.h"
#include "LN_CommandBuffer.h"
#include "LN_Inventory.h"
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
  /** The Inventory node's operation on `slots`, with the list's Inventory Rules. Returns what
   * did not fit (Leftover) and the outcome. */
  static std::pair<int32_t, LN_InventoryOutcome> RunInventoryEdit(
      LN_RuntimeTree &tree,
      const LN_ValueExpression &edit,
      std::vector<LN_Value> &slots,
      const LN_InventoryRules *list_rules)
  {
    LN_TickContext context;
    const auto result = tree.RunInventoryEdit(
        edit, slots, context, nullptr, list_rules, nullptr, tree.m_gameObject);
    return {result.amount, result.outcome};
  }

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

  static LN_Rotation ResolveRotation(LN_RuntimeTree &tree,
                                     const uint32_t expression_index,
                                     const LN_Rotation &fallback,
                                     const LN_TickContext &context)
  {
    return tree.ResolveRotationExpression(expression_index, fallback, context);
  }

  static float ResolveFloat(LN_RuntimeTree &tree,
                            const uint32_t expression_index,
                            const float fallback,
                            const LN_TickContext &context)
  {
    return tree.ResolveFloatExpression(expression_index, fallback, context);
  }

  static MT_Vector3 ResolveVector(LN_RuntimeTree &tree,
                                  const uint32_t expression_index,
                                  const MT_Vector3 &fallback,
                                  const LN_TickContext &context)
  {
    return tree.ResolveVectorExpression(expression_index, fallback, context);
  }

  static LN_Value ResolveValue(LN_RuntimeTree &tree,
                               const uint32_t expression_index,
                               const LN_TickContext &context)
  {
    return tree.ResolveValueExpression(expression_index, LN_Value(), context);
  }

  static bool ResolveBool(LN_RuntimeTree &tree,
                          const uint32_t expression_index,
                          const LN_TickContext &context)
  {
    return tree.ResolveBoolExpression(expression_index, false, context);
  }

  static int32_t ResolveInt(LN_RuntimeTree &tree,
                            const uint32_t expression_index,
                            const LN_TickContext &context)
  {
    return tree.ResolveIntExpression(expression_index, 0, context);
  }

  static LN_RuntimeRef MakeObjectRef(LN_RuntimeTree &tree, KX_GameObject &object)
  {
    return tree.MakeObjectRef(&object, object.GetName());
  }

  static std::vector<LN_RuntimeRef> ResolveIgnoreObjectRefs(LN_RuntimeTree &tree,
                                                            const uint32_t object_expr_index,
                                                            const uint32_t list_expr_index,
                                                            const LN_TickContext &context)
  {
    return tree.ResolveIgnoreObjectRefs(object_expr_index, list_expr_index, context);
  }

  static const LN_QueryResult &ResolveQuery(LN_RuntimeTree &tree,
                                            const uint32_t expression_index,
                                            const LN_TickContext &context)
  {
    return tree.ResolveQueryExpression(expression_index, context);
  }

  static void SeedQueryResult(LN_RuntimeTree &tree,
                              const uint32_t state_index,
                              const uint64_t tick_index,
                              const LN_QueryResult &result)
  {
    LN_RuntimeTree::QueryExpressionRuntimeState &state = tree.QueryExpressionState(state_index);
    state.cached_tick = tick_index;
    state.cached_command_execution_key = UINT64_MAX;
    state.cached_command_ordered = false;
    state.cached_result = result;
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

class PositionedGameObject {
 public:
  explicit PositionedGameObject(const MT_Vector3 &position = MT_Vector3(0.0f, 0.0f, 0.0f))
      : node(&object, nullptr, callbacks)
  {
    node.SetParentRelation(new TestRootParentRelation());
    object.SetSGNode(&node);
    SetPosition(position);
  }

  ~PositionedGameObject()
  {
    object.SetSGNode(nullptr);
  }

  void SetPosition(const MT_Vector3 &position)
  {
    object.NodeSetWorldPosition(position);
    object.NodeUpdateGS(0.0);
  }

  SG_Callbacks callbacks;
  KX_GameObject object;
  SG_Node node;
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

uint32_t AddPersistenceCompletion(LN_Program &program)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::PersistenceCompleted;
  return program.AddBoolExpression(expression);
}

uint32_t AddPersistenceSuccess(LN_Program &program, const uint32_t completion_index)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::PersistenceSucceeded;
  expression.input0 = completion_index;
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
  LN_CurveTable table;
  for (int sample_index = 0; sample_index < LN_CURVE_SAMPLE_COUNT; sample_index++) {
    const float factor = float(sample_index) / float(LN_CURVE_SAMPLE_COUNT - 1);
    table.samples[size_t(sample_index)] = factor * end_value;
  }
  return program.AddCurveTable(table);
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

uint32_t AddFlowGateState(LN_Program &program, const uint32_t start_open_index)
{
  LN_BoolExpression gate;
  gate.kind = LN_BoolExpressionKind::FlowGateOpen;
  gate.input0 = start_open_index;
  return program.AddBoolExpression(gate);
}

void AddFlowGateControl(LN_Program &program,
                        const uint32_t gate_index,
                        const uint32_t guard_index,
                        const LN_FlowGateControl control,
                        const uint32_t loop_frame_index = LN_INVALID_INDEX)
{
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetFlowGate;
  instruction.bool_guard_expr_index = guard_index;
  instruction.int_value = int32_t(gate_index);
  instruction.secondary_int_value = int32_t(control);
  instruction.loop_frame_index = loop_frame_index;
  program.AddInstruction(LN_Event::OnFixedUpdate, instruction);
}

uint32_t AddFlowGateAttempt(LN_Program &program,
                            const uint32_t gate_index,
                            const uint32_t guard_index,
                            const uint32_t loop_frame_index = LN_INVALID_INDEX)
{
  LN_Instruction attempt;
  attempt.opcode = LN_OpCode::TryFlowGate;
  attempt.bool_guard_expr_index = guard_index;
  attempt.int_value = int32_t(gate_index);
  attempt.loop_frame_index = loop_frame_index;
  const uint32_t attempt_index = program.AddInstruction(LN_Event::OnFixedUpdate, attempt);

  LN_BoolExpression accepted;
  accepted.kind = LN_BoolExpressionKind::InstructionExecuted;
  accepted.input0 = attempt_index;
  return program.AddBoolExpression(accepted);
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

uint32_t AddIntTreePropertyExpression(LN_Program &program,
                                      const char *name,
                                      const int32_t default_value,
                                      uint32_t &r_property_ref_index)
{
  LN_Value value;
  value.type = LN_ValueType::Int;
  value.exists = true;
  value.int_value = default_value;

  LN_TreePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = LN_ValueType::Int;
  property_ref.default_value = value;
  r_property_ref_index = program.AddTreePropertyRef(property_ref);

  LN_IntExpression expression;
  expression.kind = LN_IntExpressionKind::RuntimeTreeProperty;
  expression.property_ref_index = r_property_ref_index;
  return program.AddIntExpression(expression);
}

bool SetIntTreeProperty(LN_RuntimeTree &runtime_tree,
                        const uint32_t property_ref_index,
                        const int32_t value)
{
  LN_Value property_value;
  property_value.type = LN_ValueType::Int;
  property_value.exists = true;
  property_value.int_value = value;
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

void AddIntPrint(LN_Program &program, const uint32_t int_expression_index)
{
  LN_ValueExpression value;
  value.kind = LN_ValueExpressionKind::FromInt;
  value.input0 = int_expression_index;

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
                                        const float curve_end_value = 1.0f,
                                        const bool ignore_timescale = false)
{
  TweenExpressionRefs refs;

  LN_BoolExpression tween;
  tween.kind = LN_BoolExpressionKind::TweenValue;
  tween.input0 = forward_index;
  tween.input1 = back_index;
  tween.input2 = AddBoolConstant(program, ignore_timescale);
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

std::shared_ptr<LN_Program> CreateMouseLookInputOnlyProgram(const bool center_mouse,
                                                            const float smoothing = 0.0f)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t empty_object_expr = AddNoneValueConstant(*program);
  const uint32_t invert_expr = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  const uint32_t cap_x_expr = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  const uint32_t cap_y_expr = AddVectorConstant(*program,
                                                MT_Vector3(-1.57079637f, 1.57079637f, 0.0f));
  const uint32_t no_cap_expr = AddBoolConstant(*program, false);
  const uint32_t sensitivity_expr = AddFloatConstant(*program, 1000.0f);
  const uint32_t smoothing_expr = AddFloatConstant(*program, smoothing);

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
  program->AddInstruction(
      arm_event == LN_Event::OnUpdate ? LN_Event::OnUpdate : LN_Event::OnFixedUpdate, quit);
  return program;
}

std::shared_ptr<LN_Program> CreateWaitForNextFrameQuitProgram(const LN_Event arm_event)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();

  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::NextFrameDone;
  completed.int_value = int32_t(state_index);
  const uint32_t completed_index = program->AddBoolExpression(completed);

  LN_Instruction arm;
  arm.opcode = LN_OpCode::ArmNextFrame;
  arm.int_value = int32_t(state_index);
  program->AddInstruction(arm_event, arm);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = completed_index;
  program->AddInstruction(LN_Event::OnUpdate, quit);
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
  program->AddInstruction(
      arm_event == LN_Event::OnUpdate ? LN_Event::OnUpdate : LN_Event::OnFixedUpdate, quit);
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
  program->AddInstruction(update_event == LN_Event::OnFixedUpdate ? LN_Event::OnFixedUpdate :
                                                                    LN_Event::OnUpdate,
                          quit);
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

std::shared_ptr<LN_Program> CreateRegisterMouseMovedInputCommandProgram(
    const bool each_logic_tick = false, const LN_Event event = LN_Event::OnInit)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression mouse_moved;
  mouse_moved.kind = LN_BoolExpressionKind::MouseMoved;
  mouse_moved.bool_value = each_logic_tick;
  const uint32_t mouse_moved_index = program->AddBoolExpression(mouse_moved);

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetVisibility;
  instruction.bool_expr_index = mouse_moved_index;
  program->AddInstruction(event, instruction);

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

  LN_Instruction set_game_paused;
  set_game_paused.opcode = LN_OpCode::SetGamePaused;
  set_game_paused.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_game_paused);

  LN_Instruction set_physics_paused;
  set_physics_paused.opcode = LN_OpCode::SetPhysicsPaused;
  set_physics_paused.bool_expr_index = AddBoolConstant(*program, false);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_physics_paused);

  LN_Instruction load_blend_file;
  load_blend_file.opcode = LN_OpCode::LoadBlendFile;
  load_blend_file.string_expr_index = AddStringConstant(*program, "//next_level.blend");
  program->AddInstruction(LN_Event::OnFixedUpdate, load_blend_file);

  LN_Instruction save_game;
  save_game.opcode = LN_OpCode::SaveGame;
  save_game.int_expr_index = AddIntConstant(*program, 2);
  save_game.string_expr_index = AddStringConstant(*program, "//slot2.sav");
  save_game.bool_expr_index = AddPersistenceCompletion(*program);
  program->AddInstruction(LN_Event::OnFixedUpdate, save_game);

  LN_Instruction set_save_existence;
  set_save_existence.opcode = LN_OpCode::SetSaveExistence;
  set_save_existence.bool_expr_index = AddBoolConstant(*program, false);
  set_save_existence.secondary_bool_expr_index = AddPersistenceCompletion(*program);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_save_existence);

  LN_Instruction load_game;
  load_game.opcode = LN_OpCode::LoadGame;
  load_game.int_expr_index = AddIntConstant(*program, 3);
  load_game.string_expr_index = AddStringConstant(*program, "//slot3.sav");
  load_game.bool_expr_index = AddPersistenceCompletion(*program);
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

  const float divisors[4] = {2.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0e-25f};
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
  vector.input2 = divide_indices[2];
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
    const LN_QueryExpressionKind query_kind = LN_QueryExpressionKind::ProjectileRay)
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

void ExpectRotationEulerNear(const LN_Rotation &actual, const MT_Vector3 &expected)
{
  EXPECT_LE(actual.AngleTo(LN_Rotation::FromEulerXYZ(expected)), 0.0001f);
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
  EXPECT_LE(actual.rotation_value.AngleTo(expected.rotation_value), 0.0001f);
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
  EXPECT_LE(actual.rotation_value.AngleTo(expected.rotation_value), 0.0001f);
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

TEST(LN_RuntimeTree, MixValueHandlesEverySupportedTypeAndEdgeCase)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  auto add_value = [&](const LN_Value &value) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::Constant;
    expression.value = value;
    return program->AddValueExpression(expression);
  };
  auto add_mix = [&](const uint32_t a, const uint32_t b, const float factor) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::Mix;
    expression.input0 = a;
    expression.input1 = b;
    expression.input2 = AddFloatConstant(*program, factor);
    return program->AddValueExpression(expression);
  };
  auto make_int = [](const int32_t number) {
    LN_Value value;
    value.type = LN_ValueType::Int;
    value.exists = true;
    value.int_value = number;
    return value;
  };
  auto make_float = [](const float number) {
    LN_Value value;
    value.type = LN_ValueType::Float;
    value.exists = true;
    value.float_value = number;
    return value;
  };
  auto make_vector = [](const MT_Vector3 &vector) {
    LN_Value value;
    value.type = LN_ValueType::Vector;
    value.exists = true;
    value.vector_value = vector;
    return value;
  };
  auto make_vector4 = [](const MT_Vector4 &vector) {
    LN_Value value;
    value.type = LN_ValueType::Vector4;
    value.exists = true;
    value.vector4_value = vector;
    return value;
  };
  auto make_color = [](const MT_Vector4 &color) {
    LN_Value value;
    value.type = LN_ValueType::Color;
    value.exists = true;
    value.color_value = color;
    return value;
  };
  auto make_rotation = [](const MT_Vector3 &euler) {
    LN_Value value;
    value.type = LN_ValueType::Rotation;
    value.exists = true;
    value.SetRotation(LN_Rotation::FromEulerXYZ(euler), euler);
    return value;
  };

  const uint32_t float_a = add_value(make_float(10.0f));
  const uint32_t float_b = add_value(make_float(30.0f));
  const uint32_t float_mix = add_mix(float_a, float_b, 0.25f);
  const uint32_t extrapolated_mix = add_mix(float_a, float_b, 1.5f);
  const uint32_t negative_extrapolated_mix = add_mix(float_a, float_b, -0.5f);
  const uint32_t nan_factor_mix = add_mix(
      float_a, float_b, std::numeric_limits<float>::quiet_NaN());
  const uint32_t positive_infinity_factor_mix = add_mix(
      float_a, float_b, std::numeric_limits<float>::infinity());
  const uint32_t negative_infinity_factor_mix = add_mix(
      float_a, float_b, -std::numeric_limits<float>::infinity());

  const uint32_t int_a = add_value(make_int(0));
  const uint32_t int_b = add_value(make_int(5));
  const uint32_t int_mix = add_mix(int_a, int_b, 0.5f);
  const uint32_t negative_int_mix = add_mix(int_a, add_value(make_int(-5)), 0.5f);
  const uint32_t saturated_int_mix = add_mix(
      int_a, add_value(make_int(std::numeric_limits<int32_t>::max())), 2.0f);
  const uint32_t saturated_negative_int_mix = add_mix(
      int_a, add_value(make_int(std::numeric_limits<int32_t>::min())), 2.0f);
  const uint32_t promoted_mix = add_mix(
      add_value(make_int(10)), add_value(make_float(20.0f)), 0.25f);
  const uint32_t reverse_promoted_mix = add_mix(
      add_value(make_float(20.0f)), add_value(make_int(10)), 0.25f);

  const uint32_t vector_mix = add_mix(add_value(make_vector(MT_Vector3(0.0f, 2.0f, 4.0f))),
                                      add_value(make_vector(MT_Vector3(2.0f, 4.0f, 8.0f))),
                                      0.5f);
  const uint32_t vector4_mix = add_mix(
      add_value(make_vector4(MT_Vector4(0.0f, 2.0f, 4.0f, 6.0f))),
      add_value(make_vector4(MT_Vector4(2.0f, 4.0f, 8.0f, 10.0f))),
      0.5f);
  const uint32_t color_mix = add_mix(add_value(make_color(MT_Vector4(0.0f, 0.2f, 0.4f, 0.5f))),
                                     add_value(make_color(MT_Vector4(1.0f, 0.6f, 0.8f, 1.0f))),
                                     0.5f);

  constexpr float radians_170 = 2.9670597283903604f;
  constexpr float radians_180 = 3.1415926535897932f;
  const uint32_t rotation_mix = add_mix(
      add_value(make_rotation(MT_Vector3(0.0f, 0.0f, radians_170))),
      add_value(make_rotation(MT_Vector3(0.0f, 0.0f, -radians_170))),
      0.5f);
  const uint32_t rotation_extrapolated_mix = add_mix(
      add_value(make_rotation(MT_Vector3(0.0f, 0.0f, radians_170))),
      add_value(make_rotation(MT_Vector3(0.0f, 0.0f, -radians_170))),
      1.5f);

  constexpr float tau = 6.2831853071795865f;
  const MT_Vector3 authored_rotation_a(tau, -2.0f * tau, 3.0f * tau);
  const MT_Vector3 authored_rotation_b(-2.0f * tau, 3.0f * tau, -4.0f * tau);
  const uint32_t authored_rotation_a_value = add_value(make_rotation(authored_rotation_a));
  const uint32_t authored_rotation_b_value = add_value(make_rotation(authored_rotation_b));
  const uint32_t rotation_a_endpoint_mix = add_mix(
      authored_rotation_a_value, authored_rotation_b_value, 0.0f);
  const uint32_t rotation_b_endpoint_mix = add_mix(
      authored_rotation_a_value, authored_rotation_b_value, 1.0f);
  const uint32_t rotation_non_finite_endpoint_mix = add_mix(
      authored_rotation_a_value,
      authored_rotation_b_value,
      std::numeric_limits<float>::quiet_NaN());

  LN_Value rotation_without_hint = make_rotation(MT_Vector3(0.25f, -0.5f, 0.75f));
  rotation_without_hint.rotation_euler_hint_valid = false;
  const uint32_t rotation_without_hint_endpoint_mix = add_mix(
      add_value(rotation_without_hint), authored_rotation_b_value, 0.0f);

  const uint32_t missing_mix = add_mix(AddNoneValueConstant(*program), float_b, 0.5f);
  LN_Value none_value;
  none_value.type = LN_ValueType::None;
  none_value.exists = true;
  const uint32_t explicit_none_mix = add_mix(add_value(none_value), float_b, 0.5f);
  const uint32_t mismatched_mix = add_mix(
      add_value(make_vector(MT_Vector3(1.0f, 2.0f, 3.0f))),
      add_value(make_color(MT_Vector4(1.0f, 2.0f, 3.0f, 1.0f))),
      0.5f);
  LN_Value bool_value;
  bool_value.type = LN_ValueType::Bool;
  bool_value.exists = true;
  bool_value.bool_value = true;
  const uint32_t unsupported_mix = add_mix(add_value(bool_value), add_value(bool_value), 0.5f);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_TRUE(std::any_of(ir.ops.begin(), ir.ops.end(), [](const LN_RegisterExpressionOp &op) {
    return op.kind == LN_RegisterExpressionOpKind::ValueMix;
  }));

  auto verify = [&](const bool use_register_evaluator) {
    LN_RuntimeTree tree(program, FakeGameObjectPointer(use_register_evaluator ? 286 : 285), 0, 0);
    LN_TickContext context;
    context.tick_index = 1;
    context.use_register_expression_evaluator = use_register_evaluator;
    auto resolve = [&](const uint32_t expression) {
      return LN_RuntimeTreeTestAccess::ResolveValue(tree, expression, context);
    };

    LN_Value result = resolve(float_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Float);
    EXPECT_FLOAT_EQ(result.float_value, 15.0f);
    EXPECT_FLOAT_EQ(resolve(extrapolated_mix).float_value, 40.0f);
    EXPECT_FLOAT_EQ(resolve(negative_extrapolated_mix).float_value, 0.0f);
    EXPECT_FLOAT_EQ(resolve(nan_factor_mix).float_value, 10.0f);
    EXPECT_FLOAT_EQ(resolve(positive_infinity_factor_mix).float_value, 10.0f);
    EXPECT_FLOAT_EQ(resolve(negative_infinity_factor_mix).float_value, 10.0f);

    result = resolve(int_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Int);
    EXPECT_EQ(result.int_value, 3);
    EXPECT_EQ(resolve(negative_int_mix).int_value, -3);
    EXPECT_EQ(resolve(saturated_int_mix).int_value, std::numeric_limits<int32_t>::max());
    EXPECT_EQ(resolve(saturated_negative_int_mix).int_value, std::numeric_limits<int32_t>::min());

    result = resolve(promoted_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Float);
    EXPECT_FLOAT_EQ(result.float_value, 12.5f);
    EXPECT_FLOAT_EQ(resolve(reverse_promoted_mix).float_value, 17.5f);

    result = resolve(vector_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Vector);
    ExpectVectorNear(result.vector_value, MT_Vector3(1.0f, 3.0f, 6.0f));

    result = resolve(vector4_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Vector4);
    ExpectColorNear(result.vector4_value, MT_Vector4(1.0f, 3.0f, 6.0f, 8.0f));

    result = resolve(color_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Color);
    ExpectColorNear(result.color_value, MT_Vector4(0.5f, 0.4f, 0.6f, 0.75f));

    result = resolve(rotation_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Rotation);
    EXPECT_LE(result.rotation_value.AngleTo(
                  LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, radians_180))),
              0.0001f);
    result = resolve(rotation_extrapolated_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_LE(result.rotation_value.AngleTo(
                  LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, -2.792526803190927f))),
              0.0001f);

    result = resolve(rotation_a_endpoint_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_EQ(result.type, LN_ValueType::Rotation);
    EXPECT_TRUE(result.rotation_euler_hint_valid);
    ExpectVectorNear(result.RotationEulerRadians(), authored_rotation_a);
    EXPECT_LE(result.rotation_value.AngleTo(LN_Rotation::FromEulerXYZ(authored_rotation_a)),
              0.0001f);

    result = resolve(rotation_b_endpoint_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_TRUE(result.rotation_euler_hint_valid);
    ExpectVectorNear(result.RotationEulerRadians(), authored_rotation_b);
    EXPECT_LE(result.rotation_value.AngleTo(LN_Rotation::FromEulerXYZ(authored_rotation_b)),
              0.0001f);

    result = resolve(rotation_non_finite_endpoint_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_TRUE(result.rotation_euler_hint_valid);
    ExpectVectorNear(result.RotationEulerRadians(), authored_rotation_a);

    result = resolve(rotation_without_hint_endpoint_mix);
    ASSERT_TRUE(result.exists);
    EXPECT_FALSE(result.rotation_euler_hint_valid);
    EXPECT_LE(result.rotation_value.AngleTo(rotation_without_hint.rotation_value), 0.0001f);

    EXPECT_FALSE(resolve(missing_mix).exists);
    EXPECT_FALSE(resolve(explicit_none_mix).exists);
    EXPECT_FALSE(resolve(mismatched_mix).exists);
    EXPECT_FALSE(resolve(unsupported_mix).exists);
  };

  verify(false);
  verify(true);
}

TEST(LN_RuntimeTree, FloatCurveMatchesCurveMappingSemanticsAndSanitizesEdgeCases)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  auto sampled_table = [](const auto &sample_function) {
    LN_CurveTable table;
    table.clip_output = false;
    for (int sample_index = 0; sample_index < LN_CURVE_SAMPLE_COUNT; sample_index++) {
      const float factor = float(sample_index) / float(LN_CURVE_SAMPLE_COUNT - 1);
      table.samples[size_t(sample_index)] = sample_function(factor);
    }
    return table;
  };
  auto add_curve = [&](const LN_CurveTable &table, const float value, const float factor) {
    LN_FloatExpression expression;
    expression.kind = LN_FloatExpressionKind::FloatCurve;
    expression.input0 = AddFloatConstant(*program, value);
    expression.input1 = AddFloatConstant(*program, factor);
    expression.int_value = int32_t(program->AddCurveTable(table));
    return program->AddFloatExpression(expression);
  };

  const LN_CurveTable square_table = sampled_table([](const float x) { return x * x; });
  const uint32_t full_influence = add_curve(square_table, 0.5f, 1.0f);
  const uint32_t half_influence = add_curve(square_table, 0.5f, 0.5f);
  const uint32_t zero_influence = add_curve(square_table, 0.5f, 0.0f);
  const uint32_t extrapolated_influence = add_curve(square_table, 0.5f, 2.0f);
  const uint32_t negative_influence = add_curve(square_table, 0.5f, -1.0f);
  const uint32_t non_finite_influence = add_curve(
      square_table, 0.5f, std::numeric_limits<float>::quiet_NaN());
  const uint32_t infinite_influence = add_curve(
      square_table, 0.5f, std::numeric_limits<float>::infinity());

  LN_CurveTable custom_domain = sampled_table(
      [](const float factor) { return 10.0f + factor * 10.0f; });
  custom_domain.domain_min = -2.0f;
  custom_domain.domain_max = 2.0f;
  custom_domain.sample_scale = float(LN_CURVE_SAMPLE_COUNT - 1) /
                               (custom_domain.domain_max - custom_domain.domain_min);
  custom_domain.table_min_x = custom_domain.domain_min;
  custom_domain.table_max_x = custom_domain.domain_max;
  const uint32_t custom_domain_midpoint = add_curve(custom_domain, 0.0f, 1.0f);

  LN_CurveTable linear_table = sampled_table([](const float x) { return x; });
  const uint32_t extrapolated_above = add_curve(linear_table, 2.0f, 1.0f);
  const uint32_t extrapolated_below = add_curve(linear_table, -1.0f, 1.0f);
  LN_CurveTable horizontal_extension = linear_table;
  horizontal_extension.extrapolate = false;
  const uint32_t held_above = add_curve(horizontal_extension, 2.0f, 1.0f);
  const uint32_t held_below = add_curve(horizontal_extension, -1.0f, 1.0f);
  const uint32_t full_influence_large_input = add_curve(
      horizontal_extension, std::numeric_limits<float>::max(), 1.0f);
  LN_CurveTable opposite_extremes = horizontal_extension;
  opposite_extremes.samples.back() = -std::numeric_limits<float>::max();
  const uint32_t cancellation_safe_mix = add_curve(
      opposite_extremes, std::numeric_limits<float>::max(), 0.5f);
  LN_CurveTable clipped_extension = linear_table;
  clipped_extension.clip_output = true;
  clipped_extension.clip_min = 0.0f;
  clipped_extension.clip_max = 1.0f;
  const uint32_t clipped_above = add_curve(clipped_extension, 2.0f, 1.0f);
  LN_CurveTable vertical_extension = linear_table;
  vertical_extension.extend_out_x = 0.0f;
  vertical_extension.extend_out_y = 1.0f;
  const uint32_t vertical_above = add_curve(vertical_extension, 2.0f, 1.0f);

  LN_CurveTable invalid_domain = square_table;
  invalid_domain.domain_max = invalid_domain.domain_min;
  const uint32_t invalid_domain_result = add_curve(invalid_domain, 0.5f, 1.0f);
  LN_CurveTable overflowing_domain = square_table;
  overflowing_domain.domain_min = -std::numeric_limits<float>::max();
  overflowing_domain.domain_max = std::numeric_limits<float>::max();
  const uint32_t overflowing_domain_result = add_curve(overflowing_domain, 0.5f, 1.0f);
  LN_CurveTable invalid_clip = square_table;
  invalid_clip.clip_output = true;
  invalid_clip.clip_min = std::numeric_limits<float>::quiet_NaN();
  invalid_clip.clip_max = 1.0f;
  const uint32_t invalid_clip_result = add_curve(invalid_clip, 0.5f, 1.0f);
  LN_CurveTable invalid_scale = square_table;
  invalid_scale.sample_scale = 0.0f;
  const uint32_t invalid_scale_result = add_curve(invalid_scale, 0.5f, 1.0f);
  LN_CurveTable invalid_endpoints = square_table;
  invalid_endpoints.table_max_x = std::numeric_limits<float>::quiet_NaN();
  const uint32_t invalid_endpoints_result = add_curve(invalid_endpoints, 0.5f, 1.0f);
  LN_CurveTable invalid_extension = linear_table;
  invalid_extension.extend_out_y = std::numeric_limits<float>::quiet_NaN();
  const uint32_t invalid_extension_result = add_curve(invalid_extension, 2.0f, 1.0f);

  LN_CurveTable corrupt_sample = square_table;
  corrupt_sample.samples[LN_CURVE_SAMPLE_COUNT / 2] = std::numeric_limits<float>::quiet_NaN();
  const uint32_t corrupt_sample_result = add_curve(corrupt_sample, 0.5f, 1.0f);
  const uint32_t non_finite_input = add_curve(
      square_table, std::numeric_limits<float>::infinity(), 1.0f);
  const uint32_t overflowing_result = add_curve(
      horizontal_extension, std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  LN_FloatExpression missing_table_expression;
  missing_table_expression.kind = LN_FloatExpressionKind::FloatCurve;
  missing_table_expression.input0 = AddFloatConstant(*program, 0.5f);
  missing_table_expression.input1 = AddFloatConstant(*program, 1.0f);
  missing_table_expression.int_value = -1;
  const uint32_t missing_table = program->AddFloatExpression(missing_table_expression);

  std::vector<std::string> validation_errors;
  EXPECT_FALSE(program->ValidateInstructionPayloads(&validation_errors));
  EXPECT_TRUE(
      std::any_of(validation_errors.begin(), validation_errors.end(), [](const auto &error) {
        return error.find("curve table") != std::string::npos;
      }));
  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_TRUE(std::any_of(ir.ops.begin(), ir.ops.end(), [](const LN_RegisterExpressionOp &op) {
    return op.kind == LN_RegisterExpressionOpKind::FloatCurve;
  }));

  auto verify = [&](const bool use_register_evaluator) {
    LN_RuntimeTree tree(
        program, FakeGameObjectPointer(use_register_evaluator ? 0x28a : 0x289), 0, 0);
    LN_TickContext context;
    context.tick_index = 1;
    context.use_register_expression_evaluator = use_register_evaluator;
    auto resolve = [&](const uint32_t expression_index, const float fallback = -7.0f) {
      return LN_RuntimeTreeTestAccess::ResolveFloat(tree, expression_index, fallback, context);
    };

    EXPECT_FLOAT_EQ(resolve(full_influence), 0.25f);
    EXPECT_FLOAT_EQ(resolve(half_influence), 0.375f);
    EXPECT_FLOAT_EQ(resolve(zero_influence), 0.5f);
    EXPECT_FLOAT_EQ(resolve(extrapolated_influence), 0.0f);
    EXPECT_FLOAT_EQ(resolve(negative_influence), 0.75f);
    EXPECT_FLOAT_EQ(resolve(non_finite_influence), 0.5f);
    EXPECT_FLOAT_EQ(resolve(infinite_influence), 0.5f);
    EXPECT_FLOAT_EQ(resolve(custom_domain_midpoint), 15.0f);
    EXPECT_FLOAT_EQ(resolve(extrapolated_above), 2.0f);
    EXPECT_FLOAT_EQ(resolve(extrapolated_below), -1.0f);
    EXPECT_FLOAT_EQ(resolve(held_above), 1.0f);
    EXPECT_FLOAT_EQ(resolve(held_below), 0.0f);
    EXPECT_FLOAT_EQ(resolve(full_influence_large_input), 1.0f);
    EXPECT_FLOAT_EQ(resolve(cancellation_safe_mix), 0.0f);
    EXPECT_FLOAT_EQ(resolve(clipped_above), 1.0f);
    EXPECT_FLOAT_EQ(resolve(vertical_above), -9999.0f);
    EXPECT_FLOAT_EQ(resolve(invalid_domain_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(overflowing_domain_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(invalid_clip_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(invalid_scale_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(invalid_endpoints_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(invalid_extension_result), 2.0f);
    EXPECT_FLOAT_EQ(resolve(corrupt_sample_result), 0.5f);
    EXPECT_FLOAT_EQ(resolve(non_finite_input), 0.0f);
    EXPECT_FLOAT_EQ(resolve(overflowing_result), std::numeric_limits<float>::max());
    EXPECT_FLOAT_EQ(resolve(missing_table), 0.5f);
  };

  verify(false);
  verify(true);
}

TEST(LN_RuntimeTree, MixedVectorDrivesWorldOrientationThroughGenericRotationBoundary)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  auto add_vector_value = [&](const MT_Vector3 &vector) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::Constant;
    expression.value.type = LN_ValueType::Vector;
    expression.value.exists = true;
    expression.value.vector_value = vector;
    return program->AddValueExpression(expression);
  };

  LN_ValueExpression mix;
  mix.kind = LN_ValueExpressionKind::Mix;
  mix.input0 = add_vector_value(MT_Vector3(0.0f, 0.0f, -10.0f));
  mix.input1 = add_vector_value(MT_Vector3(0.0f, 0.0f, 5.0f));
  mix.input2 = AddFloatConstant(*program, 0.5f);

  LN_VectorExpression rotation;
  rotation.kind = LN_VectorExpressionKind::FromGenericValue;
  rotation.output_type = LN_ValueType::Rotation;
  rotation.input0 = program->AddValueExpression(mix);

  LN_Instruction set_rotation;
  set_rotation.opcode = LN_OpCode::SetTransformVector;
  set_rotation.vector_expr_index = program->AddVectorExpression(rotation);
  set_rotation.vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  set_rotation.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Orientation);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_rotation);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Rotation expected = LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, -2.5f));
  for (const bool use_register_evaluator : {false, true}) {
    LN_RuntimeTree runtime_tree(
        program, FakeGameObjectPointer(use_register_evaluator ? 0x287 : 0x286), 0, 0);
    LN_TickContext context;
    context.tick_index = 1;
    context.use_register_expression_evaluator = use_register_evaluator;
    const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                context);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetWorldOrientation);
    EXPECT_LE(commands[0].rotation_value.AngleTo(expected), 1.0e-5f);
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

TEST(LN_RuntimeTree, UpdateAndFixedUpdateExecuteOnlyTheirOwnInstructionStreams)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Instruction update_instruction;
  update_instruction.opcode = LN_OpCode::Print;
  update_instruction.value_expr_index = AddIntValueConstant(*program, 1);
  program->AddInstruction(LN_Event::OnUpdate, update_instruction);

  LN_Instruction fixed_instruction;
  fixed_instruction.opcode = LN_OpCode::Print;
  fixed_instruction.value_expr_index = AddIntValueConstant(*program, 2);
  program->AddInstruction(LN_Event::OnFixedUpdate, fixed_instruction);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(59), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.delta_time = 0.0;
  context.unscaled_dt = 1.0 / 60.0;

  context.event = LN_Event::OnUpdate;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "1");

  context.event = LN_Event::OnFixedUpdate;
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "2");
}

TEST(LN_RuntimeTree, WhilePausedATreeRunsOnlyItsUIEventRoutes)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_Instruction init_instruction;
  init_instruction.opcode = LN_OpCode::Print;
  init_instruction.value_expr_index = AddIntValueConstant(*program, 1);
  program->AddInstruction(LN_Event::OnInit, init_instruction);
  LN_Instruction ui_instruction;
  ui_instruction.opcode = LN_OpCode::Print;
  ui_instruction.value_expr_index = AddIntValueConstant(*program, 2);
  ui_instruction.flags = LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE;
  program->AddInstruction(LN_Event::OnUpdate, ui_instruction);
  LN_Instruction game_instruction;
  game_instruction.opcode = LN_OpCode::Print;
  game_instruction.value_expr_index = AddIntValueConstant(*program, 3);
  program->AddInstruction(LN_Event::OnUpdate, game_instruction);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_TRUE(program->HasUIEventRoutes());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(62), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.event = LN_Event::OnUpdate;
  auto printed = [&](const uint64_t tick_index) {
    context.tick_index = tick_index;
    std::vector<std::string> messages;
    for (const LN_CommandBuffer::Command &command : RecordReadyCommands(runtime_tree, context)) {
      messages.push_back(command.property_name);
    }
    return messages;
  };

  /* Started while paused (a menu added then): On Init runs in full and the UI route works. */
  runtime_tree.SetUIEventRoutesOnly(true);
  EXPECT_EQ(printed(0), (std::vector<std::string>{"1", "2"}));
  runtime_tree.SetUIEventRoutesOnly(false);
  EXPECT_EQ(printed(1), (std::vector<std::string>{"2", "3"}));
  /* Paused: the UI event route runs, the rest of the frame logic waits. */
  runtime_tree.SetUIEventRoutesOnly(true);
  EXPECT_EQ(printed(2), (std::vector<std::string>{"2"}));
  runtime_tree.SetUIEventRoutesOnly(false);
  EXPECT_EQ(printed(3), (std::vector<std::string>{"2", "3"}));
}

TEST(LN_RuntimeTree, WhilePausedOnlyTimeFlowsAUIEventRouteStartedKeepTime)
{
  /* Clicked -> Wait for Next Frame -> Print, once from a UI route and once from game logic. */
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  auto add_next_frame = [&](const bool ui_route, const int value) {
    const uint32_t state_index = program->AddTimeFlowState();
    LN_BoolExpression done;
    done.kind = LN_BoolExpressionKind::NextFrameDone;
    done.int_value = int32_t(state_index);
    const uint32_t done_index = program->AddBoolExpression(done);
    LN_Instruction arm;
    arm.opcode = LN_OpCode::ArmNextFrame;
    arm.int_value = int32_t(state_index);
    arm.bool_guard_expr_index = AddBoolConstant(*program, true);
    LN_Instruction print;
    print.opcode = LN_OpCode::Print;
    print.value_expr_index = AddIntValueConstant(*program, value);
    print.bool_guard_expr_index = done_index;
    /* The continuation belongs to the route that armed it (compiler ContinuationPaths). */
    if (ui_route) {
      arm.flags = LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE;
      print.flags = LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE;
    }
    program->AddInstruction(LN_Event::OnUpdate, arm);
    program->AddInstruction(LN_Event::OnUpdate, print);
  };
  add_next_frame(true, 1);
  add_next_frame(false, 2);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(63), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.event = LN_Event::OnUpdate;
  auto printed = [&](const uint64_t tick_index) {
    context.tick_index = tick_index;
    context.phase_tick_index = tick_index;
    std::vector<std::string> messages;
    for (const LN_CommandBuffer::Command &command : RecordReadyCommands(runtime_tree, context)) {
      messages.push_back(command.property_name);
    }
    return messages;
  };

  /* Both armed before the pause. */
  EXPECT_TRUE(printed(0).empty());
  /* Paused: the UI route's Next Frame completes; the game logic one waits for the game. */
  runtime_tree.SetUIEventRoutesOnly(true);
  EXPECT_EQ(printed(1), (std::vector<std::string>{"1"}));
  EXPECT_EQ(printed(2), (std::vector<std::string>{"1"}));
  runtime_tree.SetUIEventRoutesOnly(false);
  EXPECT_EQ(printed(3), (std::vector<std::string>{"1", "2"}));
}

TEST(LN_RuntimeTree, PhaseAwareCanTickSkipsEmptyRecurringStreamsAfterInitialization)
{
  std::shared_ptr<LN_Program> update_program = std::make_shared<LN_Program>();
  LN_Instruction update_instruction;
  update_instruction.opcode = LN_OpCode::Print;
  update_instruction.value_expr_index = AddIntValueConstant(*update_program, 1);
  update_program->AddInstruction(LN_Event::OnUpdate, update_instruction);

  KX_GameObject update_object;
  LN_RuntimeTree update_tree(update_program, &update_object, 0, 0);
  LN_TickContext update_context;
  update_context.event = LN_Event::OnUpdate;
  update_context.use_fixed_timestep = true;
  RecordReadyCommands(update_tree, update_context);

  EXPECT_TRUE(update_tree.CanTick(LN_Event::OnUpdate));
  EXPECT_FALSE(update_tree.CanTick(LN_Event::OnFixedUpdate));

  std::shared_ptr<LN_Program> fixed_program = std::make_shared<LN_Program>();
  LN_Instruction fixed_instruction;
  fixed_instruction.opcode = LN_OpCode::Print;
  fixed_instruction.value_expr_index = AddIntValueConstant(*fixed_program, 2);
  fixed_program->AddInstruction(LN_Event::OnFixedUpdate, fixed_instruction);

  KX_GameObject fixed_object;
  LN_RuntimeTree fixed_tree(fixed_program, &fixed_object, 0, 0);
  RecordReadyCommands(fixed_tree, update_context);

  EXPECT_FALSE(fixed_tree.CanTick(LN_Event::OnUpdate));
  EXPECT_TRUE(fixed_tree.CanTick(LN_Event::OnFixedUpdate));
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

TEST(LN_RuntimeTree, PersistenceCompletionQueuesDoneAndTickAlignedSuccessResults)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t completion_index = AddPersistenceCompletion(*program);
  const uint32_t success_index = AddPersistenceSuccess(*program, completion_index);

  LN_Instruction print_done;
  print_done.opcode = LN_OpCode::Print;
  print_done.bool_guard_expr_index = completion_index;
  print_done.value_expr_index = AddIntValueConstant(*program, 1);
  program->AddInstruction(LN_Event::OnFixedUpdate, print_done);

  LN_Instruction print_success;
  print_success.opcode = LN_OpCode::Print;
  print_success.bool_guard_expr_index = success_index;
  print_success.value_expr_index = AddIntValueConstant(*program, 2);
  program->AddInstruction(LN_Event::OnFixedUpdate, print_success);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x71), 0, 0);
  LN_TickContext context;
  runtime_tree.CompletePersistenceOperation(completion_index, false);
  runtime_tree.CompletePersistenceOperation(completion_index, true);

  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  context.tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 2u);

  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, PersistenceCompletionRejectsUnrelatedBoolState)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t constant_index = AddBoolConstant(*program, false);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = constant_index;
  print.value_expr_index = AddIntValueConstant(*program, 1);
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x72), 0, 0);
  runtime_tree.CompletePersistenceOperation(constant_index, true);

  LN_TickContext context;
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, PrintRotationUsesEditorStyleDegrees)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_ValueExpression rotation;
  rotation.kind = LN_ValueExpressionKind::Constant;
  rotation.value.type = LN_ValueType::Rotation;
  rotation.value.exists = true;
  rotation.value.rotation_value = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.0f, 0.0f, float(M_PI / 6.0)));
  const uint32_t rotation_index = program->AddValueExpression(rotation);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = rotation_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x73), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "Rotation(deg)=(0, 0, 30)");
}

TEST(LN_RuntimeTree, PrintGetParentValueUsesOwnerAndReturnsDirectParent)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_ValueExpression owner;
  owner.kind = LN_ValueExpressionKind::OwnerObject;
  const uint32_t owner_index = program->AddValueExpression(owner);

  LN_ValueExpression parent;
  parent.kind = LN_ValueExpressionKind::ObjectParent;
  parent.input0 = owner_index;

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = program->AddValueExpression(parent);
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  KX_GameObject parent_object;
  KX_GameObject child_object;
  parent_object.SetName("Parent");
  child_object.SetName("Child");

  std::vector<LN_CommandBuffer::Command> commands;
  {
    SG_Callbacks callbacks;
    SG_Node parent_node(&parent_object, nullptr, callbacks);
    SG_Node child_node(&child_object, nullptr, callbacks);
    parent_object.SetSGNode(&parent_node);
    child_object.SetSGNode(&child_node);
    parent_node.AddChild(&child_node);

    LN_RuntimeTree runtime_tree(program, &child_object, 0, 0);
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.tick_index = 1;
    commands = RecordReadyCommands(runtime_tree, context);

    child_node.DisconnectFromParent();
    child_object.SetSGNode(nullptr);
    parent_object.SetSGNode(nullptr);
  }

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(commands[0].property_name, "Parent");
}

TEST(LN_RuntimeTree, PrintRotationPreservesAuthoredEulerTurnsAtGraphBoundary)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  constexpr float degrees_to_radians = float(M_PI / 180.0);
  const MT_Vector3 authored_euler(0.0f, 0.0f, 350.0f * degrees_to_radians);

  LN_ValueExpression rotation;
  rotation.kind = LN_ValueExpressionKind::Constant;
  rotation.value.type = LN_ValueType::Rotation;
  rotation.value.exists = true;
  rotation.value.SetRotation(LN_Rotation::FromEulerXYZ(authored_euler), authored_euler);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = program->AddValueExpression(rotation);
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x731), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "Rotation(deg)=(0, 0, 350)");
}

TEST(LN_RuntimeTree, BoneRotationToObjectRotationConvertsOnlyAtExplicitGraphBoundary)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_ValueExpression native_bone_value;
  native_bone_value.kind = LN_ValueExpressionKind::Constant;
  native_bone_value.value.type = LN_ValueType::Rotation;
  native_bone_value.value.exists = true;
  native_bone_value.value.SetRotation(LN_BlenderBoneBasisRotation(),
                                      LN_BlenderBoneBasisRotation().ToEulerXYZ());
  const uint32_t native_bone_value_index = program->AddValueExpression(native_bone_value);

  LN_VectorExpression native_bone_rotation;
  native_bone_rotation.kind = LN_VectorExpressionKind::FromGenericValue;
  native_bone_rotation.output_type = LN_ValueType::Rotation;
  native_bone_rotation.input0 = native_bone_value_index;
  const uint32_t native_bone_rotation_index = program->AddVectorExpression(native_bone_rotation);

  LN_VectorExpression object_rotation;
  object_rotation.kind = LN_VectorExpressionKind::BoneRotationToObjectRotation;
  object_rotation.output_type = LN_ValueType::Rotation;
  object_rotation.input0 = native_bone_rotation_index;
  const uint32_t object_rotation_index = program->AddVectorExpression(object_rotation);

  LN_ValueExpression printable_rotation;
  printable_rotation.kind = LN_ValueExpressionKind::FromRotation;
  printable_rotation.input0 = object_rotation_index;

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.value_expr_index = program->AddValueExpression(printable_rotation);
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x732), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;

  const LN_Rotation converted = LN_RuntimeTreeTestAccess::ResolveRotation(
      runtime_tree, object_rotation_index, LN_Rotation::Identity(), context);
  EXPECT_TRUE(converted.IsIdentity(1.0e-6f));

  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "Rotation(deg)=(0, 0, 0)");
}

TEST(LN_RuntimeTree, RotationTweenPreservesEulerModeAndUsesShortestQuaternionArc)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t duration_index = AddFloatConstant(*program, 1.0f);

  LN_BoolExpression tween;
  tween.kind = LN_BoolExpressionKind::TweenValue;
  tween.float_expr_index = duration_index;
  tween.bool_value = true;
  const uint32_t tween_index = program->AddBoolExpression(tween);

  constexpr float degrees_to_radians = float(M_PI / 180.0);
  LN_VectorExpression from;
  from.kind = LN_VectorExpressionKind::Constant;
  from.output_type = LN_ValueType::Rotation;
  from.vector_value = MT_Vector3(0.0f, 0.0f, 350.0f * degrees_to_radians);
  from.rotation_value = LN_Rotation::FromEulerXYZ(from.vector_value);
  const uint32_t from_index = program->AddVectorExpression(from);

  LN_VectorExpression to;
  to.kind = LN_VectorExpressionKind::Constant;
  to.output_type = LN_ValueType::Rotation;
  to.vector_value = MT_Vector3(0.0f, 0.0f, 10.0f * degrees_to_radians);
  to.rotation_value = LN_Rotation::FromEulerXYZ(to.vector_value);
  const uint32_t to_index = program->AddVectorExpression(to);

  LN_VectorExpression euler_result;
  euler_result.kind = LN_VectorExpressionKind::TweenRotationResult;
  euler_result.output_type = LN_ValueType::Rotation;
  euler_result.input0 = tween_index;
  euler_result.input1 = from_index;
  euler_result.input2 = to_index;
  euler_result.property_ref_index = 0;
  const uint32_t euler_result_index = program->AddVectorExpression(euler_result);

  LN_VectorExpression quaternion_result = euler_result;
  quaternion_result.property_ref_index = 1;
  const uint32_t quaternion_result_index = program->AddVectorExpression(quaternion_result);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x74), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  context.delta_time = 0.5;

  const LN_Rotation euler_rotation = LN_RuntimeTreeTestAccess::ResolveRotation(
      runtime_tree, euler_result_index, LN_Rotation::Identity(), context);
  const LN_Rotation quaternion_rotation = LN_RuntimeTreeTestAccess::ResolveRotation(
      runtime_tree, quaternion_result_index, LN_Rotation::Identity(), context);

  EXPECT_LE(euler_rotation.AngleTo(LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, float(M_PI)))),
            1.0e-4f);
  EXPECT_TRUE(quaternion_rotation.IsIdentity(1.0e-4f));
}

TEST(LN_RuntimeTree, RotationCacheNeverCapturesCallerFallback)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_VectorExpression unresolved;
  unresolved.kind = LN_VectorExpressionKind::FromGenericValue;
  unresolved.output_type = LN_ValueType::Rotation;
  const uint32_t unresolved_index = program->AddVectorExpression(unresolved);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x75), 0, 0);
  LN_TickContext context;
  context.tick_index = 7;
  const LN_Rotation first_fallback = LN_Rotation::FromEulerXYZ(MT_Vector3(0.2f, 0.0f, 0.0f));
  const LN_Rotation second_fallback = LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.4f, 0.0f));

  const LN_Rotation first = LN_RuntimeTreeTestAccess::ResolveRotation(
      runtime_tree, unresolved_index, first_fallback, context);
  const LN_Rotation second = LN_RuntimeTreeTestAccess::ResolveRotation(
      runtime_tree, unresolved_index, second_fallback, context);
  EXPECT_LE(first.AngleTo(first_fallback), 1.0e-6f);
  EXPECT_LE(second.AngleTo(second_fallback), 1.0e-6f);
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
  const_cast<LN_EventBus &>(manager.GetEventBus()).BeginFrame();

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

TEST(LN_RuntimeTree, ValueChangedToDoesNotFireWhenValueLeavesTarget)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Value initial_value;
  initial_value.type = LN_ValueType::Float;
  initial_value.exists = true;
  initial_value.float_value = 1.0f;

  LN_TreePropertyRef property_ref;
  property_ref.name = "watched";
  property_ref.value_type = LN_ValueType::Float;
  property_ref.default_value = initial_value;
  const uint32_t property_ref_index = program->AddTreePropertyRef(property_ref);

  LN_ValueExpression property_value;
  property_value.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  property_value.property_ref_index = property_ref_index;
  const uint32_t property_value_index = program->AddValueExpression(property_value);

  LN_BoolExpression changed_to;
  changed_to.kind = LN_BoolExpressionKind::ValueChangedTo;
  changed_to.input0 = property_value_index;
  changed_to.input1 = AddFloatValueConstant(*program, 1.0f);
  const uint32_t changed_to_index = program->AddBoolExpression(changed_to);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = changed_to_index;
  print.value_expr_index = property_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(651), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  LN_Value updated_value = initial_value;
  updated_value.float_value = 2.0f;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, updated_value));

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, ValueChangedReferenceFiresOnlyWhenIdentityChanges)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Value initial_value;
  initial_value.type = LN_ValueType::ObjectRef;
  initial_value.exists = true;
  initial_value.runtime_ref.kind = LN_RuntimeRefKind::Object;
  initial_value.runtime_ref.slot = 7;
  initial_value.runtime_ref.generation = 3;
  initial_value.reference_name = "First";

  LN_TreePropertyRef property_ref;
  property_ref.name = "watched_object";
  property_ref.value_type = LN_ValueType::ObjectRef;
  property_ref.default_value = initial_value;
  const uint32_t property_ref_index = program->AddTreePropertyRef(property_ref);

  LN_ValueExpression property_value;
  property_value.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  property_value.property_ref_index = property_ref_index;
  const uint32_t property_value_index = program->AddValueExpression(property_value);

  LN_BoolExpression changed;
  changed.kind = LN_BoolExpressionKind::ValueChanged;
  changed.input0 = property_value_index;
  const uint32_t changed_index = program->AddBoolExpression(changed);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = changed_index;
  print.value_expr_index = property_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(652), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  LN_Value changed_value = initial_value;
  changed_value.runtime_ref.slot = 8;
  changed_value.reference_name = "Second";
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, changed_value));

  context.tick_index = 3;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, ValueChangedCanIgnoreInitialNoneToValueTransition)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_TreePropertyRef property_ref;
  property_ref.name = "late_value";
  property_ref.value_type = LN_ValueType::Generic;
  const uint32_t property_ref_index = program->AddTreePropertyRef(property_ref);

  LN_ValueExpression property_value;
  property_value.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  property_value.property_ref_index = property_ref_index;
  const uint32_t property_value_index = program->AddValueExpression(property_value);

  LN_BoolExpression changed;
  changed.kind = LN_BoolExpressionKind::ValueChanged;
  changed.input0 = property_value_index;
  changed.bool_value = true;
  const uint32_t changed_index = program->AddBoolExpression(changed);

  LN_Instruction print;
  print.opcode = LN_OpCode::Print;
  print.bool_guard_expr_index = changed_index;
  print.value_expr_index = property_value_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(653), 0, 0);
  LN_Value missing_value;
  missing_value.type = LN_ValueType::ObjectRef;
  missing_value.exists = false;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, missing_value));

  LN_TickContext context;
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  LN_Value first_value;
  first_value.type = LN_ValueType::Int;
  first_value.exists = true;
  first_value.int_value = 10;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, first_value));

  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  LN_Value second_value = first_value;
  second_value.int_value = 11;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(property_ref_index, second_value));

  context.tick_index = 3;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
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

TEST(LN_RuntimeTree, RegisterExpressionEvaluatorRecordsEquivalentDirectTypecastCommands)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  auto add_property = [&](const char *name, const LN_Value &default_value) {
    LN_TreePropertyRef property_ref;
    property_ref.name = name;
    property_ref.value_type = default_value.type;
    property_ref.default_value = default_value;
    return program->AddTreePropertyRef(property_ref);
  };

  LN_Value bool_value;
  bool_value.type = LN_ValueType::Bool;
  bool_value.exists = true;
  bool_value.bool_value = true;
  const uint32_t bool_property = add_property("bool_value", bool_value);

  LN_Value int_value;
  int_value.type = LN_ValueType::Int;
  int_value.exists = true;
  int_value.int_value = 7;
  const uint32_t int_property = add_property("int_value", int_value);

  LN_Value float_value;
  float_value.type = LN_ValueType::Float;
  float_value.exists = true;
  float_value.float_value = 2.5f;
  const uint32_t float_property = add_property("float_value", float_value);

  LN_Value string_value;
  string_value.type = LN_ValueType::String;
  string_value.exists = true;
  string_value.string_value = "not a number";
  const uint32_t string_property = add_property("string_value", string_value);

  LN_Value vector_value;
  vector_value.type = LN_ValueType::Vector;
  vector_value.exists = true;
  vector_value.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  const uint32_t vector_property = add_property("vector_value", vector_value);

  LN_Value color_value;
  color_value.type = LN_ValueType::Color;
  color_value.exists = true;
  color_value.color_value = MT_Vector4(0.2f, 0.4f, 0.6f, 1.0f);
  const uint32_t color_property = add_property("color_value", color_value);

  LN_BoolExpression bool_source;
  bool_source.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
  bool_source.property_ref_index = bool_property;
  const uint32_t bool_source_index = program->AddBoolExpression(bool_source);

  LN_IntExpression int_source;
  int_source.kind = LN_IntExpressionKind::RuntimeTreeProperty;
  int_source.property_ref_index = int_property;
  const uint32_t int_source_index = program->AddIntExpression(int_source);

  LN_FloatExpression float_source;
  float_source.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
  float_source.property_ref_index = float_property;
  const uint32_t float_source_index = program->AddFloatExpression(float_source);

  LN_StringExpression string_source;
  string_source.kind = LN_StringExpressionKind::RuntimeTreeProperty;
  string_source.property_ref_index = string_property;
  const uint32_t string_source_index = program->AddStringExpression(string_source);

  LN_VectorExpression vector_source;
  vector_source.kind = LN_VectorExpressionKind::RuntimeTreeProperty;
  vector_source.property_ref_index = vector_property;
  const uint32_t vector_source_index = program->AddVectorExpression(vector_source);

  LN_ColorExpression color_source;
  color_source.kind = LN_ColorExpressionKind::RuntimeTreeProperty;
  color_source.property_ref_index = color_property;
  const uint32_t color_source_index = program->AddColorExpression(color_source);

  LN_IntExpression int_from_bool;
  int_from_bool.kind = LN_IntExpressionKind::FromBool;
  int_from_bool.input0 = bool_source_index;
  const uint32_t int_from_bool_index = program->AddIntExpression(int_from_bool);
  LN_Instruction set_group_from_bool;
  set_group_from_bool.opcode = LN_OpCode::SetCollisionGroup;
  set_group_from_bool.int_expr_index = int_from_bool_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_group_from_bool);

  LN_FloatExpression float_from_int;
  float_from_int.kind = LN_FloatExpressionKind::FromInt;
  float_from_int.input0 = int_source_index;
  const uint32_t float_from_int_index = program->AddFloatExpression(float_from_int);
  LN_Instruction set_scale_from_int;
  set_scale_from_int.opcode = LN_OpCode::SetTimeScale;
  set_scale_from_int.float_expr_index = float_from_int_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_scale_from_int);

  LN_IntExpression int_from_float;
  int_from_float.kind = LN_IntExpressionKind::FromFloat;
  int_from_float.input0 = float_source_index;
  int_from_float.float_to_int_mode = LN_FloatToIntMode::Round;
  const uint32_t int_from_float_index = program->AddIntExpression(int_from_float);
  LN_Instruction set_group_from_float;
  set_group_from_float.opcode = LN_OpCode::SetCollisionGroup;
  set_group_from_float.int_expr_index = int_from_float_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_group_from_float);

  LN_FloatExpression float_from_string;
  float_from_string.kind = LN_FloatExpressionKind::FromString;
  float_from_string.string_expr_index = string_source_index;
  const uint32_t float_from_string_index = program->AddFloatExpression(float_from_string);
  LN_Instruction set_scale_from_string;
  set_scale_from_string.opcode = LN_OpCode::SetTimeScale;
  set_scale_from_string.float_expr_index = float_from_string_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_scale_from_string);

  LN_VectorExpression vector_from_float;
  vector_from_float.kind = LN_VectorExpressionKind::BroadcastFloat;
  vector_from_float.float_expr_index = float_source_index;
  const uint32_t vector_from_float_index = program->AddVectorExpression(vector_from_float);
  LN_Instruction set_position_from_float;
  set_position_from_float.opcode = LN_OpCode::SetWorldPosition;
  set_position_from_float.vector_expr_index = vector_from_float_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_position_from_float);

  LN_StringExpression string_from_vector;
  string_from_vector.kind = LN_StringExpressionKind::FromVector;
  string_from_vector.input0 = vector_source_index;
  const uint32_t string_from_vector_index = program->AddStringExpression(string_from_vector);
  LN_ValueExpression printable_string;
  printable_string.kind = LN_ValueExpressionKind::FromString;
  printable_string.input0 = string_from_vector_index;
  const uint32_t printable_string_index = program->AddValueExpression(printable_string);
  LN_Instruction print_vector;
  print_vector.opcode = LN_OpCode::Print;
  print_vector.value_expr_index = printable_string_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, print_vector);

  LN_BoolExpression bool_from_color;
  bool_from_color.kind = LN_BoolExpressionKind::FromColor;
  bool_from_color.input0 = color_source_index;
  const uint32_t bool_from_color_index = program->AddBoolExpression(bool_from_color);
  LN_Instruction set_visibility_from_color;
  set_visibility_from_color.opcode = LN_OpCode::SetVisibility;
  set_visibility_from_color.bool_expr_index = bool_from_color_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_visibility_from_color);

  LN_VectorExpression vector_from_color;
  vector_from_color.kind = LN_VectorExpressionKind::FromColor;
  vector_from_color.input0 = color_source_index;
  const uint32_t vector_from_color_index = program->AddVectorExpression(vector_from_color);
  LN_Instruction set_position_from_color;
  set_position_from_color.opcode = LN_OpCode::SetWorldPosition;
  set_position_from_color.vector_expr_index = vector_from_color_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, set_position_from_color);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  ASSERT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_NE(ir.int_expression_registers[int_from_bool_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.float_expression_registers[float_from_int_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.int_expression_registers[int_from_float_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.float_expression_registers[float_from_string_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.vector_expression_registers[vector_from_float_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.string_expression_registers[string_from_vector_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.bool_expression_registers[bool_from_color_index], LN_INVALID_INDEX);
  EXPECT_NE(ir.vector_expression_registers[vector_from_color_index], LN_INVALID_INDEX);

  LN_TickContext reference_context;
  reference_context.tick_index = 1;
  reference_context.use_register_expression_evaluator = false;
  LN_RuntimeTree reference_tree(program, FakeGameObjectPointer(84), 0, 0);
  const std::vector<LN_CommandBuffer::Command> reference_commands = RecordReadyCommands(
      reference_tree, reference_context);

  LN_TickContext register_context = reference_context;
  register_context.use_register_expression_evaluator = true;
  LN_RuntimeTree register_tree(program, FakeGameObjectPointer(84), 0, 0);
  LN_CommandBuffer command_buffer;
  LN_RuntimeProfileCounters counters;
  command_buffer.BeginRecording();
  register_tree.ExecuteReady(command_buffer, register_context, &counters);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> register_commands =
      command_buffer.TakeRecordedCommands();

  ASSERT_EQ(register_commands.size(), reference_commands.size());
  ASSERT_EQ(register_commands.size(), 8u);
  for (size_t index = 0; index < register_commands.size(); index++) {
    SCOPED_TRACE(index);
    ExpectCommandEqual(register_commands[index], reference_commands[index]);
  }
  EXPECT_EQ(register_commands[0].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[1].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(register_commands[2].type, LN_CommandBuffer::CommandType::SetCollisionGroup);
  EXPECT_EQ(register_commands[3].type, LN_CommandBuffer::CommandType::SetTimeScale);
  EXPECT_EQ(register_commands[4].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(register_commands[5].type, LN_CommandBuffer::CommandType::Print);
  EXPECT_EQ(register_commands[6].type, LN_CommandBuffer::CommandType::SetVisibility);
  EXPECT_EQ(register_commands[7].type, LN_CommandBuffer::CommandType::SetWorldPosition);
  EXPECT_EQ(register_commands[0].int_value, 1);
  EXPECT_NEAR(register_commands[1].scalar_value, 7.0f, 0.0001f);
  EXPECT_EQ(register_commands[2].int_value, 3);
  EXPECT_EQ(register_commands[3].scalar_value, 0.0f);
  ExpectVectorNear(register_commands[4].vector_value, MT_Vector3(2.5f, 2.5f, 2.5f));
  EXPECT_EQ(register_commands[5].property_name, "(1, 2, 3)");
  EXPECT_TRUE(register_commands[6].bool_value);
  ExpectVectorNear(register_commands[7].vector_value, MT_Vector3(0.2f, 0.4f, 0.6f));
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
  rotation_constant.value.rotation_value = LN_Rotation::FromEulerXYZ(MT_Vector3(0.1f, 0.2f, 0.3f));
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

TEST(LN_RuntimeTree, InvalidOverlayRequestsCompleteFailedWithoutQueuing)
{
  const LN_OpCode overlay_opcodes[] = {
      LN_OpCode::SetOverlayCollection,
      LN_OpCode::RemoveOverlayCollection,
  };

  for (const LN_OpCode opcode : overlay_opcodes) {
    SCOPED_TRACE(static_cast<int>(opcode));
    std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
    const uint32_t none_value = AddNoneValueConstant(*program);

    LN_BoolExpression completed;
    completed.kind = LN_BoolExpressionKind::UICommandCompleted;
    const uint32_t completion_index = program->AddBoolExpression(completed);

    LN_Instruction overlay;
    overlay.opcode = opcode;
    overlay.value_expr_index = none_value;
    overlay.property_ref_index = completion_index;
    if (opcode == LN_OpCode::SetOverlayCollection) {
      overlay.secondary_value_expr_index = none_value;
    }
    program->AddInstruction(LN_Event::OnFixedUpdate, overlay);

    LN_BoolExpression succeeded;
    succeeded.kind = LN_BoolExpressionKind::UICommandSucceeded;
    succeeded.input0 = completion_index;

    LN_Instruction quit;
    quit.opcode = LN_OpCode::QuitGame;
    quit.bool_guard_expr_index = program->AddBoolExpression(succeeded);
    program->AddInstruction(LN_Event::OnFixedUpdate, quit);

    ASSERT_TRUE(program->ValidateInstructionPayloads());
    KX_GameObject owner;
    SG_Callbacks callbacks;
    SG_Node scene_node(&owner, nullptr, callbacks);
    owner.SetSGNode(&scene_node);
    {
      LN_RuntimeTree runtime_tree(program, &owner, 0, 0);
      LN_TickContext context;
      context.tick_index = 1;

      EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
    }
    owner.SetSGNode(nullptr);
  }
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

TEST(LN_RuntimeTree, FlowGateControlsPersistentStateAndFiltersEveryEnterPulse)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t enter_ref = LN_INVALID_INDEX;
  uint32_t toggle_ref = LN_INVALID_INDEX;
  uint32_t open_ref = LN_INVALID_INDEX;
  uint32_t close_ref = LN_INVALID_INDEX;
  const uint32_t enter = AddBoolTreePropertyExpression(*program, "enter", true, enter_ref);
  const uint32_t toggle = AddBoolTreePropertyExpression(*program, "toggle", false, toggle_ref);
  const uint32_t open = AddBoolTreePropertyExpression(*program, "open", false, open_ref);
  const uint32_t close = AddBoolTreePropertyExpression(*program, "close", false, close_ref);
  const uint32_t gate = AddFlowGateState(*program, AddBoolConstant(*program, false));

  AddFlowGateControl(*program, gate, toggle, LN_FlowGateControl::Toggle);
  AddFlowGateControl(*program, gate, open, LN_FlowGateControl::Open);
  AddFlowGateControl(*program, gate, close, LN_FlowGateControl::Close);
  AddGuardedPrint(*program, AddFlowGateAttempt(*program, gate, enter), 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(269), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, open_ref, true));
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, open_ref, false));
  context.tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, close_ref, true));
  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, close_ref, false));
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, toggle_ref, true));
  context.tick_index = 4;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  /* Controls execute before Enter, and compiler-defined Close > Open > Toggle precedence wins
   * deterministically if every control receives a pulse in the same execution scope. */
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, open_ref, true));
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, close_ref, true));
  context.tick_index = 5;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, FlowGateSamplesStartOpenOnceOnFirstEvaluationPerRuntimeTree)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t start_open_ref = LN_INVALID_INDEX;
  const uint32_t start_open = AddBoolTreePropertyExpression(
      *program, "start_open", true, start_open_ref);
  const uint32_t gate = AddFlowGateState(*program, start_open);
  AddGuardedPrint(
      *program, AddFlowGateAttempt(*program, gate, AddBoolConstant(*program, true)), 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree first_tree(program, FakeGameObjectPointer(270), 0, 0);
  LN_RuntimeTree second_tree(program, FakeGameObjectPointer(271), 1, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(first_tree, context).size(), 1u);

  ASSERT_TRUE(SetBoolTreeProperty(first_tree, start_open_ref, false));
  ASSERT_TRUE(SetBoolTreeProperty(second_tree, start_open_ref, false));
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(first_tree, context).size(), 1u);
  EXPECT_TRUE(RecordReadyCommands(second_tree, context).empty());
}

TEST(LN_RuntimeTree, FlowGateStateSurvivesDisableResume)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t always = AddBoolConstant(*program, true);
  uint32_t open_ref = LN_INVALID_INDEX;
  const uint32_t open = AddBoolTreePropertyExpression(*program, "open", true, open_ref);
  const uint32_t gate = AddFlowGateState(*program, AddBoolConstant(*program, false));
  AddFlowGateControl(*program, gate, open, LN_FlowGateControl::Open);
  AddGuardedPrint(*program, AddFlowGateAttempt(*program, gate, always), 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(275), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, open_ref, false));
  runtime_tree.SetEnabled(false);
  context.tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, FlowGatePreservesRepeatedTogglePulses)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t always = AddBoolConstant(*program, true);
  const uint32_t gate = AddFlowGateState(*program, always);
  AddFlowGateControl(*program, gate, always, LN_FlowGateControl::Toggle);
  AddFlowGateControl(*program, gate, always, LN_FlowGateControl::Toggle);
  AddGuardedPrint(*program, AddFlowGateAttempt(*program, gate, always), 1);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(272), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
  context.tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, FlowGateInstancesWithIdenticalInitialValuesKeepIndependentState)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t always = AddBoolConstant(*program, true);
  const uint32_t initially_closed = AddBoolConstant(*program, false);
  const uint32_t first_gate = AddFlowGateState(*program, initially_closed);
  const uint32_t second_gate = AddFlowGateState(*program, initially_closed);
  ASSERT_NE(first_gate, second_gate);

  AddFlowGateControl(*program, first_gate, always, LN_FlowGateControl::Open);
  AddGuardedPrint(*program, AddFlowGateAttempt(*program, first_gate, always), 1);
  AddGuardedPrint(*program, AddFlowGateAttempt(*program, second_gate, always), 2);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(273), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "1");
}

TEST(LN_RuntimeTree, FlowGateAcceptedPulseDoesNotLeakIntoLaterLoopScope)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t always = AddBoolConstant(*program, true);
  const uint32_t gate = AddFlowGateState(*program, AddBoolConstant(*program, false));

  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = always;
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

  LN_BoolExpression second_iteration = first_iteration;
  second_iteration.input1 = AddIntValueConstant(*program, 1);
  const uint32_t second_iteration_index = program->AddBoolExpression(second_iteration);

  AddFlowGateControl(*program, gate, first_iteration_index, LN_FlowGateControl::Open, frame_index);
  AddFlowGateControl(
      *program, gate, second_iteration_index, LN_FlowGateControl::Close, frame_index);
  const uint32_t accepted = AddFlowGateAttempt(*program, gate, always, frame_index);
  AddGuardedPrint(*program, accepted, 1, frame_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(274), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "1");
}

TEST(LN_RuntimeTree, FlowRouterCyclesResetsAndScopesEachRoutePulse)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const uint32_t state_index = program->AddFlowControlState();

  auto add_route_expression = [&](const LN_BoolExpressionKind kind, const int32_t route) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(state_index);
    expression.secondary_int_value = route;
    return program->AddBoolExpression(expression);
  };
  const uint32_t route_zero = add_route_expression(LN_BoolExpressionKind::FlowRouterSelected, 0);
  const uint32_t route_one = add_route_expression(LN_BoolExpressionKind::FlowRouterSelected, 1);
  const uint32_t route_two = add_route_expression(LN_BoolExpressionKind::FlowRouterSelected, 2);
  const uint32_t last_route_zero = add_route_expression(LN_BoolExpressionKind::FlowRouterLastRoute,
                                                        0);

  LN_Instruction reset_router;
  reset_router.opcode = LN_OpCode::ResetFlowRouter;
  reset_router.bool_guard_expr_index = reset;
  reset_router.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnFixedUpdate, reset_router);

  LN_Instruction route;
  route.opcode = LN_OpCode::TryFlowRouter;
  route.bool_guard_expr_index = flow;
  route.int_value = int32_t(state_index);
  route.secondary_int_value = 3;
  program->AddInstruction(LN_Event::OnFixedUpdate, route);

  AddGuardedPrint(*program, route_zero, 10);
  AddGuardedPrint(*program, route_one, 20);
  AddGuardedPrint(*program, route_two, 30);
  AddGuardedPrint(*program, last_route_zero, 40);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(276), 0, 0);
  LN_TickContext context;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "40");

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "20");

  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "30");

  context.tick_index = 3;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "40");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 4;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "40");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, false));
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 5;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "40");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 6;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, FlowRouterRoutesEachLoopIterationWithoutLeakingItsPulse)
{
  constexpr int32_t iteration_count = 300;
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, iteration_count);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_BoolExpression loop_active;
  loop_active.kind = LN_BoolExpressionKind::LoopActive;
  loop_active.int_value = int32_t(frame_index);
  frame.loop_active_bool_expr_index = program->AddBoolExpression(loop_active);
  program->UpdateLoopFrame(frame_index, frame);

  const uint32_t state_index = program->AddFlowControlState();
  auto add_route_expression = [&](const int32_t route_index) {
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::FlowRouterSelected;
    expression.int_value = int32_t(state_index);
    expression.secondary_int_value = route_index;
    return program->AddBoolExpression(expression);
  };
  const uint32_t route_a = add_route_expression(0);
  const uint32_t route_b = add_route_expression(1);

  LN_Instruction route;
  route.opcode = LN_OpCode::TryFlowRouter;
  route.bool_guard_expr_index = frame.loop_active_bool_expr_index;
  route.int_value = int32_t(state_index);
  route.secondary_int_value = 2;
  route.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, route);
  AddGuardedPrint(*program, route_a, 10, frame_index);
  AddGuardedPrint(*program, route_b, 20, frame_index);
  /* The final loop pulse must not make an unrelated event-scope branch true. */
  AddGuardedPrint(*program, route_b, 30);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(280), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  context.use_register_expression_evaluator = true;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), size_t(iteration_count));
  for (int32_t index = 0; index < iteration_count; index++) {
    EXPECT_EQ(commands[index].property_name, (index % 2 == 0) ? "10" : "20") << index;
  }
}

TEST(LN_RuntimeTree, FlipFlopAlternatesAndResetDefinesNextRouteAndStateOutput)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const uint32_t state_index = program->AddFlowControlState();

  auto add_route_expression = [&](const LN_BoolExpressionKind kind, const int32_t route) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(state_index);
    expression.secondary_int_value = route;
    return program->AddBoolExpression(expression);
  };
  const uint32_t route_a = add_route_expression(LN_BoolExpressionKind::FlowRouterSelected, 0);
  const uint32_t route_b = add_route_expression(LN_BoolExpressionKind::FlowRouterSelected, 1);
  const uint32_t is_a = add_route_expression(LN_BoolExpressionKind::FlowRouterLastRoute, 0);

  LN_Instruction reset_flip_flop;
  reset_flip_flop.opcode = LN_OpCode::ResetFlowRouter;
  reset_flip_flop.bool_guard_expr_index = reset;
  reset_flip_flop.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnFixedUpdate, reset_flip_flop);

  LN_Instruction select_route;
  select_route.opcode = LN_OpCode::TryFlowRouter;
  select_route.bool_guard_expr_index = flow;
  select_route.int_value = int32_t(state_index);
  select_route.secondary_int_value = 2;
  program->AddInstruction(LN_Event::OnFixedUpdate, select_route);

  AddGuardedPrint(*program, route_a, 10);
  AddGuardedPrint(*program, route_b, 20);
  AddGuardedPrint(*program, is_a, 30);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(281), 0, 0);
  LN_TickContext context;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "30");

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "20");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "30");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, RepeatLimitSamplesTheCurrentLimitAndResetsBeforeAccepting)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  uint32_t reset_ref = LN_INVALID_INDEX;
  uint32_t limit_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  const uint32_t reset = AddBoolTreePropertyExpression(*program, "reset", false, reset_ref);
  const uint32_t limit = AddIntTreePropertyExpression(*program, "limit", 2, limit_ref);
  const uint32_t state_index = program->AddFlowControlState();

  auto add_state_expression = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(state_index);
    expression.input0 = kind == LN_BoolExpressionKind::RepeatLimitExhausted ? limit :
                                                                              LN_INVALID_INDEX;
    return program->AddBoolExpression(expression);
  };
  const uint32_t accepted = add_state_expression(LN_BoolExpressionKind::RepeatLimitAccepted);
  const uint32_t blocked = add_state_expression(LN_BoolExpressionKind::RepeatLimitBlocked);
  const uint32_t exhausted = add_state_expression(LN_BoolExpressionKind::RepeatLimitExhausted);

  LN_IntExpression remaining_expression;
  remaining_expression.kind = LN_IntExpressionKind::RepeatLimitRemaining;
  remaining_expression.int_value = int32_t(state_index);
  remaining_expression.input0 = limit;
  const uint32_t remaining = program->AddIntExpression(remaining_expression);

  LN_Instruction reset_limit;
  reset_limit.opcode = LN_OpCode::ResetRepeatLimit;
  reset_limit.bool_guard_expr_index = reset;
  reset_limit.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnFixedUpdate, reset_limit);

  LN_Instruction attempt;
  attempt.opcode = LN_OpCode::TryRepeatLimit;
  attempt.bool_guard_expr_index = flow;
  attempt.int_value = int32_t(state_index);
  attempt.int_expr_index = limit;
  program->AddInstruction(LN_Event::OnFixedUpdate, attempt);

  AddGuardedPrint(*program, accepted, 1);
  AddGuardedPrint(*program, blocked, 2);
  AddGuardedPrint(*program, exhausted, 3);
  AddIntPrint(*program, remaining);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(277), 0, 0);
  LN_TickContext context;

  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "1");

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "3");
  EXPECT_EQ(commands[2].property_name, "0");

  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "2");
  EXPECT_EQ(commands[1].property_name, "3");
  EXPECT_EQ(commands[2].property_name, "0");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, true));
  context.tick_index = 3;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "1");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, reset_ref, false));
  ASSERT_TRUE(SetIntTreeProperty(runtime_tree, limit_ref, 0));
  context.tick_index = 4;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "2");
  EXPECT_EQ(commands[1].property_name, "3");
  EXPECT_EQ(commands[2].property_name, "0");

  ASSERT_TRUE(SetIntTreeProperty(runtime_tree, limit_ref, 5));
  context.tick_index = 5;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "3");
}

TEST(LN_RuntimeTree, BreakLoopStopsTheCurrentBodyAndStillEmitsCompleted)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 5);
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

  AddGuardedPrint(*program, AddBoolConstant(*program, true), 1, frame_index);
  LN_Instruction break_loop;
  break_loop.opcode = LN_OpCode::BreakLoop;
  break_loop.bool_guard_expr_index = AddBoolConstant(*program, true);
  break_loop.int_value = int32_t(frame_index);
  break_loop.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, break_loop);
  AddGuardedPrint(*program, AddBoolConstant(*program, true), 2, frame_index);

  LN_Instruction completed;
  completed.opcode = LN_OpCode::Print;
  completed.bool_guard_expr_index = AddBoolConstant(*program, true);
  completed.value_expr_index = AddIntValueConstant(*program, 3);
  completed.completed_loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, completed);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(278), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "3");

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "3");

  std::shared_ptr<LN_Program> zero_iteration_program = std::make_shared<LN_Program>();
  LN_LoopFrame zero_frame;
  zero_frame.kind = LN_LoopKind::Count;
  zero_frame.trigger_bool_expr_index = AddBoolConstant(*zero_iteration_program, true);
  zero_frame.count_int_expr_index = AddIntConstant(*zero_iteration_program, 0);
  const uint32_t zero_frame_index = zero_iteration_program->AddLoopFrame(zero_frame);

  LN_Instruction body;
  body.opcode = LN_OpCode::Nop;
  body.loop_frame_index = zero_frame_index;
  zero_iteration_program->AddInstruction(LN_Event::OnFixedUpdate, body);

  LN_Instruction zero_completed;
  zero_completed.opcode = LN_OpCode::Print;
  zero_completed.value_expr_index = AddIntValueConstant(*zero_iteration_program, 4);
  zero_completed.completed_loop_frame_index = zero_frame_index;
  zero_iteration_program->AddInstruction(LN_Event::OnFixedUpdate, zero_completed);
  ASSERT_TRUE(zero_iteration_program->ValidateInstructionPayloads());

  LN_RuntimeTree zero_iteration_tree(zero_iteration_program, FakeGameObjectPointer(279), 0, 0);
  context.tick_index = 0;
  commands = RecordReadyCommands(zero_iteration_tree, context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "4");
}

TEST(LN_RuntimeTree, BatchedCountLoopDistributesWorkAndCompletesOnTheFinalTick)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  frame.count_int_expr_index = AddIntConstant(*program, 10);
  frame.ticks_to_complete_int_expr_index = AddIntConstant(*program, 3);
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

  LN_Instruction body;
  body.opcode = LN_OpCode::Print;
  body.bool_guard_expr_index = AddBoolConstant(*program, true);
  body.value_expr_index = frame.loop_current_value_expr_index;
  body.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, body);

  LN_Instruction completed;
  completed.opcode = LN_OpCode::Print;
  completed.value_expr_index = AddIntValueConstant(*program, 99);
  completed.completed_loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, completed);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(280), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_EQ(commands[0].property_name, "0");
  EXPECT_EQ(commands[3].property_name, "3");

  /* The trigger is sampled only when the loop starts. Its later value does not cancel the job. */
  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "4");
  EXPECT_EQ(commands[2].property_name, "6");

  context.tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_EQ(commands[0].property_name, "7");
  EXPECT_EQ(commands[2].property_name, "9");
  EXPECT_EQ(commands[3].property_name, "99");

  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, BatchedLoopClampsInvalidTicksAndAvoidsEmptyBatches)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t ticks_ref = LN_INVALID_INDEX;
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 2);
  frame.ticks_to_complete_int_expr_index = AddIntTreePropertyExpression(
      *program, "ticks", 10, ticks_ref);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  LN_Instruction body;
  body.opcode = LN_OpCode::Print;
  body.bool_guard_expr_index = AddBoolConstant(*program, true);
  body.value_expr_index = frame.loop_current_value_expr_index;
  body.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, body);

  LN_Instruction completed;
  completed.opcode = LN_OpCode::Print;
  completed.value_expr_index = AddIntValueConstant(*program, 99);
  completed.completed_loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, completed);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext context;
  context.tick_index = 0;
  LN_RuntimeTree over_requested_tree(program, FakeGameObjectPointer(284), 0, 0);
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(over_requested_tree,
                                                                        context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].property_name, "0");

  context.tick_index = 1;
  commands = RecordReadyCommands(over_requested_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "1");
  EXPECT_EQ(commands[1].property_name, "99");

  LN_RuntimeTree invalid_ticks_tree(program, FakeGameObjectPointer(285), 0, 0);
  ASSERT_TRUE(SetIntTreeProperty(invalid_ticks_tree, ticks_ref, -5));
  context.tick_index = 0;
  commands = RecordReadyCommands(invalid_ticks_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "0");
  EXPECT_EQ(commands[1].property_name, "1");
  EXPECT_EQ(commands[2].property_name, "99");
}

TEST(LN_RuntimeTree, SynchronousLoopCarriesExcessSafetyBudgetWorkWithoutDroppingIterations)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolConstant(*program, true);
  frame.count_int_expr_index = AddIntConstant(*program, 10001);
  frame.ticks_to_complete_int_expr_index = AddIntConstant(*program, 1);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  LN_Instruction body;
  body.opcode = LN_OpCode::Print;
  body.bool_guard_expr_index = AddBoolConstant(*program, true);
  body.value_expr_index = frame.loop_current_value_expr_index;
  body.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, body);

  LN_Instruction completed;
  completed.opcode = LN_OpCode::Print;
  completed.value_expr_index = AddIntValueConstant(*program, 99);
  completed.completed_loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, completed);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(304), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 10000u);
  EXPECT_EQ(commands.front().property_name, "0");
  EXPECT_EQ(commands.back().property_name, "9999");

  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10000");
  EXPECT_EQ(commands[1].property_name, "99");
}

TEST(LN_RuntimeTree, BatchedListLoopSnapshotsItemsAndIgnoresOverlappingTriggers)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  const uint32_t flow = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);

  LN_Value initial_list;
  initial_list.type = LN_ValueType::List;
  initial_list.exists = true;
  for (const int32_t item : {10, 20, 30, 40}) {
    LN_Value value;
    value.type = LN_ValueType::Int;
    value.exists = true;
    value.int_value = item;
    initial_list.list_value.push_back(value);
  }
  LN_TreePropertyRef list_ref;
  list_ref.name = "items";
  list_ref.value_type = LN_ValueType::List;
  list_ref.default_value = initial_list;
  const uint32_t list_ref_index = program->AddTreePropertyRef(list_ref);

  LN_ValueExpression list_expression;
  list_expression.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
  list_expression.property_ref_index = list_ref_index;
  const uint32_t list_expression_index = program->AddValueExpression(list_expression);

  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::FromList;
  frame.trigger_bool_expr_index = flow;
  frame.list_value_expr_index = list_expression_index;
  frame.ticks_to_complete_int_expr_index = AddIntConstant(*program, 2);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  LN_Instruction body;
  body.opcode = LN_OpCode::Print;
  body.bool_guard_expr_index = AddBoolConstant(*program, true);
  body.value_expr_index = frame.loop_current_value_expr_index;
  body.loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, body);

  LN_Instruction completed;
  completed.opcode = LN_OpCode::Print;
  completed.value_expr_index = AddIntValueConstant(*program, 99);
  completed.completed_loop_frame_index = frame_index;
  program->AddInstruction(LN_Event::OnFixedUpdate, completed);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(281), 0, 0);
  LN_TickContext context;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "10");
  EXPECT_EQ(commands[1].property_name, "20");

  LN_Value replacement_list;
  replacement_list.type = LN_ValueType::List;
  replacement_list.exists = true;
  replacement_list.list_value.push_back(initial_list.list_value.front());
  replacement_list.list_value.front().int_value = 777;
  ASSERT_TRUE(runtime_tree.SetTreePropertyValue(list_ref_index, replacement_list));

  /* Flow remains true, but the overlapping pulse and replacement list are ignored while active. */
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "30");
  EXPECT_EQ(commands[1].property_name, "40");
  EXPECT_EQ(commands[2].property_name, "99");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, OnInitBatchedLoopDoesNotConsumeTwoBatchesOnItsFirstUpdate)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t flow_ref = LN_INVALID_INDEX;
  LN_LoopFrame frame;
  frame.kind = LN_LoopKind::Count;
  frame.trigger_bool_expr_index = AddBoolTreePropertyExpression(*program, "flow", true, flow_ref);
  frame.count_int_expr_index = AddIntConstant(*program, 4);
  frame.ticks_to_complete_int_expr_index = AddIntConstant(*program, 2);
  const uint32_t frame_index = program->AddLoopFrame(frame);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program->AddValueExpression(current_value);
  program->UpdateLoopFrame(frame_index, frame);

  auto add_loop_routes = [&](const LN_Event event) {
    LN_Instruction body;
    body.opcode = LN_OpCode::Print;
    body.bool_guard_expr_index = AddBoolConstant(*program, true);
    body.value_expr_index = frame.loop_current_value_expr_index;
    body.loop_frame_index = frame_index;
    program->AddInstruction(event, body);

    LN_Instruction completed;
    completed.opcode = LN_OpCode::Print;
    completed.value_expr_index = AddIntValueConstant(*program, 99);
    completed.completed_loop_frame_index = frame_index;
    program->AddInstruction(event, completed);
  };
  add_loop_routes(LN_Event::OnInit);
  add_loop_routes(LN_Event::OnUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(282), 0, 0);
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[0].property_name, "0");
  EXPECT_EQ(commands[1].property_name, "1");

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.tick_index = 1;
  commands = RecordReadyCommands(runtime_tree, context);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[0].property_name, "2");
  EXPECT_EQ(commands[1].property_name, "3");
  EXPECT_EQ(commands[2].property_name, "99");
}

TEST(LN_RuntimeTree, MapRangeSafeDivideMatchesBlenderForTinyAndZeroSourceRanges)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  uint32_t numerator_ref = LN_INVALID_INDEX;
  uint32_t divisor_ref = LN_INVALID_INDEX;
  const uint32_t numerator = AddFloatTreePropertyExpression(
      *program, "numerator", 1.0e-25f, numerator_ref);
  const uint32_t divisor = AddFloatTreePropertyExpression(
      *program, "divisor", 1.0e-25f, divisor_ref);

  LN_FloatExpression safe_divide;
  safe_divide.kind = LN_FloatExpressionKind::SafeDivide;
  safe_divide.input0 = numerator;
  safe_divide.input1 = divisor;
  const uint32_t safe_divide_index = program->AddFloatExpression(safe_divide);

  LN_FloatExpression guarded_divide = safe_divide;
  guarded_divide.kind = LN_FloatExpressionKind::Divide;
  const uint32_t guarded_divide_index = program->AddFloatExpression(guarded_divide);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(283), 0, 0);
  LN_TickContext context;
  EXPECT_FLOAT_EQ(
      LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, safe_divide_index, -1.0f, context),
      1.0f);
  EXPECT_FLOAT_EQ(
      LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, guarded_divide_index, -1.0f, context),
      0.0f);

  ASSERT_TRUE(SetFloatTreeProperty(runtime_tree, divisor_ref, 0.0f));
  EXPECT_FLOAT_EQ(
      LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, safe_divide_index, -1.0f, context),
      0.0f);
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
  context.delta_time = 0.1;
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
  context.delta_time = 1.0;

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
  context.delta_time = 0.1;
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
  context.delta_time = 0.1;
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
    context.delta_time = 1.0;
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
  context.delta_time = double(tiny_duration) * 0.5;
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
  context.delta_time = 0.01;
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
  context.delta_time = 0.1;
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
  context.delta_time = 0.01;
  context.unscaled_dt = 0.0;
  context.tick_index = 4;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  ASSERT_TRUE(SetBoolTreeProperty(runtime_tree, flow_ref, false));
  context.unscaled_dt = 0.2;
  context.tick_index = 5;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, WaitForNextFrameFromFixedFlowPulsesExactlyOnceAtZeroDelta)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();

  LN_BoolExpression next_frame_done;
  next_frame_done.kind = LN_BoolExpressionKind::NextFrameDone;
  next_frame_done.int_value = int32_t(state_index);
  const uint32_t done_index = program->AddBoolExpression(next_frame_done);

  LN_Instruction arm;
  arm.opcode = LN_OpCode::ArmNextFrame;
  arm.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnFixedUpdate, arm);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = done_index;
  program->AddInstruction(LN_Event::OnUpdate, quit);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(47), 4, 0);

  LN_TickContext fixed_context;
  fixed_context.event = LN_Event::OnFixedUpdate;
  fixed_context.tick_index = 1;
  fixed_context.phase_tick_index = 0;
  fixed_context.delta_time = 1.0 / 60.0;
  fixed_context.use_fixed_timestep = true;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, fixed_context).empty());

  LN_TickContext update_context;
  update_context.event = LN_Event::OnUpdate;
  update_context.tick_index = 2;
  update_context.phase_tick_index = 0;
  update_context.delta_time = 0.0;
  update_context.unscaled_dt = 0.0;
  update_context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              update_context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);

  update_context.tick_index = 3;
  update_context.phase_tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, update_context).empty());
}

TEST(LN_RuntimeTree, WaitForNextFrameOnInitDoesNotPulseDuringItsHostingUpdate)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t state_index = program->AddTimeFlowState();

  LN_BoolExpression next_frame_done;
  next_frame_done.kind = LN_BoolExpressionKind::NextFrameDone;
  next_frame_done.int_value = int32_t(state_index);
  const uint32_t done_index = program->AddBoolExpression(next_frame_done);

  LN_Instruction arm;
  arm.opcode = LN_OpCode::ArmNextFrame;
  arm.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnInit, arm);

  LN_Instruction quit;
  quit.opcode = LN_OpCode::QuitGame;
  quit.bool_guard_expr_index = done_index;
  program->AddInstruction(LN_Event::OnUpdate, quit);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(48), 4, 0);

  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 10;
  context.phase_tick_index = 0;
  context.delta_time = 0.0;
  context.unscaled_dt = 0.0;
  context.use_fixed_timestep = true;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 11;
  context.phase_tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, WaitForNextFrameCoalescesAllPendingFixedPulses)
{
  std::shared_ptr<LN_Program> program = CreateWaitForNextFrameQuitProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x30a), 0, 0);

  LN_TickContext fixed_context;
  fixed_context.event = LN_Event::OnFixedUpdate;
  fixed_context.use_fixed_timestep = true;
  fixed_context.delta_time = 1.0 / 60.0;
  for (uint64_t fixed_tick = 0; fixed_tick < 3; fixed_tick++) {
    fixed_context.tick_index = fixed_tick;
    fixed_context.phase_tick_index = fixed_tick;
    EXPECT_TRUE(RecordReadyCommands(runtime_tree, fixed_context).empty());
  }

  LN_TickContext update_context;
  update_context.event = LN_Event::OnUpdate;
  update_context.use_fixed_timestep = true;
  update_context.tick_index = 3;
  update_context.phase_tick_index = 0;
  update_context.delta_time = 0.0;
  update_context.unscaled_dt = 0.0;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, update_context).size(), 1u);

  update_context.tick_index = 4;
  update_context.phase_tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, update_context).empty());
}

TEST(LN_RuntimeTree, WaitForNextFrameCanRearmEveryFrameWithoutLosingPulses)
{
  std::shared_ptr<LN_Program> program = CreateWaitForNextFrameQuitProgram(LN_Event::OnUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x30b), 0, 0);

  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.0;
  context.unscaled_dt = 0.0;
  context.tick_index = 0;
  context.phase_tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 1;
  context.phase_tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  context.tick_index = 2;
  context.phase_tick_index = 2;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);
}

TEST(LN_RuntimeTree, WaitForNextFramePendingStateSurvivesDisableResume)
{
  std::shared_ptr<LN_Program> program = CreateWaitForNextFrameQuitProgram(LN_Event::OnFixedUpdate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x30c), 0, 0);

  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.tick_index = 0;
  context.phase_tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(false);
  context.event = LN_Event::OnUpdate;
  context.tick_index = 1;
  context.phase_tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 2;
  context.phase_tick_index = 1;
  EXPECT_EQ(RecordReadyCommands(runtime_tree, context).size(), 1u);

  context.tick_index = 3;
  context.phase_tick_index = 2;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
}

TEST(LN_RuntimeTree, InitLatentStateUsesItsCompiledFixedContinuationDomain)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(49), 0, 0);

  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 1.0;
  context.tick_index = 20;
  context.phase_tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 21;
  context.phase_tick_index = 1;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.event = LN_Event::OnFixedUpdate;
  context.delta_time = 0.1;
  context.tick_index = 22;
  context.phase_tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  context.tick_index = 23;
  context.phase_tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::QuitGame);
}

TEST(LN_RuntimeTree, TimerArmedOnInitEmitsContinuationAfterFixedDelta)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(65), 0, 0);

  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;

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

TEST(LN_RuntimeTree, UnscaledTimerAdvancesWhenScaledFixedDeltaIsZero)
{
  std::shared_ptr<LN_Program> program = CreateTimerQuitProgram(0.2f, LN_Event::OnInit, true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(165), 0, 0);

  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.0;
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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;
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
  context.delta_time = 0.1;

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

TEST(LN_RuntimeTree, PauseContextAndUnscaledTweenUseTheRenderUpdateClock)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression paused;
  paused.kind = LN_BoolExpressionKind::GamePaused;
  const uint32_t paused_index = program->AddBoolExpression(paused);

  LN_FloatExpression unscaled_delta;
  unscaled_delta.kind = LN_FloatExpressionKind::UnscaledDeltaTime;
  const uint32_t unscaled_delta_index = program->AddFloatExpression(unscaled_delta);

  const TweenExpressionRefs scaled_tween = AddTweenExpressions(
      *program, AddBoolConstant(*program, true), AddBoolConstant(*program, false), 0.2f);
  const TweenExpressionRefs unscaled_tween = AddTweenExpressions(*program,
                                                                 AddBoolConstant(*program, true),
                                                                 AddBoolConstant(*program, false),
                                                                 0.2f,
                                                                 false,
                                                                 false,
                                                                 1.0f,
                                                                 true);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(205), 0, 0);
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.use_fixed_timestep = true;
  context.game_paused = true;
  context.delta_time = 0.0;
  context.unscaled_dt = 0.1;
  context.tick_index = 1;
  context.phase_tick_index = 1;

  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, paused_index, context));
  EXPECT_FLOAT_EQ(
      LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, unscaled_delta_index, -1.0f, context),
      0.1f);
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      runtime_tree, scaled_tween.factor_index, -1.0f, context),
                  0.0f);
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      runtime_tree, unscaled_tween.factor_index, -1.0f, context),
                  0.5f);

  context.game_paused = false;
  context.tick_index = 2;
  context.phase_tick_index = 2;
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, paused_index, context));

  LN_RuntimeTree register_runtime_tree(program, FakeGameObjectPointer(206), 0, 0);
  context.use_register_expression_evaluator = true;
  context.game_paused = true;
  context.tick_index = 3;
  context.phase_tick_index = 3;
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(register_runtime_tree, paused_index, context));
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      register_runtime_tree, unscaled_delta_index, -1.0f, context),
                  0.1f);
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      register_runtime_tree, unscaled_tween.factor_index, -1.0f, context),
                  0.5f);
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
  context.delta_time = 0.1;

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
  context.delta_time = 0.1;

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
  context.delta_time = 0.1;

  context.tick_index = 0;
  context.phase_tick_index = 0;
  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  const LN_CommandBuffer::Command *command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 15.0f, 0.0001f);

  context.tick_index = 2;
  context.phase_tick_index = 2;
  commands = RecordReadyCommands(runtime_tree, context);
  command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 15.0f, 0.0001f);
}

TEST(LN_RuntimeTree, TweenValueInstantResetIgnoresInterleavedSchedulerPhases)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const TweenExpressionRefs tween = AddTweenExpressions(
      *program, LN_INVALID_INDEX, LN_INVALID_INDEX, 0.2f, true, true);
  const uint32_t result_property = AddTweenTestPropertyRef(
      *program, "tween_interleaved_phase", LN_ValueType::Float);
  AddSetFloatPropertyInstruction(*program, result_property, tween.float_result_index);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(303), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;
  context.tick_index = 10;
  context.phase_tick_index = 20;

  std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree, context);
  const LN_CommandBuffer::Command *command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 15.0f, 0.0001f);

  /* A render/update manager tick may occur between consecutive fixed-phase ticks. The global
   * scheduler index skips, but the fixed-phase index remains consecutive and must not reset. */
  context.tick_index = 12;
  context.phase_tick_index = 21;
  commands = RecordReadyCommands(runtime_tree, context);
  command = FindPropertyCommand(commands, result_property);
  ASSERT_NE(command, nullptr);
  EXPECT_NEAR(command->property_value.float_value, 20.0f, 0.0001f);
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
  context.delta_time = 0.1;

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
  /* Serialized/compiler-produced programs must reject non-finite curve samples.  Continue with
   * this deliberately malformed program to verify that the runtime still fails safely if a
   * corrupted program reaches evaluation. */
  ASSERT_FALSE(program->ValidateInstructionPayloads());

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(205), 0, 0);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;
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
  context.delta_time = 0.1;

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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;

  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(false);
  context.tick_index = 1;
  context.delta_time = 1.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetEnabled(true);
  context.tick_index = 2;
  context.delta_time = 0.1;
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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;

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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;
  context.tick_index = 0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

  runtime_tree.SetProgram(LN_Program::CreateDebugSetWorldPosition(MT_Vector3(4.0f, 5.0f, 6.0f)));

  context.event = LN_Event::OnUpdate;
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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;

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
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.05;
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
  context.delta_time = 0.1;

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
  context.delta_time = 0.1;

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
  context.delta_time = 0.05;
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
  context.event = LN_Event::OnUpdate;
  context.use_fixed_timestep = true;
  context.delta_time = 0.1;

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
  context.delta_time = 0.1;

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
  context.delta_time = 0.05;
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
  context.delta_time = 0.1;

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

TEST(LN_RuntimeTree, ExecBlockIRRecordsShapeKeyCommandDirectly)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetShapeKeyValue;
  instruction.string_expr_index = AddStringConstant(*program, "Smile");
  instruction.float_expr_index = AddFloatConstant(*program, 0.625f);
  program->AddInstruction(LN_Event::OnFixedUpdate, instruction);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

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
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetShapeKeyValue);
  EXPECT_EQ(commands[0].object, FakeGameObjectPointer(45));
  EXPECT_EQ(commands[0].property_name, "Smile");
  EXPECT_EQ(commands[0].scalar_value, 0.625f);
  EXPECT_EQ(counters.instruction_dispatch_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
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

TEST(LN_RuntimeTree, ExecBlockIRRecordsModernBoneParentOptions)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(0x880);
  KX_GameObject *parent_object = FakeGameObjectPointer(0x881);

  LN_RuntimeTree runtime_tree(program, owner, 4, 0);
  const LN_RuntimeRef parent_ref = runtime_tree.MakeObjectRef(parent_object, "armature");

  LN_Instruction set_parent;
  set_parent.opcode = LN_OpCode::SetParent;
  set_parent.bool_value = true; /* Modern request marker. */
  set_parent.int_value = int32_t(KX_ParentType::Bone);
  set_parent.secondary_value_expr_index = AddObjectRefConstant(*program, parent_ref);
  set_parent.string_expr_index = AddStringConstant(*program, "hand.R");
  program->AddInstruction(LN_Event::OnFixedUpdate, set_parent);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext context;
  context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, nullptr);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetParentFromRef);
  EXPECT_EQ(commands[0].object, owner);
  ASSERT_NE(commands[0].parent_options, nullptr);
  const KX_ParentOptions &options = *commands[0].parent_options;
  EXPECT_EQ(options.type, KX_ParentType::Bone);
  EXPECT_FALSE(options.compound);
  EXPECT_EQ(options.bone_name, "hand.R");
}

TEST(LN_RuntimeTree, ExecBlockIRRecordsModernObjectCompoundOption)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(0x882);
  KX_GameObject *parent_object = FakeGameObjectPointer(0x883);

  LN_RuntimeTree runtime_tree(program, owner, 4, 0);
  const LN_RuntimeRef parent_ref = runtime_tree.MakeObjectRef(parent_object, "parent");

  LN_Instruction set_parent;
  set_parent.opcode = LN_OpCode::SetParent;
  set_parent.bool_value = true;
  set_parent.int_value = int32_t(KX_ParentType::Object);
  set_parent.secondary_value_expr_index = AddObjectRefConstant(*program, parent_ref);
  set_parent.bool_expr_index = AddBoolConstant(*program, true);
  program->AddInstruction(LN_Event::OnFixedUpdate, set_parent);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext context;
  context.use_fixed_timestep = true;
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context, nullptr);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();

  ASSERT_EQ(commands.size(), 1u);
  ASSERT_NE(commands[0].parent_options, nullptr);
  EXPECT_EQ(commands[0].parent_options->type, KX_ParentType::Object);
  EXPECT_TRUE(commands[0].parent_options->compound);
  EXPECT_TRUE(commands[0].parent_options->bone_name.empty());
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

TEST(LN_RuntimeTree, TranslateTreatsSpeedAsUnitsPerSecond)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject *owner = FakeGameObjectPointer(89);
  LN_RuntimeTree runtime_tree(program, owner, 4, 0);

  LN_Instruction translate;
  translate.opcode = LN_OpCode::Translate;
  translate.vector_expr_index = AddVectorConstant(*program, MT_Vector3(2.0f, 0.0f, 0.0f));
  translate.float_expr_index = AddFloatConstant(*program, 3.0f);
  translate.bool_expr_index = AddBoolConstant(*program, false);
  program->AddInstruction(LN_Event::OnFixedUpdate, translate);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.delta_time = 0.25;
  context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyMovement);
  EXPECT_NEAR(commands[0].vector_value.x(), 1.5f, 0.0001f);
  EXPECT_NEAR(commands[0].vector_value.y(), 0.0f, 0.0001f);
  EXPECT_NEAR(commands[0].vector_value.z(), 0.0f, 0.0001f);
}

TEST(LN_RuntimeTree, RotateTowardMaximumStepIsAnAngularDistanceNotAFactor)
{
  KX_GameObject owner;
  SG_Callbacks callbacks;
  SG_Node sg_node(&owner, nullptr, callbacks);
  sg_node.SetParentRelation(new TestRootParentRelation());
  owner.SetSGNode(&sg_node);
  owner.NodeSetWorldPosition(MT_Vector3(0.0f, 0.0f, 0.0f));
  owner.NodeSetLocalOrientation(MT_Matrix3x3::Identity());
  owner.NodeUpdateGS(0.0);

  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  command_buffer.AppendRotateToward(&owner, MT_Vector3(1.0f, 0.0f, 0.0f), 0.25f, 2, 1, true, 1, 1);
  command_buffer.EndRecording();
  command_buffer.Flush();

  const LN_Rotation actual = LN_Rotation::FromMatrix(owner.NodeGetLocalOrientation());
  EXPECT_NEAR(actual.AngleTo(LN_Rotation::Identity()), 0.25f, 0.0001f);
  owner.SetSGNode(nullptr);
}

TEST(LN_RuntimeTree, GamepadLookUsesAngularSpeedAndFixedDeltaTime)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  SG_Callbacks callbacks;
  SG_Node sg_node(&owner, nullptr, callbacks);
  sg_node.SetParentRelation(new TestRootParentRelation());
  owner.SetSGNode(&sg_node);
  owner.NodeUpdateGS(0.0);
  LN_RuntimeTree runtime_tree(program, &owner, 4, 0);

  LN_Instruction look;
  look.opcode = LN_OpCode::GamepadLook;
  look.vector_expr_index = AddVectorConstant(*program, MT_Vector3(1.0f, 0.0f, 0.0f));
  look.secondary_vector_expr_index = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  look.color_expr_index = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  look.int_expr_index = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  look.bool_expr_index = AddBoolConstant(*program, false);
  look.secondary_bool_expr_index = AddBoolConstant(*program, false);
  look.float_expr_index = AddFloatConstant(*program, 2.0f);
  look.string_expr_index = AddFloatConstant(*program, 1.0f);
  program->AddInstruction(LN_Event::OnFixedUpdate, look);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.delta_time = 0.25;
  context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetLocalOrientation);
  /* The stick to the right turns right: clockwise seen from above. */
  ExpectRotationEulerNear(commands[0].rotation_value, MT_Vector3(0.0f, 0.0f, -0.5f));

  owner.SetSGNode(nullptr);
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
  ExpectRotationEulerNear(reference_commands[0].rotation_value, MT_Vector3(0.0f, 0.0f, -0.2f));
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
  EXPECT_LE(ir_commands[0].rotation_value.AngleTo(reference_commands[0].rotation_value), 0.0001f);
  EXPECT_EQ(ir_commands[0].bool_value, reference_commands[0].bool_value);
  EXPECT_EQ(counters.exec_block_count, 1u);
  EXPECT_EQ(counters.exec_direct_instruction_count, 1u);
  EXPECT_EQ(counters.exec_fallback_instruction_count, 0u);
  EXPECT_EQ(counters.exec_fallback_block_count, 0u);
}

TEST(LN_RuntimeTree, MouseLookSmoothingHasTheSameTimeResponseAtDifferentFixedRates)
{
  auto accumulated_yaw_after_one_second = [](const int32_t tick_rate) {
    std::shared_ptr<const LN_Program> program = CreateMouseLookInputOnlyProgram(false, 0.9f);
    DEV_InputDevice input_device;
    LN_InputSnapshot input_snapshot;
    LN_TickReadContext read_context;
    read_context.input = &input_snapshot;
    read_context.input_channels = LN_DEP_INPUT_MOUSE;
    read_context.has_input = true;

    KX_GameObject owner;
    LN_RuntimeTree runtime_tree(program, &owner, 4, 0);
    const float delta_time = 1.0f / float(tick_rate);

    CaptureMousePosition(input_device, input_snapshot, 0, 0);
    read_context.tick_index = 1;
    runtime_tree.CaptureSnapshot(&read_context, delta_time, read_context.tick_index, false);
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.delta_time = delta_time;
    context.use_fixed_timestep = true;
    context.tick_index = read_context.tick_index;
    EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());

    double accumulated_yaw = 0.0;
    for (int32_t tick = 0; tick < tick_rate; tick++) {
      CaptureMousePosition(input_device, input_snapshot, 100, 0);
      read_context.tick_index = uint64_t(tick) + 2;
      runtime_tree.CaptureSnapshot(&read_context, delta_time, read_context.tick_index, false);
      context.tick_index = read_context.tick_index;
      const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                                  context);
      for (const LN_CommandBuffer::Command &command : commands) {
        if (command.type == LN_CommandBuffer::CommandType::ApplyRotation &&
            command.object == &owner && !command.bool_value)
        {
          accumulated_yaw += double(command.rotation_value.ToEulerXYZ().z());
        }
      }
    }
    return accumulated_yaw;
  };

  const double yaw_at_60_hz = accumulated_yaw_after_one_second(60);
  const double yaw_at_240_hz = accumulated_yaw_after_one_second(240);
  EXPECT_NEAR(yaw_at_60_hz, yaw_at_240_hz, 0.00001);
  EXPECT_NEAR(yaw_at_60_hz, -0.2, 0.001);
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
  ExpectRotationEulerNear(commands[0].rotation_value, MT_Vector3(0.0f, 0.0f, -0.2f));
  EXPECT_FALSE(commands[0].bool_value);

  EXPECT_EQ(commands[1].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(commands[1].object, &owner);
  ExpectRotationEulerNear(commands[1].rotation_value, MT_Vector3(-0.2f, 0.0f, 0.0f));
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
    ExpectRotationEulerNear(commands[0].rotation_value, MT_Vector3(0.0f, 0.0f, -0.2f));
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
  ExpectRotationEulerNear(commands[0].rotation_value, MT_Vector3(0.0f, 0.0f, -0.2f));
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

TEST(LN_RuntimeTree, FrameUpdateRecordsRelativeRotationForNonDynamicVisualObject)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_Instruction apply_rotation;
  apply_rotation.opcode = LN_OpCode::ApplyTransformVector;
  apply_rotation.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
  apply_rotation.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Rotation);
  apply_rotation.vector_value = MT_Vector3(0.0f, 0.0f, 0.5f);
  program->AddInstruction(LN_Event::OnUpdate, apply_rotation);
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  KX_GameObject visual_object;
  LN_RuntimeTree runtime_tree(program, &visual_object, 4, 0);
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.use_fixed_timestep = true;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);

  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::ApplyRotation);
  EXPECT_EQ(commands[0].object, &visual_object);
  EXPECT_LE(
      commands[0].rotation_value.AngleTo(LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.0f, 0.5f))),
      0.0001f);
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
  ASSERT_EQ(ir_commands.size(), 75u);
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
      LN_CommandBuffer::CommandType::SetGamePaused,
      LN_CommandBuffer::CommandType::SetPhysicsPaused,
      LN_CommandBuffer::CommandType::LoadBlendFile,
      LN_CommandBuffer::CommandType::SaveGame,
      LN_CommandBuffer::CommandType::SetSaveExistence,
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
  ExpectRotationEulerNear(transform_command->rotation_value, MT_Vector3(0.7f, 0.8f, 0.9f));
  EXPECT_EQ(transform_command->int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(transform_command->secondary_int_value, 1);
  const auto set_save_existence_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetSaveExistence;
      });
  ASSERT_NE(set_save_existence_command, ir_commands.end());
  EXPECT_EQ(set_save_existence_command->object, FakeGameObjectPointer(48));
  EXPECT_EQ(set_save_existence_command->event_target_object, FakeGameObjectPointer(48));
  EXPECT_FALSE(set_save_existence_command->bool_value);
  EXPECT_NE(set_save_existence_command->property_ref_index, LN_INVALID_INDEX);
  const auto set_game_paused_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetGamePaused;
      });
  ASSERT_NE(set_game_paused_command, ir_commands.end());
  EXPECT_TRUE(set_game_paused_command->bool_value);
  EXPECT_EQ(set_game_paused_command->object, nullptr);

  const auto set_physics_paused_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetPhysicsPaused;
      });
  ASSERT_NE(set_physics_paused_command, ir_commands.end());
  EXPECT_FALSE(set_physics_paused_command->bool_value);
  EXPECT_EQ(set_physics_paused_command->object, nullptr);

  const auto load_scene_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::LoadScene;
      });
  ASSERT_NE(load_scene_command, ir_commands.end());
  EXPECT_EQ(load_scene_command->object, FakeGameObjectPointer(48));
  EXPECT_EQ(load_scene_command->property_name, "LevelScene");

  const auto set_scene_command = std::find_if(
      ir_commands.begin(), ir_commands.end(), [](const LN_CommandBuffer::Command &command) {
        return command.type == LN_CommandBuffer::CommandType::SetScene;
      });
  ASSERT_NE(set_scene_command, ir_commands.end());
  EXPECT_EQ(set_scene_command->object, FakeGameObjectPointer(48));
  EXPECT_EQ(set_scene_command->property_name, "MenuScene");
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

TEST(LN_RuntimeTree, MouseMovedDistinguishesOnceFromEachLogicTick)
{
  auto run_sequence = [](const bool each_logic_tick,
                         const bool use_register_evaluator,
                         uint64_t &r_register_hits,
                         uint64_t &r_register_fallbacks) {
    std::shared_ptr<const LN_Program> program = CreateRegisterMouseMovedInputCommandProgram(
        each_logic_tick, LN_Event::OnFixedUpdate);
    std::vector<std::string> validation_errors;
    if (!program->ValidateInstructionPayloads(&validation_errors)) {
      for (const std::string &error : validation_errors) {
        ADD_FAILURE() << error;
      }
    }

    DEV_InputDevice input_device;
    LN_InputSnapshot input_snapshot;
    /* Establish and release a baseline position before the observed sequence. */
    input_device.ConvertMoveEvent(0, 0);
    input_snapshot.Capture(&input_device, nullptr);
    input_device.ClearInputs();
    input_device.ReleaseMoveEvent();
    input_snapshot.Capture(&input_device, nullptr);

    LN_TickReadContext read_context;
    read_context.input = &input_snapshot;
    read_context.input_channels = LN_DEP_INPUT_MOUSE;
    read_context.has_input = true;

    KX_GameObject owner;
    LN_RuntimeTree runtime_tree(program, &owner, 4, 0);
    LN_RuntimeProfileCounters counters;
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.use_fixed_timestep = true;
    context.use_register_expression_evaluator = use_register_evaluator;

    auto evaluate_tick = [&](const uint64_t tick) {
      read_context.tick_index = tick;
      runtime_tree.CaptureSnapshot(&read_context, 1.0f / 240.0f, tick, false);
      context.tick_index = tick;
      context.phase_tick_index = tick;

      LN_CommandBuffer command_buffer;
      command_buffer.BeginRecording();
      runtime_tree.ExecuteReady(command_buffer, context, &counters);
      command_buffer.EndRecording();
      const std::vector<LN_CommandBuffer::Command> commands =
          command_buffer.TakeRecordedCommands();
      EXPECT_EQ(commands.size(), 1u);
      return commands.empty() ? false : commands.front().bool_value;
    };

    std::array<bool, 5> results;
    results[0] = evaluate_tick(0);

    input_device.ClearInputs();
    input_device.ConvertMoveEvent(10, 0);
    input_snapshot.Capture(&input_device, nullptr);
    results[1] = evaluate_tick(1);

    /* Consecutive movement keeps the axes active without producing a second activation edge. */
    input_device.ClearInputs();
    input_device.ConvertMoveEvent(20, 0);
    input_snapshot.Capture(&input_device, nullptr);
    results[2] = evaluate_tick(2);

    input_device.ClearInputs();
    input_device.ReleaseMoveEvent();
    input_snapshot.Capture(&input_device, nullptr);
    results[3] = evaluate_tick(3);

    input_device.ClearInputs();
    input_device.ConvertMoveEvent(30, 0);
    input_snapshot.Capture(&input_device, nullptr);
    LN_InputSnapshot pending_fixed_input;
    pending_fixed_input.CopyFrom(input_snapshot);
    input_device.ClearInputs();
    input_device.ReleaseMoveEvent();
    input_snapshot.Capture(&input_device, nullptr);
    pending_fixed_input.AccumulateFrame(input_snapshot);
    read_context.input = &pending_fixed_input;
    /* A complete movement/release pair between fixed ticks still retains its activation edge. */
    results[4] = evaluate_tick(4);

    r_register_hits = counters.register_expression_hit_count;
    r_register_fallbacks = counters.register_expression_fallback_count;
    return results;
  };

  uint64_t unused_hits = 0;
  uint64_t unused_fallbacks = 0;
  const std::array<bool, 5> once_reference = run_sequence(
      false, false, unused_hits, unused_fallbacks);
  EXPECT_EQ(once_reference, (std::array<bool, 5>{false, true, false, false, true}));

  const std::array<bool, 5> each_reference = run_sequence(
      true, false, unused_hits, unused_fallbacks);
  EXPECT_EQ(each_reference, (std::array<bool, 5>{false, true, true, false, true}));

  uint64_t once_hits = 0;
  uint64_t once_fallbacks = 0;
  EXPECT_EQ(run_sequence(false, true, once_hits, once_fallbacks), once_reference);
  EXPECT_GT(once_hits, 0u);
  EXPECT_EQ(once_fallbacks, 0u);

  uint64_t each_hits = 0;
  uint64_t each_fallbacks = 0;
  EXPECT_EQ(run_sequence(true, true, each_hits, each_fallbacks), each_reference);
  EXPECT_GT(each_hits, 0u);
  EXPECT_EQ(each_fallbacks, 0u);
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
  EXPECT_TRUE(std::isnan(reference_commands[0].vector_value.z()));
  EXPECT_TRUE(std::isnan(register_commands[0].vector_value.z()));
  EXPECT_FLOAT_EQ(register_commands[0].vector_value.y(), 0.0f);
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

TEST(LN_RuntimeTree, AverageCenterPositionVectorListReportsAllOutputsAndRejectsInvalidItems)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_Value vectors;
  vectors.type = LN_ValueType::List;
  vectors.exists = true;
  const auto append_vector = [&](const MT_Vector3 &value) {
    LN_Value item;
    item.type = LN_ValueType::Vector;
    item.exists = true;
    item.vector_value = value;
    vectors.list_value.push_back(item);
  };
  append_vector(MT_Vector3(1.0f, 2.0f, 3.0f));
  append_vector(MT_Vector3(3.0f, 4.0f, 5.0f));
  append_vector(MT_Vector3(std::numeric_limits<float>::infinity(), 9.0f, 9.0f));
  LN_Value non_vector;
  non_vector.type = LN_ValueType::Float;
  non_vector.exists = true;
  non_vector.float_value = 100.0f;
  vectors.list_value.push_back(non_vector);

  LN_ValueExpression vector_list;
  vector_list.kind = LN_ValueExpressionKind::Constant;
  vector_list.value = vectors;
  const uint32_t list_index = program->AddValueExpression(vector_list);

  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::VectorAverage;
  query.average_center_source = LN_AverageCenterSource::VectorList;
  query.input0 = list_index;
  const uint32_t query_index = program->AddQueryExpression(query);

  LN_VectorExpression average;
  average.kind = LN_VectorExpressionKind::QueryVectorAverage;
  average.input0 = query_index;
  const uint32_t average_index = program->AddVectorExpression(average);

  LN_IntExpression count;
  count.kind = LN_IntExpressionKind::QueryVectorCount;
  count.input0 = query_index;
  const uint32_t count_index = program->AddIntExpression(count);

  LN_BoolExpression valid;
  valid.kind = LN_BoolExpressionKind::QueryValid;
  valid.input0 = query_index;
  const uint32_t valid_index = program->AddBoolExpression(valid);

  LN_Value empty_vectors;
  empty_vectors.type = LN_ValueType::List;
  empty_vectors.exists = true;
  LN_ValueExpression empty_list;
  empty_list.kind = LN_ValueExpressionKind::Constant;
  empty_list.value = empty_vectors;
  LN_QueryExpression empty_query;
  empty_query.kind = LN_QueryExpressionKind::VectorAverage;
  empty_query.average_center_source = LN_AverageCenterSource::VectorList;
  empty_query.input0 = program->AddValueExpression(empty_list);
  const uint32_t empty_query_index = program->AddQueryExpression(empty_query);
  average.input0 = empty_query_index;
  const uint32_t empty_average_index = program->AddVectorExpression(average);
  count.input0 = empty_query_index;
  const uint32_t empty_count_index = program->AddIntExpression(count);
  valid.input0 = empty_query_index;
  const uint32_t empty_valid_index = program->AddBoolExpression(valid);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_TRUE(program->IsParallelEligible());
  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  LN_TickContext context;
  context.tick_index = 1;

  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, average_index, MT_Vector3(-1.0f, -1.0f, -1.0f), context),
            MT_Vector3(2.0f, 3.0f, 4.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, count_index, context), 2);
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid_index, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, empty_average_index, MT_Vector3(-1.0f, -1.0f, -1.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, empty_count_index, context), 0);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, empty_valid_index, context));
}

TEST(LN_RuntimeTree, DistanceBetweenSupportsEveryEndpointCombinationAndOutput)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t vector_a = AddVectorConstant(*program, MT_Vector3(1.0f, 2.0f, 3.0f));
  const uint32_t vector_b = AddVectorConstant(*program, MT_Vector3(4.0f, 6.0f, 3.0f));
  const uint32_t position_for_object_a = AddVectorConstant(*program,
                                                           MT_Vector3(10.0f, 6.0f, 8.0f));
  const uint32_t position_for_object_b = AddVectorConstant(*program,
                                                           MT_Vector3(-2.0f, 3.0f, 6.0f));
  const uint32_t target_value = AddNoneValueConstant(*program);

  struct Outputs {
    uint32_t distance;
    uint32_t squared_distance;
    uint32_t valid;
  };
  const auto add_query = [&](const LN_DistanceEndpointMode mode_a,
                             const uint32_t input_a,
                             const LN_DistanceEndpointMode mode_b,
                             const uint32_t input_b) {
    LN_QueryExpression query;
    query.kind = LN_QueryExpressionKind::DistanceBetween;
    query.distance_endpoint_a = mode_a;
    query.distance_endpoint_b = mode_b;
    query.input0 = input_a;
    query.input1 = input_b;
    const uint32_t query_index = program->AddQueryExpression(query);

    LN_FloatExpression distance;
    distance.kind = LN_FloatExpressionKind::QueryDistance;
    distance.input0 = query_index;
    const uint32_t distance_index = program->AddFloatExpression(distance);
    distance.kind = LN_FloatExpressionKind::QuerySquaredDistance;
    const uint32_t squared_distance_index = program->AddFloatExpression(distance);
    LN_BoolExpression valid;
    valid.kind = LN_BoolExpressionKind::QueryValid;
    valid.input0 = query_index;
    return Outputs{distance_index, squared_distance_index, program->AddBoolExpression(valid)};
  };

  const Outputs vector_vector = add_query(
      LN_DistanceEndpointMode::Position, vector_a, LN_DistanceEndpointMode::Position, vector_b);
  const Outputs object_vector = add_query(LN_DistanceEndpointMode::Object,
                                          target_value,
                                          LN_DistanceEndpointMode::Position,
                                          position_for_object_a);
  const Outputs vector_object = add_query(LN_DistanceEndpointMode::Position,
                                          position_for_object_b,
                                          LN_DistanceEndpointMode::Object,
                                          target_value);
  const Outputs object_object = add_query(LN_DistanceEndpointMode::Object,
                                          LN_INVALID_INDEX,
                                          LN_DistanceEndpointMode::Object,
                                          target_value);
  const Outputs missing_target = add_query(LN_DistanceEndpointMode::Object,
                                           LN_INVALID_INDEX,
                                           LN_DistanceEndpointMode::Object,
                                           LN_INVALID_INDEX);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  PositionedGameObject owner(MT_Vector3(1.0f, 2.0f, 3.0f));
  PositionedGameObject target(MT_Vector3(10.0f, 0.0f, 0.0f));
  LN_RuntimeTree runtime_tree(program, &owner.object, 0, 0);

  std::vector<LN_ValueExpression> &value_expressions =
      const_cast<std::vector<LN_ValueExpression> &>(program->GetValueExpressions());
  value_expressions[target_value].value.type = LN_ValueType::ObjectRef;
  value_expressions[target_value].value.exists = true;
  value_expressions[target_value].value.runtime_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(
      runtime_tree, target.object);

  LN_TickContext context;
  context.tick_index = 5;
  const auto expect_outputs =
      [&](const Outputs &outputs, const float distance, const float squared_distance) {
        EXPECT_NEAR(
            LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, outputs.distance, -1.0f, context),
            distance,
            1.0e-5f);
        EXPECT_NEAR(LN_RuntimeTreeTestAccess::ResolveFloat(
                        runtime_tree, outputs.squared_distance, -1.0f, context),
                    squared_distance,
                    1.0e-5f);
        EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, outputs.valid, context));
      };
  expect_outputs(vector_vector, 5.0f, 25.0f);
  expect_outputs(object_vector, 10.0f, 100.0f);
  expect_outputs(vector_object, std::sqrt(189.0f), 189.0f);
  expect_outputs(object_object, std::sqrt(94.0f), 94.0f);
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      runtime_tree, missing_target.distance, -1.0f, context),
                  0.0f);
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(
                      runtime_tree, missing_target.squared_distance, -1.0f, context),
                  0.0f);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, missing_target.valid, context));
}

TEST(LN_RuntimeTree, ReadsApplyOnlyTheReadObjectsPendingTransformsInFlushOrder)
{
  /* A read that must see the step's earlier writes (Get Distance, raycasts) applies the read
   * object's pending transform writes in the order the step's flush would (sort key, then record
   * order), leaves the other objects' writes for the flush and never applies one twice. The
   * typed streams find them through an index, the legacy stream plans every command: the same
   * results. */
  const auto expect_at = [](const PositionedGameObject &object, const MT_Vector3 &expected) {
    const MT_Vector3 position = object.object.NodeGetWorldPosition();
    EXPECT_FLOAT_EQ(position.x(), expected.x());
    EXPECT_FLOAT_EQ(position.y(), expected.y());
    EXPECT_FLOAT_EQ(position.z(), expected.z());
  };
  for (const bool typed_streams : {true, false}) {
    SCOPED_TRACE(typed_streams ? "typed streams" : "legacy stream");
    PositionedGameObject first;
    PositionedGameObject second(MT_Vector3(5.0f, 0.0f, 0.0f));
    PositionedGameObject removed;
    LN_CommandBuffer buffer;
    buffer.SetTypedCommandStreamsEnabled(typed_streams);
    buffer.BeginRecording();
    buffer.AppendSetWorldPosition(&first.object, MT_Vector3(1.0f, 0.0f, 0.0f), 20, 0);
    buffer.AppendSetWorldPosition(&second.object, MT_Vector3(9.0f, 0.0f, 0.0f), 15, 0);
    /* Recorded last but sorted first: the move comes before the set. */
    buffer.AppendApplyMovement(&first.object, MT_Vector3(0.0f, 0.0f, 3.0f), false, 10, 0);
    buffer.FlushPendingObjectHierarchyTransformCommands(&first.object);
    expect_at(first, MT_Vector3(1.0f, 0.0f, 0.0f));
    expect_at(second, MT_Vector3(5.0f, 0.0f, 0.0f));
    EXPECT_EQ(buffer.Size(), 1u);

    /* A write after a read is found by the next read; a read with nothing pending does nothing. */
    buffer.AppendApplyMovement(&first.object, MT_Vector3(0.0f, 2.0f, 0.0f), false, 30, 0);
    buffer.FlushPendingObjectTransformCommands(&first.object);
    buffer.FlushPendingObjectTransformCommands(&first.object);
    expect_at(first, MT_Vector3(1.0f, 2.0f, 0.0f));
    ASSERT_EQ(buffer.GetCommandsForTests().size(), 1u);
    EXPECT_EQ(buffer.GetCommandsForTests().front().object, &second.object);

    /* Removing an object's writes moves the others in their streams. */
    buffer.AppendSetWorldPosition(&removed.object, MT_Vector3(7.0f, 7.0f, 7.0f), 5, 0);
    buffer.AppendApplyMovement(&first.object, MT_Vector3(1.0f, 0.0f, 0.0f), false, 40, 0);
    buffer.RemoveCommandsForGameObject(&removed.object);
    buffer.FlushPendingObjectTransformCommands(&first.object);
    expect_at(first, MT_Vector3(2.0f, 2.0f, 0.0f));
    buffer.AppendApplyMovement(&second.object, MT_Vector3(0.0f, 1.0f, 0.0f), false, 50, 0);

    buffer.EndRecording();
    EXPECT_EQ(buffer.Size(), 2u);
    buffer.Flush();
    EXPECT_EQ(buffer.Size(), 0u);
    expect_at(first, MT_Vector3(2.0f, 2.0f, 0.0f));
    expect_at(second, MT_Vector3(9.0f, 1.0f, 0.0f));
    expect_at(removed, MT_Vector3(0.0f, 0.0f, 0.0f));
  }
}

TEST(LN_RuntimeTree, PredictMotionPositionModeCoversSamplingSmoothingResetAndEdgeInputs)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t source_position = AddVectorConstant(*program, MT_Vector3(0.0f, 0.0f, 0.0f));
  const uint32_t prediction_time = AddFloatConstant(*program, 2.0f);
  const uint32_t smoothing_time = AddFloatConstant(*program, 0.0f);
  const uint32_t state_index = program->AddFlowControlState();

  LN_Instruction sample;
  sample.opcode = LN_OpCode::SampleMotion;
  sample.int_value = int32_t(state_index);
  sample.secondary_int_value = int32_t(LN_MotionSourceMode::Position);
  sample.vector_expr_index = source_position;
  sample.float_expr_index = prediction_time;
  sample.secondary_float_expr_index = smoothing_time;
  const uint32_t sample_instruction = program->AddInstruction(LN_Event::OnUpdate, sample);

  LN_Instruction reset;
  reset.opcode = LN_OpCode::ResetMotion;
  reset.int_value = int32_t(state_index);
  program->AddInstruction(LN_Event::OnFixedUpdate, reset);

  const auto add_motion_vector = [&](const LN_VectorExpressionKind kind) {
    LN_VectorExpression expression;
    expression.kind = kind;
    expression.property_ref_index = state_index;
    return program->AddVectorExpression(expression);
  };
  const uint32_t position = add_motion_vector(LN_VectorExpressionKind::MotionPosition);
  const uint32_t velocity = add_motion_vector(LN_VectorExpressionKind::MotionVelocity);
  const uint32_t direction = add_motion_vector(LN_VectorExpressionKind::MotionDirection);
  const uint32_t predicted = add_motion_vector(LN_VectorExpressionKind::MotionPredictedPosition);
  LN_FloatExpression speed_expression;
  speed_expression.kind = LN_FloatExpressionKind::MotionSpeed;
  speed_expression.int_value = int32_t(state_index);
  const uint32_t speed = program->AddFloatExpression(speed_expression);
  LN_BoolExpression valid_expression;
  valid_expression.kind = LN_BoolExpressionKind::MotionValid;
  valid_expression.int_value = int32_t(state_index);
  const uint32_t valid = program->AddBoolExpression(valid_expression);
  LN_BoolExpression updated_expression;
  updated_expression.kind = LN_BoolExpressionKind::InstructionExecuted;
  updated_expression.input0 = sample_instruction;
  const uint32_t updated = program->AddBoolExpression(updated_expression);

  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_TRUE(program->IsParallelEligible());
  PositionedGameObject owner;
  LN_RuntimeTree runtime_tree(program, &owner.object, 0, 0);
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 1;
  context.current_time = 1.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, updated, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(-1.0f, -1.0f, -1.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, predicted, MT_Vector3(-1.0f, -1.0f, -1.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));

  std::vector<LN_VectorExpression> &vector_expressions =
      const_cast<std::vector<LN_VectorExpression> &>(program->GetVectorExpressions());
  std::vector<LN_FloatExpression> &float_expressions =
      const_cast<std::vector<LN_FloatExpression> &>(program->GetFloatExpressions());
  vector_expressions[source_position].vector_value = MT_Vector3(10.0f, 0.0f, 0.0f);
  context.tick_index = 2;
  context.current_time = 3.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, updated, context));
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(10.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, velocity, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(5.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, direction, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(1.0f, 0.0f, 0.0f));
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, speed, -1.0f, context),
                  5.0f);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, predicted, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(20.0f, 0.0f, 0.0f));

  /* A duplicate route in the same engine tick must not replace the accepted sample, even if its
   * source becomes invalid later in that tick. */
  vector_expressions[source_position].vector_value = MT_Vector3(
      std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(10.0f, 0.0f, 0.0f));

  /* Render updates may occur without a fixed-step time advance. Preserve the previous timed
   * sample and include the whole displacement when simulation time advances again. */
  vector_expressions[source_position].vector_value = MT_Vector3(12.0f, 0.0f, 0.0f);
  context.tick_index = 3;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(10.0f, 0.0f, 0.0f));

  vector_expressions[source_position].vector_value = MT_Vector3(20.0f, 0.0f, 0.0f);
  float_expressions[smoothing_time].float_value = 2.0f;
  context.tick_index = 4;
  context.current_time = 4.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  const float expected_smoothed_speed = 5.0f + 5.0f * (1.0f - std::exp(-0.5f));
  EXPECT_NEAR(LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, speed, -1.0f, context),
              expected_smoothed_speed,
              1.0e-5f);
  EXPECT_NEAR(LN_RuntimeTreeTestAccess::ResolveVector(
                  runtime_tree, predicted, MT_Vector3(0.0f, 0.0f, 0.0f), context)
                  .x(),
              20.0f + expected_smoothed_speed * 2.0f,
              1.0e-5f);

  context.event = LN_Event::OnFixedUpdate;
  context.tick_index = 5;
  context.current_time = 5.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, velocity, MT_Vector3(-1.0f, -1.0f, -1.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));

  vector_expressions[source_position].vector_value = MT_Vector3(30.0f, 0.0f, 0.0f);
  context.event = LN_Event::OnUpdate;
  context.tick_index = 6;
  context.current_time = 6.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, updated, context));

  vector_expressions[source_position].vector_value = MT_Vector3(40.0f, 0.0f, 0.0f);
  float_expressions[prediction_time].float_value = -2.0f;
  float_expressions[smoothing_time].float_value = std::numeric_limits<float>::quiet_NaN();
  context.tick_index = 7;
  context.current_time = 7.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, speed, -1.0f, context),
                  10.0f);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, predicted, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(40.0f, 0.0f, 0.0f));

  vector_expressions[source_position].vector_value = MT_Vector3(
      std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
  context.tick_index = 8;
  context.current_time = 8.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(40.0f, 0.0f, 0.0f));

  vector_expressions[source_position].vector_value = MT_Vector3(50.0f, 0.0f, 0.0f);
  context.tick_index = 9;
  context.current_time = 9.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));

  vector_expressions[source_position].vector_value = MT_Vector3(60.0f, 0.0f, 0.0f);
  context.tick_index = 10;
  context.current_time = 8.5;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, position, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(60.0f, 0.0f, 0.0f));

  vector_expressions[source_position].vector_value = MT_Vector3(70.0f, 0.0f, 0.0f);
  float_expressions[prediction_time].float_value = std::numeric_limits<float>::max();
  float_expressions[smoothing_time].float_value = 0.0f;
  context.tick_index = 11;
  context.current_time = 9.5;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, valid, context));
  EXPECT_FLOAT_EQ(LN_RuntimeTreeTestAccess::ResolveFloat(runtime_tree, speed, -1.0f, context),
                  10.0f);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, predicted, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(70.0f, 0.0f, 0.0f));
}

TEST(LN_RuntimeTree, PredictMotionObjectModeSupportsExplicitObjectAndOwnerFallback)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const uint32_t object_value = AddNoneValueConstant(*program);
  const uint32_t prediction_time = AddFloatConstant(*program, 1.0f);
  const uint32_t smoothing_time = AddFloatConstant(*program, 0.0f);

  const auto add_tracker = [&](const LN_MotionSourceMode mode, const uint32_t value_input) {
    const uint32_t state = program->AddFlowControlState();
    LN_Instruction sample;
    sample.opcode = LN_OpCode::SampleMotion;
    sample.int_value = int32_t(state);
    sample.secondary_int_value = int32_t(mode);
    sample.value_expr_index = value_input;
    sample.float_expr_index = prediction_time;
    sample.secondary_float_expr_index = smoothing_time;
    program->AddInstruction(LN_Event::OnUpdate, sample);

    LN_VectorExpression position;
    position.kind = LN_VectorExpressionKind::MotionPosition;
    position.property_ref_index = state;
    const uint32_t position_index = program->AddVectorExpression(position);
    LN_BoolExpression valid;
    valid.kind = LN_BoolExpressionKind::MotionValid;
    valid.int_value = int32_t(state);
    return std::pair<uint32_t, uint32_t>(position_index, program->AddBoolExpression(valid));
  };

  const auto explicit_object = add_tracker(LN_MotionSourceMode::Object, object_value);
  const auto owner_object = add_tracker(LN_MotionSourceMode::OwnerObject, LN_INVALID_INDEX);
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_FALSE(program->IsParallelEligible());

  PositionedGameObject owner(MT_Vector3(1.0f, 0.0f, 0.0f));
  PositionedGameObject target(MT_Vector3(5.0f, 0.0f, 0.0f));
  PositionedGameObject replacement(MT_Vector3(11.0f, 0.0f, 0.0f));
  LN_RuntimeTree runtime_tree(program, &owner.object, 0, 0);
  std::vector<LN_ValueExpression> &value_expressions =
      const_cast<std::vector<LN_ValueExpression> &>(program->GetValueExpressions());
  value_expressions[object_value].value.type = LN_ValueType::ObjectRef;
  value_expressions[object_value].value.exists = true;
  value_expressions[object_value].value.runtime_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(
      runtime_tree, target.object);

  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 10;
  context.current_time = 1.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(
      LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, explicit_object.second, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, owner_object.second, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, explicit_object.first, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(5.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, owner_object.first, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(1.0f, 0.0f, 0.0f));

  target.SetPosition(MT_Vector3(7.0f, 0.0f, 0.0f));
  owner.SetPosition(MT_Vector3(4.0f, 0.0f, 0.0f));
  context.tick_index = 11;
  context.current_time = 2.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(
      LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, explicit_object.second, context));
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, owner_object.second, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, explicit_object.first, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(7.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, owner_object.first, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(4.0f, 0.0f, 0.0f));

  value_expressions[object_value].value.runtime_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(
      runtime_tree, replacement.object);
  context.tick_index = 12;
  context.current_time = 3.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(
      LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, explicit_object.second, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, explicit_object.first, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            MT_Vector3(11.0f, 0.0f, 0.0f));

  replacement.SetPosition(MT_Vector3(12.0f, 0.0f, 0.0f));
  context.tick_index = 13;
  context.current_time = 4.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_TRUE(
      LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, explicit_object.second, context));

  runtime_tree.InvalidateObjectRef(&replacement.object);
  context.tick_index = 14;
  context.current_time = 5.0;
  EXPECT_TRUE(RecordReadyCommands(runtime_tree, context).empty());
  EXPECT_FALSE(
      LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, explicit_object.second, context));
}

TEST(LN_RuntimeTree, RaycastFromMeshSeparatesHitDetailsFromPerRayDiagnostics)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_QueryExpression query_expression;
  query_expression.kind = LN_QueryExpressionKind::RaycastFromMesh;
  const uint32_t query_index = program->AddQueryExpression(query_expression);
  ASSERT_EQ(query_index, 0u);
  const uint32_t query_state_index =
      program->GetQueryExpressions()[query_index].runtime_state_index;
  ASSERT_NE(query_state_index, LN_INVALID_INDEX);

  const auto add_bool = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.input0 = query_index;
    return program->AddBoolExpression(expression);
  };
  const auto add_int = [&](const LN_IntExpressionKind kind) {
    LN_IntExpression expression;
    expression.kind = kind;
    expression.input0 = query_index;
    return program->AddIntExpression(expression);
  };
  const auto add_value = [&](const LN_ValueExpressionKind kind) {
    LN_ValueExpression expression;
    expression.kind = kind;
    expression.input0 = query_index;
    return program->AddValueExpression(expression);
  };

  const uint32_t done_index = add_bool(LN_BoolExpressionKind::PhysicsQueryDone);
  const uint32_t any_hit_index = add_bool(LN_BoolExpressionKind::PhysicsQueryHit);
  const uint32_t all_hit_index = add_bool(LN_BoolExpressionKind::PhysicsQueryAllHit);
  const uint32_t hit_count_index = add_int(LN_IntExpressionKind::PhysicsQueryHitCount);
  const uint32_t ray_count_index = add_int(LN_IntExpressionKind::PhysicsQueryRayCount);
  const std::array<LN_ValueExpressionKind, 13> list_kinds = {
      LN_ValueExpressionKind::PhysicsQueryHits,
      LN_ValueExpressionKind::PhysicsQueryBlockedList,
      LN_ValueExpressionKind::PhysicsQueryObjects,
      LN_ValueExpressionKind::PhysicsQueryPoints,
      LN_ValueExpressionKind::PhysicsQueryNormals,
      LN_ValueExpressionKind::PhysicsQueryDistances,
      LN_ValueExpressionKind::PhysicsQueryFractions,
      LN_ValueExpressionKind::PhysicsQueryFaceIndices,
      LN_ValueExpressionKind::PhysicsQueryHasUVs,
      LN_ValueExpressionKind::PhysicsQueryUVs,
      LN_ValueExpressionKind::PhysicsQueryRayOrigins,
      LN_ValueExpressionKind::PhysicsQueryRayDirections,
      LN_ValueExpressionKind::PhysicsQueryEndPoints,
  };
  std::array<uint32_t, list_kinds.size()> list_indices{};
  for (size_t index = 0; index < list_kinds.size(); index++) {
    list_indices[index] = add_value(list_kinds[index]);
  }

  KX_GameObject hit_object;
  hit_object.SetName("MeshRayHit");
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7a1), 0, 0);
  LN_QueryResult seeded;
  LN_PhysicsQueryResult hit;
  hit.hit = true;
  hit.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
  hit.object_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree, hit_object);
  hit.hit_position = MT_Vector3(1.0f, 2.0f, 3.0f);
  hit.hit_normal = MT_Vector3(0.0f, 0.0f, -1.0f);
  hit.hit_distance = 2.5f;
  hit.hit_fraction = 0.25f;
  hit.polygon_index = 17;
  hit.has_uv = true;
  hit.hit_uv = MT_Vector2(0.2f, 0.7f);
  hit.ray_origin = MT_Vector3(1.0f, 2.0f, 5.5f);
  hit.ray_direction = MT_Vector3(0.0f, 0.0f, -1.0f);
  hit.end_position = MT_Vector3(1.0f, 2.0f, -4.5f);

  LN_PhysicsQueryResult blocker;
  blocker.blocked = true;
  blocker.hit_position = MT_Vector3(-1.0f, 0.0f, 4.0f);
  blocker.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  blocker.hit_distance = 1.0f;
  blocker.hit_fraction = 0.1f;
  blocker.polygon_index = -1;
  blocker.ray_origin = MT_Vector3(-1.0f, 0.0f, 5.0f);
  blocker.ray_direction = MT_Vector3(0.0f, 0.0f, -1.0f);
  blocker.end_position = MT_Vector3(-1.0f, 0.0f, -5.0f);

  LN_PhysicsQueryResult miss;
  miss.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
  miss.hit_position = MT_Vector3(8.0f, 9.0f, 10.0f);
  miss.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  miss.hit_distance = 10.0f;
  miss.hit_fraction = 1.0f;
  miss.polygon_index = -1;
  miss.ray_origin = MT_Vector3(8.0f, 9.0f, 0.0f);
  miss.ray_direction = MT_Vector3(0.0f, 0.0f, 1.0f);
  miss.end_position = miss.hit_position;

  seeded.physics_query = hit;
  seeded.physics_query_results = {hit, blocker, miss};
  constexpr uint64_t tick_index = 41;
  LN_RuntimeTreeTestAccess::SeedQueryResult(runtime_tree, query_state_index, tick_index, seeded);
  LN_TickContext context;
  context.tick_index = tick_index;

  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, done_index, context));
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, any_hit_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, all_hit_index, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, hit_count_index, context), 1);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, ray_count_index, context), 3);

  std::array<LN_Value, list_kinds.size()> lists;
  for (size_t index = 0; index < lists.size(); index++) {
    lists[index] = LN_RuntimeTreeTestAccess::ResolveValue(
        runtime_tree, list_indices[index], context);
    ASSERT_EQ(lists[index].type, LN_ValueType::List) << index;
  }
  /* Per-ray masks and ray geometry retain all three generated-ray slots. */
  for (const size_t index : {size_t(0), size_t(1), size_t(10), size_t(11), size_t(12)}) {
    ASSERT_EQ(lists[index].list_value.size(), 3u) << index;
  }
  /* Collision details contain the one matching hit only. Blockers and clear misses are excluded.
   */
  for (size_t index = 2; index <= 9; index++) {
    ASSERT_EQ(lists[index].list_value.size(), 1u) << index;
  }
  EXPECT_TRUE(lists[0].list_value[0].bool_value);
  EXPECT_FALSE(lists[0].list_value[1].bool_value);
  EXPECT_FALSE(lists[0].list_value[2].bool_value);
  EXPECT_FALSE(lists[1].list_value[0].bool_value);
  EXPECT_TRUE(lists[1].list_value[1].bool_value);
  EXPECT_FALSE(lists[1].list_value[2].bool_value);
  EXPECT_TRUE(lists[2].list_value[0].exists);
  EXPECT_EQ(lists[3].list_value[0].vector_value, hit.hit_position);
  EXPECT_EQ(lists[4].list_value[0].vector_value, hit.hit_normal);
  EXPECT_FLOAT_EQ(lists[5].list_value[0].float_value, hit.hit_distance);
  EXPECT_FLOAT_EQ(lists[6].list_value[0].float_value, hit.hit_fraction);
  EXPECT_EQ(lists[7].list_value[0].int_value, hit.polygon_index);
  EXPECT_TRUE(lists[8].list_value[0].bool_value);
  EXPECT_EQ(lists[9].list_value[0].vector_value, MT_Vector3(0.2f, 0.7f, 0.0f));
  EXPECT_EQ(lists[10].list_value[0].vector_value, hit.ray_origin);
  EXPECT_EQ(lists[10].list_value[1].vector_value, blocker.ray_origin);
  EXPECT_EQ(lists[10].list_value[2].vector_value, miss.ray_origin);
  EXPECT_EQ(lists[11].list_value[0].vector_value, hit.ray_direction);
  EXPECT_EQ(lists[11].list_value[1].vector_value, blocker.ray_direction);
  EXPECT_EQ(lists[11].list_value[2].vector_value, miss.ray_direction);
  EXPECT_EQ(lists[12].list_value[0].vector_value, hit.end_position);
  EXPECT_EQ(lists[12].list_value[1].vector_value, blocker.end_position);
  EXPECT_EQ(lists[12].list_value[2].vector_value, miss.end_position);

  /* A batch with no matching hits must expose no collision details at all, while preserving its
   * two diagnostic ray slots. This is the regression for Points previously returning miss ends. */
  LN_QueryResult no_hits;
  no_hits.physics_query = blocker;
  no_hits.physics_query_results = {blocker, miss};
  context.tick_index++;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, no_hits);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, any_hit_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, all_hit_index, context));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, hit_count_index, context), 0);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveInt(runtime_tree, ray_count_index, context), 2);
  for (size_t index = 2; index <= 9; index++) {
    const LN_Value detail_list = LN_RuntimeTreeTestAccess::ResolveValue(
        runtime_tree, list_indices[index], context);
    EXPECT_TRUE(detail_list.list_value.empty()) << index;
  }
  for (const size_t index : {size_t(0), size_t(1), size_t(10), size_t(11), size_t(12)}) {
    const LN_Value per_ray_list = LN_RuntimeTreeTestAccess::ResolveValue(
        runtime_tree, list_indices[index], context);
    EXPECT_EQ(per_ray_list.list_value.size(), 2u) << index;
  }
}

TEST(LN_RuntimeTree, RaycastFromMeshDoneDoesNotFireWhenFlowIsInactive)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();

  LN_BoolExpression inactive_flow;
  inactive_flow.kind = LN_BoolExpressionKind::Constant;
  inactive_flow.bool_value = false;
  const uint32_t inactive_flow_index = program->AddBoolExpression(inactive_flow);

  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::RaycastFromMesh;
  query.condition_bool_expr_index = inactive_flow_index;
  const uint32_t query_index = program->AddQueryExpression(query);

  LN_BoolExpression done;
  done.kind = LN_BoolExpressionKind::PhysicsQueryDone;
  done.input0 = query_index;
  const uint32_t done_index = program->AddBoolExpression(done);

  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  LN_TickContext context;
  context.tick_index = 42;
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, done_index, context));
}

TEST(LN_RuntimeTree, PhysicsQueryHitVectorsAreNeutralOnMissWhileRayGeometryRemainsAvailable)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::Raycast;
  const uint32_t query_index = program->AddQueryExpression(query);
  const uint32_t query_state_index =
      program->GetQueryExpressions()[query_index].runtime_state_index;

  const auto add_vector = [&](const LN_VectorExpressionKind kind) {
    LN_VectorExpression expression;
    expression.kind = kind;
    expression.input0 = query_index;
    return program->AddVectorExpression(expression);
  };
  const uint32_t point_index = add_vector(LN_VectorExpressionKind::PhysicsQueryPoint);
  const uint32_t normal_index = add_vector(LN_VectorExpressionKind::PhysicsQueryNormal);
  const uint32_t direction_index = add_vector(LN_VectorExpressionKind::PhysicsQueryDirection);
  const uint32_t end_point_index = add_vector(LN_VectorExpressionKind::PhysicsQueryEndPoint);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7a2), 0, 0);
  LN_QueryResult seeded;
  seeded.physics_query.hit = false;
  seeded.physics_query.hit_position = MT_Vector3(4.0f, 5.0f, 6.0f);
  seeded.physics_query.hit_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  seeded.physics_query.ray_direction = MT_Vector3(1.0f, 0.0f, 0.0f);
  seeded.physics_query.end_position = MT_Vector3(20.0f, 0.0f, 0.0f);

  LN_TickContext context;
  context.tick_index = 73;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, seeded);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, point_index, MT_Vector3(9.0f, 9.0f, 9.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, normal_index, MT_Vector3(9.0f, 9.0f, 9.0f), context),
            MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, direction_index, MT_Vector3(9.0f, 9.0f, 9.0f), context),
            seeded.physics_query.ray_direction);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, end_point_index, MT_Vector3(9.0f, 9.0f, 9.0f), context),
            seeded.physics_query.end_position);

  seeded.physics_query.hit = true;
  context.tick_index++;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, seeded);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, point_index, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            seeded.physics_query.hit_position);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveVector(
                runtime_tree, normal_index, MT_Vector3(0.0f, 0.0f, 0.0f), context),
            seeded.physics_query.hit_normal);

  const LN_PhysicsQueryResult defaults;
  EXPECT_EQ(defaults.hit_normal, MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(defaults.ray_direction, MT_Vector3(0.0f, 0.0f, 0.0f));
}

TEST(LN_RuntimeTree, PhysicsQueryObjectIsNullOnMissAndAfterObjectRemoval)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::MouseOver;
  const uint32_t query_index = program->AddQueryExpression(query);
  const uint32_t query_state_index =
      program->GetQueryExpressions()[query_index].runtime_state_index;

  LN_ValueExpression object_expression;
  object_expression.kind = LN_ValueExpressionKind::PhysicsQueryObject;
  object_expression.input0 = query_index;
  const uint32_t object_index = program->AddValueExpression(object_expression);

  KX_GameObject hit_object;
  hit_object.SetName("MouseOverHit");
  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  LN_QueryResult seeded;
  seeded.physics_query.hit = true;
  seeded.physics_query.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
  seeded.physics_query.object_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree,
                                                                            hit_object);

  LN_TickContext context;
  context.tick_index = 81;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, seeded);
  const LN_Value hit_value = LN_RuntimeTreeTestAccess::ResolveValue(
      runtime_tree, object_index, context);
  EXPECT_TRUE(hit_value.exists);
  EXPECT_EQ(hit_value.type, LN_ValueType::ObjectRef);
  EXPECT_EQ(hit_value.reference_name, "MouseOverHit");

  seeded.physics_query.hit = false;
  seeded.physics_query.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
  context.tick_index++;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, seeded);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveValue(runtime_tree, object_index, context).exists);

  seeded.physics_query.hit = true;
  seeded.physics_query.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
  runtime_tree.InvalidateObjectRef(&hit_object);
  context.tick_index++;
  LN_RuntimeTreeTestAccess::SeedQueryResult(
      runtime_tree, query_state_index, context.tick_index, seeded);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveValue(runtime_tree, object_index, context).exists);
}

TEST(LN_RuntimeTree, IgnoreObjectListUnionsDeduplicatesAndSkipsInvalidEntries)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7a3), 0, 0);
  KX_GameObject first;
  first.SetName("FirstIgnored");
  KX_GameObject second;
  second.SetName("SecondIgnored");
  KX_GameObject stale;
  stale.SetName("StaleIgnored");
  const LN_RuntimeRef first_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree, first);
  const LN_RuntimeRef second_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree, second);
  const LN_RuntimeRef stale_ref = LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree, stale);

  const uint32_t single_index = AddObjectRefConstant(*program, first_ref);
  LN_Value ignore_list;
  ignore_list.type = LN_ValueType::List;
  ignore_list.exists = true;
  const auto append_object = [&](const LN_RuntimeRef &ref) {
    LN_Value item;
    item.type = LN_ValueType::ObjectRef;
    item.exists = true;
    item.runtime_ref = ref;
    ignore_list.list_value.push_back(item);
  };
  append_object(first_ref);
  append_object(second_ref);
  append_object(second_ref);
  append_object(stale_ref);
  LN_Value invalid;
  invalid.type = LN_ValueType::Float;
  invalid.exists = true;
  invalid.float_value = 1.0f;
  ignore_list.list_value.push_back(invalid);
  ignore_list.list_value.push_back(LN_Value());

  LN_ValueExpression list_expression;
  list_expression.kind = LN_ValueExpressionKind::Constant;
  list_expression.value = ignore_list;
  const uint32_t list_index = program->AddValueExpression(list_expression);
  runtime_tree.InvalidateObjectRef(&stale);

  LN_TickContext context;
  context.tick_index = 74;
  const std::vector<LN_RuntimeRef> refs = LN_RuntimeTreeTestAccess::ResolveIgnoreObjectRefs(
      runtime_tree, single_index, list_index, context);
  ASSERT_EQ(refs.size(), 2u);
  EXPECT_EQ(runtime_tree.ResolveObjectRef(refs[0]), &first);
  EXPECT_EQ(runtime_tree.ResolveObjectRef(refs[1]), &second);
}

TEST(LN_RuntimeTree, ShapeCastRejectsOverflowedDerivedDistanceButAllowsOverlapQueries)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::ShapeCast;
  query.shape_cast_type = LN_ShapeCastType::Sphere;
  query.vector_value = MT_Vector3(std::numeric_limits<float>::max(), 0.0f, 0.0f);
  query.secondary_vector_value = MT_Vector3(-std::numeric_limits<float>::max(), 0.0f, 0.0f);
  query.float_value = 0.5f;
  query.secondary_float_value = 2.0f;
  const uint32_t query_index = program->AddQueryExpression(query);

  LN_RuntimeTree runtime_tree(program, nullptr, 0, 0);
  LN_TickContext context;
  context.tick_index = 75;
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ResolveQuery(runtime_tree, query_index, context)
                .physics_query.diagnostic_status,
            LN_QueryDiagnosticStatus::InvalidTarget);

  program = std::make_shared<LN_Program>();
  query = LN_QueryExpression();
  query.kind = LN_QueryExpressionKind::ShapeCast;
  query.shape_cast_type = LN_ShapeCastType::Sphere;
  query.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  query.secondary_vector_value = query.vector_value;
  query.float_value = 0.5f;
  query.secondary_float_value = 2.0f;
  const uint32_t overlap_query_index = program->AddQueryExpression(query);
  LN_RuntimeTree overlap_runtime_tree(program, nullptr, 0, 0);
  context.tick_index++;
  EXPECT_EQ(
      LN_RuntimeTreeTestAccess::ResolveQuery(overlap_runtime_tree, overlap_query_index, context)
          .physics_query.diagnostic_status,
      LN_QueryDiagnosticStatus::MissingPhysicsWorld);
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
  EXPECT_EQ(missing_world.ray_origin, MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(missing_world.ray_direction, MT_Vector3(0.0f, 0.0f, 1.0f));
  EXPECT_EQ(missing_world.end_position, MT_Vector3(0.0f, 0.0f, 1.0f));
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

TEST(LN_RuntimeTree, UICompletionQueueNeverRefusesCommands)
{
  /* A menu toggled many times must keep working whether or not its result outputs are wired:
   * unread results are not queued, and a full queue keeps the newest results. */
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_BoolExpression observed;
  observed.kind = LN_BoolExpressionKind::UICommandCompleted;
  observed.bool_value = true;
  LN_BoolExpression unobserved = observed;
  unobserved.bool_value = false;
  const uint32_t observed_index = program->AddBoolExpression(observed);
  const uint32_t unobserved_index = program->AddBoolExpression(unobserved);
  LN_BoolExpression other;
  other.kind = LN_BoolExpressionKind::Constant;
  const uint32_t other_index = program->AddBoolExpression(other);
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7a), 0, 0);

  for (int32_t handle = 1; handle <= 200; handle++) {
    EXPECT_TRUE(runtime_tree.CanCompleteUIOperation(observed_index));
    EXPECT_TRUE(runtime_tree.CompleteUIOperation(observed_index, LN_UIResult::Success, handle));
    EXPECT_TRUE(runtime_tree.CanCompleteUIOperation(unobserved_index));
    EXPECT_TRUE(runtime_tree.CompleteUIOperation(unobserved_index, LN_UIResult::Success, handle));
  }
  EXPECT_FALSE(runtime_tree.CanCompleteUIOperation(other_index));
  EXPECT_FALSE(runtime_tree.CompleteUIOperation(other_index, LN_UIResult::Success, 1));
}

TEST(LN_RuntimeTree, ActionLifecycleEventsAreExactOrderedOneTickPulses)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_BoolExpression lifecycle;
  lifecycle.kind = LN_BoolExpressionKind::ActionLifecycleEvent;
  const uint32_t lifecycle_index = program->AddBoolExpression(lifecycle);

  const auto add_result = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.input0 = lifecycle_index;
    return program->AddBoolExpression(expression);
  };
  const uint32_t started_index = add_result(LN_BoolExpressionKind::ActionStarted);
  const uint32_t completed_index = add_result(LN_BoolExpressionKind::ActionCompleted);
  const uint32_t interrupted_index = add_result(LN_BoolExpressionKind::ActionInterrupted);
  const uint32_t failed_index = add_result(LN_BoolExpressionKind::ActionFailed);

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7b), 0, 0);
  EXPECT_TRUE(runtime_tree.CompleteActionOperation(lifecycle_index, LN_ActionResult::Started));
  EXPECT_TRUE(runtime_tree.CompleteActionOperation(lifecycle_index, LN_ActionResult::Completed));

  LN_TickContext context;
  context.tick_index = 10;
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, started_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, completed_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, interrupted_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, failed_index, context));

  context.tick_index++;
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, started_index, context));
  EXPECT_TRUE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, completed_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, interrupted_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, failed_index, context));

  context.tick_index++;
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, started_index, context));
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::ResolveBool(runtime_tree, completed_index, context));
}

TEST(LN_RuntimeTree, ActionLifecycleQueueIsBounded)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_BoolExpression lifecycle;
  lifecycle.kind = LN_BoolExpressionKind::ActionLifecycleEvent;
  const uint32_t lifecycle_index = program->AddBoolExpression(lifecycle);
  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(0x7c), 0, 0);

  for (int index = 0; index < 64; index++) {
    EXPECT_TRUE(runtime_tree.CompleteActionOperation(lifecycle_index, LN_ActionResult::Started));
  }
  EXPECT_FALSE(runtime_tree.CompleteActionOperation(lifecycle_index, LN_ActionResult::Failed));
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

TEST(LN_RuntimeTree, ObjectGamePropertyReadsTargetAndFallsBackToOwner)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  KX_GameObject target;
  LN_RuntimeTree runtime_tree(program, &owner, 1, 0);

  EXP_IntValue *target_property = new EXP_IntValue(0);
  target.SetProperty("exit", target_property);
  target_property->Release();
  EXP_IntValue *owner_property = new EXP_IntValue(7);
  owner.SetProperty("owner_only", owner_property);
  owner_property->Release();

  const uint32_t target_object = AddObjectRefConstant(
      *program, LN_RuntimeTreeTestAccess::MakeObjectRef(runtime_tree, target));
  const uint32_t exit_name = AddStringConstant(*program, "exit");
  const uint32_t owner_name = AddStringConstant(*program, "owner_only");
  const uint32_t missing_name = AddStringConstant(*program, "missing");

  LN_ValueExpression target_read;
  target_read.kind = LN_ValueExpressionKind::ObjectGameProperty;
  target_read.input0 = target_object;
  target_read.input1 = exit_name;
  const uint32_t target_read_index = program->AddValueExpression(target_read);

  LN_ValueExpression owner_read;
  owner_read.kind = LN_ValueExpressionKind::ObjectGameProperty;
  owner_read.input1 = owner_name;
  const uint32_t owner_read_index = program->AddValueExpression(owner_read);

  LN_ValueExpression missing_read;
  missing_read.kind = LN_ValueExpressionKind::ObjectGameProperty;
  missing_read.input0 = target_object;
  missing_read.input1 = missing_name;
  const uint32_t missing_read_index = program->AddValueExpression(missing_read);

  std::vector<std::string> validation_errors;
  ASSERT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  EXPECT_FALSE(program->IsParallelEligible());
  EXPECT_EQ(program->GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::QueryExpression);
  EXPECT_NE(program->GetSchedulerSummary().resource_access & LN_SCHEDULER_RESOURCE_SCENE, 0u);

  LN_TickContext context;
  const LN_Value target_value = LN_RuntimeTreeTestAccess::ResolveValue(
      runtime_tree, target_read_index, context);
  EXPECT_TRUE(target_value.exists);
  EXPECT_EQ(target_value.type, LN_ValueType::Int);
  EXPECT_EQ(target_value.int_value, 0);

  const LN_Value owner_value = LN_RuntimeTreeTestAccess::ResolveValue(
      runtime_tree, owner_read_index, context);
  EXPECT_TRUE(owner_value.exists);
  EXPECT_EQ(owner_value.type, LN_ValueType::Int);
  EXPECT_EQ(owner_value.int_value, 7);

  const LN_Value missing_value = LN_RuntimeTreeTestAccess::ResolveValue(
      runtime_tree, missing_read_index, context);
  EXPECT_FALSE(missing_value.exists);
  EXPECT_EQ(missing_value.type, LN_ValueType::None);

  runtime_tree.InvalidateObjectRef(&target);
  const LN_Value stale_value = LN_RuntimeTreeTestAccess::ResolveValue(
      runtime_tree, target_read_index, context);
  EXPECT_FALSE(stale_value.exists);
  EXPECT_EQ(stale_value.type, LN_ValueType::None);
}

TEST(LN_RuntimeTree, CollisionExitPayloadUsesLifetimeSafeObjectRefs)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  KX_GameObject hit_object;
  LN_RuntimeTree runtime_tree(program, &owner, 1, 0);

  constexpr uint64_t hit_tick = 10;
  constexpr uint64_t exit_tick = 11;
  LN_RuntimeTreeTestAccess::StoreCollisionPayload(runtime_tree, &owner, &hit_object, hit_tick);
  EXPECT_FALSE(LN_RuntimeTreeTestAccess::PersistentPayloadContainsRawObjectPointers(runtime_tree));

  LN_RuntimeTreeTestAccess::MarkCollisionExit(runtime_tree, &owner, exit_tick);
  EXPECT_EQ(LN_RuntimeTreeTestAccess::ExitHitObject(runtime_tree, &owner, exit_tick), &hit_object);
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

TEST(LN_RuntimeTree, InventoryAndListHelpersComputeTheirOutputs)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  const auto constant = [&](const LN_Value &value) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::Constant;
    expression.value = value;
    return program->AddValueExpression(expression);
  };
  const auto text = [](const std::string &string) {
    LN_Value value;
    value.type = LN_ValueType::String;
    value.exists = true;
    value.string_value = string;
    return value;
  };
  const auto list = [](std::vector<LN_Value> items) {
    LN_Value value;
    value.type = LN_ValueType::List;
    value.exists = true;
    value.list_value = std::move(items);
    return value;
  };
  LN_Value three;
  three.type = LN_ValueType::Int;
  three.exists = true;
  three.int_value = 3;
  LN_Value one = three;
  one.int_value = 1;
  LN_Value five_coins = three;
  five_coins.int_value = 5;
  LN_Value truth;
  truth.type = LN_ValueType::Bool;
  truth.exists = true;
  truth.bool_value = true;

  const uint32_t bag = constant(
      list({LN_MakeInventoryEntry("apple", 2), LN_MakeInventoryEntry("sword", 1)}));
  const uint32_t apple = constant(text("apple"));
  const uint32_t yes = constant(truth);
  const auto inventory = [&](const LN_InventoryOperation operation,
                             const LN_ValueExpressionKind kind,
                             std::vector<std::pair<LN_InventoryInput, uint32_t>> inputs) {
    LN_ValueExpression expression;
    expression.kind = kind;
    expression.input0 = bag;
    expression.value.type = LN_ValueType::Int;
    expression.value.int_value = int32_t(operation);
    expression.input_indices.assign(LN_INVENTORY_INPUT_COUNT, LN_INVALID_INDEX);
    for (const auto &[input, index] : inputs) {
      expression.input_indices[input] = index;
    }
    return program->AddValueExpression(expression);
  };
  const uint32_t five = AddIntValueConstant(*program, 5);
  const uint32_t four = AddIntValueConstant(*program, 4);
  const uint32_t two = AddIntValueConstant(*program, 2);
  const std::vector<std::pair<LN_InventoryInput, uint32_t>> add_inputs = {
      {LN_INVENTORY_INPUT_ITEM, apple},
      {LN_INVENTORY_INPUT_AMOUNT, five},
      {LN_INVENTORY_INPUT_MAX_STACK, four},
      {LN_INVENTORY_INPUT_STACK, yes}};
  std::vector<std::pair<LN_InventoryInput, uint32_t>> full_inputs = add_inputs;
  full_inputs.push_back({LN_INVENTORY_INPUT_CAPACITY, two});

  LN_ValueExpression sort;
  sort.kind = LN_ValueExpressionKind::ListSort;
  sort.input0 = constant(list({three, text("b"), one, text("a")}));
  const uint32_t sorted = program->AddValueExpression(sort);
  sort.input2 = yes;
  const uint32_t sorted_descending = program->AddValueExpression(sort);
  LN_ValueExpression sort_by_key;
  sort_by_key.kind = LN_ValueExpressionKind::ListSort;
  sort_by_key.input0 = bag;
  sort_by_key.input1 = constant(text("count"));
  LN_ValueExpression find = sort;
  find.kind = LN_ValueExpressionKind::ListFindIndex;
  find.input1 = constant(text("a"));
  LN_ValueExpression find_missing = find;
  find_missing.input1 = five;
  LN_ValueExpression count;
  count.kind = LN_ValueExpressionKind::InventoryItemCount;
  count.input0 = bag;
  count.input1 = apple;
  /* A slot from the inventory names its item too. */
  LN_ValueExpression count_by_slot = count;
  count_by_slot.input1 = constant(LN_MakeInventoryEntry("apple", 9));

  LN_Value coins;
  coins.type = LN_ValueType::Dict;
  coins.exists = true;
  coins.dict_value["coins"] = five_coins;
  LN_ValueExpression add_coins;
  add_coins.kind = LN_ValueExpressionKind::DictAddValue;
  add_coins.input0 = constant(coins);
  add_coins.input1 = AddStringConstant(*program, "coins");
  add_coins.input2 = AddFloatValueConstant(*program, 2.0f);
  LN_ValueExpression add_gems = add_coins;
  add_gems.input0 = program->AddValueExpression(add_coins);
  add_gems.input1 = AddStringConstant(*program, "gems");
  add_gems.input2 = AddFloatValueConstant(*program, 3.0f);

  const std::vector<std::pair<uint32_t, std::string>> expected = {
      /* Tops up the apple stack to 4, then a new stack of 3. */
      {inventory(LN_InventoryOperation::AddItem, LN_ValueExpressionKind::InventoryEdit, add_inputs),
       "[{count: 4, item: apple}, {count: 1, item: sword}, {count: 3, item: apple}]"},
      {inventory(
           LN_InventoryOperation::AddItem, LN_ValueExpressionKind::InventoryAmount, add_inputs),
       "0"},
      /* Two slots at most: 3 apples do not fit. */
      {inventory(
           LN_InventoryOperation::AddItem, LN_ValueExpressionKind::InventoryAmount, full_inputs),
       "3"},
      {inventory(LN_InventoryOperation::RemoveItem,
                 LN_ValueExpressionKind::InventoryAmount,
                 {{LN_INVENTORY_INPUT_ITEM, apple}, {LN_INVENTORY_INPUT_AMOUNT, five}}),
       "2"},
      {inventory(LN_InventoryOperation::RemoveItem,
                 LN_ValueExpressionKind::InventoryEdit,
                 {{LN_INVENTORY_INPUT_ITEM, apple}, {LN_INVENTORY_INPUT_AMOUNT, five}}),
       "[{count: 1, item: sword}]"},
      {inventory(LN_InventoryOperation::Sort,
                 LN_ValueExpressionKind::InventoryEdit,
                 {{LN_INVENTORY_INPUT_BY_COUNT, yes}}),
       "[{count: 1, item: sword}, {count: 2, item: apple}]"},
      {inventory(LN_InventoryOperation::SwapSlots,
                 LN_ValueExpressionKind::InventoryEdit,
                 {{LN_INVENTORY_INPUT_SLOT, AddIntValueConstant(*program, 0)},
                  {LN_INVENTORY_INPUT_OTHER_SLOT, AddIntValueConstant(*program, 1)}}),
       "[{count: 1, item: sword}, {count: 2, item: apple}]"},
      {program->AddValueExpression(count), "2"},
      {program->AddValueExpression(count_by_slot), "2"},
      /* Numbers before text in both directions. */
      {sorted, "[1, 3, a, b]"},
      {sorted_descending, "[3, 1, b, a]"},
      {program->AddValueExpression(sort_by_key),
       "[{count: 1, item: sword}, {count: 2, item: apple}]"},
      {program->AddValueExpression(find), "3"},
      {program->AddValueExpression(find_missing), "-1"},
      /* Whole amounts keep counts as integers. */
      {program->AddValueExpression(add_gems), "{coins: 7, gems: 3}"},
  };
  for (const auto &[value_index, message] : expected) {
    LN_Instruction print;
    print.opcode = LN_OpCode::Print;
    print.value_expr_index = value_index;
    program->AddInstruction(LN_Event::OnFixedUpdate, print);
  }

  LN_RuntimeTree runtime_tree(program, FakeGameObjectPointer(65), 0, 0);
  LN_TickContext context;
  context.tick_index = 1;
  const std::vector<LN_CommandBuffer::Command> commands = RecordReadyCommands(runtime_tree,
                                                                              context);
  ASSERT_EQ(commands.size(), expected.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(commands[index].type, LN_CommandBuffer::CommandType::Print);
    EXPECT_EQ(commands[index].property_name, expected[index].second) << "print " << index;
  }
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

/* The Inventory node follows the Inventory Rules of the list it changes; Override uses its own
 * inputs instead. What the list may hold (Accept Categories) applies either way. */
TEST(LN_RuntimeTree, InventoryNodeFollowsTheListsRulesUnlessOverridden)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  KX_GameObject owner;
  LN_RuntimeTree runtime_tree(program, &owner, 1, 0);
  const auto constant = [&](const LN_Value &value) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::Constant;
    expression.value = value;
    return program->AddValueExpression(expression);
  };
  LN_Value gem = LN_MakeInventoryEntry("Gem", 1);
  LN_Value treasure_text;
  treasure_text.type = LN_ValueType::String;
  treasure_text.exists = true;
  treasure_text.string_value = "treasure";
  gem.dict_value["category"] = treasure_text;
  LN_Value yes;
  yes.type = LN_ValueType::Bool;
  yes.exists = true;
  yes.bool_value = true;
  LN_Value no = yes;
  no.bool_value = false;
  const auto add_item = [&](const bool override_rules) {
    LN_ValueExpression edit;
    edit.kind = LN_ValueExpressionKind::InventoryEdit;
    edit.value.type = LN_ValueType::Int;
    edit.value.int_value = int32_t(LN_InventoryOperation::AddItem);
    edit.input_indices.assign(LN_INVENTORY_INPUT_COUNT, LN_INVALID_INDEX);
    edit.input_indices[LN_INVENTORY_INPUT_ITEM] = constant(gem);
    edit.input_indices[LN_INVENTORY_INPUT_AMOUNT] = AddIntValueConstant(*program, 3);
    /* The node's own rules: Stack on, no Capacity. */
    edit.input_indices[LN_INVENTORY_INPUT_STACK] = constant(yes);
    edit.input_indices[LN_INVENTORY_INPUT_CAPACITY] = AddIntValueConstant(*program, 0);
    edit.input_indices[LN_INVENTORY_INPUT_OVERRIDE] = constant(override_rules ? yes : no);
    return edit;
  };
  /* The list's rules: no stacking, one place. */
  LN_InventoryRules list_rules;
  list_rules.stack = false;
  list_rules.capacity = 1;

  std::vector<LN_Value> slots;
  auto [leftover, outcome] = LN_RuntimeTreeTestAccess::RunInventoryEdit(
      runtime_tree, add_item(false), slots, &list_rules);
  EXPECT_EQ(leftover, 2);
  EXPECT_EQ(outcome, LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(slots.size(), 1u);

  slots.clear();
  std::tie(leftover, outcome) = LN_RuntimeTreeTestAccess::RunInventoryEdit(
      runtime_tree, add_item(true), slots, &list_rules);
  EXPECT_EQ(leftover, 0);
  EXPECT_EQ(outcome, LN_InventoryOutcome::None);
  ASSERT_EQ(slots.size(), 1u);
  std::string item;
  int32_t count = 0;
  ASSERT_TRUE(LN_InventoryEntry(slots[0], item, count));
  EXPECT_EQ(count, 3);

  /* A list for weapons refuses the gem, with Override too: nothing goes in. */
  list_rules.accept_categories = LN_NormalizedCategories("Weapon");
  for (const bool override_rules : {false, true}) {
    slots.clear();
    std::tie(leftover, outcome) = LN_RuntimeTreeTestAccess::RunInventoryEdit(
        runtime_tree, add_item(override_rules), slots, &list_rules);
    EXPECT_EQ(leftover, 3);
    EXPECT_EQ(outcome, LN_InventoryOutcome::Refused);
    EXPECT_TRUE(slots.empty());
  }
  /* A list value (no object): the node's own rules. */
  slots.clear();
  std::tie(leftover, outcome) = LN_RuntimeTreeTestAccess::RunInventoryEdit(
      runtime_tree, add_item(false), slots, nullptr);
  EXPECT_EQ(leftover, 0);
}
