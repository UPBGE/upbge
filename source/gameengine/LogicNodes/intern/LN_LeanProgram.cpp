/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "LN_LeanProgram.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <sstream>

#include "LN_KindCatalog.h"
#include "LN_Program.h"
#include "LN_TreeBatch.h"

namespace {

std::atomic<int> g_mode_override{-1};
thread_local bool g_lean_helper_thread = false;

/** Lean tree runs are counted per thread slot (each on its own cache line, written without a
 * locked instruction): one shared counter added to by every helper of a tree batch bounced its
 * line between cores at every tree (plan A18). A thread takes its slot at its first run; more than
 * 64 threads would share slots and may lose counts, which only tests read. */
struct alignas(64) RunCounterSlot {
  std::atomic<uint64_t> count{0};
};
constexpr uint32_t kRunCounterSlots = 64;
RunCounterSlot g_run_counters[kRunCounterSlots];
std::atomic<uint32_t> g_next_run_slot{0};
thread_local uint32_t g_run_slot = UINT32_MAX;
/** Route runs, counted the same way. */
RunCounterSlot g_route_counters[kRunCounterSlots];

std::atomic<int> g_route_mode_override{-1};

LN_RouteMode route_mode_from_environment()
{
  static const LN_RouteMode mode = []() {
    const char *value = std::getenv("UPBGE_LN_ROUTES");
    if (value == nullptr || value[0] == '\0') {
      return LN_RouteMode::On; /* since plan phase A33's gate */
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
        std::strcmp(value, "OFF") == 0)
    {
      return LN_RouteMode::Off;
    }
    return LN_RouteMode::On;
  }();
  return mode;
}

/** The lean memo (LN_LeanProgram::memo_*): which expressions the lean reader keeps for the tick.
 * Families: 0 Bool, 1 Int, 2 Float, 3 Vector, 4 Color, 5 String, 6 Value. */
void BuildLeanMemo(const LN_Program &program, LN_LeanProgram &lean)
{
  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  constexpr uint32_t kFamilies = 7;
  const std::array<size_t, kFamilies> sizes = {program.GetBoolExpressions().size(),
                                               program.GetIntExpressions().size(),
                                               program.GetFloatExpressions().size(),
                                               program.GetVectorExpressions().size(),
                                               program.GetColorExpressions().size(),
                                               program.GetStringExpressions().size(),
                                               program.GetValueExpressions().size()};
  const std::array<const std::vector<uint8_t> *, kFamilies> classes = {&ir.bool_memo_class,
                                                                       &ir.int_memo_class,
                                                                       &ir.float_memo_class,
                                                                       &ir.vector_memo_class,
                                                                       &ir.color_memo_class,
                                                                       &ir.string_memo_class,
                                                                       &ir.value_memo_class};
  const auto family_of = [](const LN_RegisterValueKind kind) -> uint32_t {
    switch (kind) {
      case LN_RegisterValueKind::Bool:
        return 0;
      case LN_RegisterValueKind::Int:
        return 1;
      case LN_RegisterValueKind::Float:
        return 2;
      case LN_RegisterValueKind::Vector:
        return 3;
      case LN_RegisterValueKind::Color:
        return 4;
      case LN_RegisterValueKind::String:
      case LN_RegisterValueKind::StringId:
        return 5;
      case LN_RegisterValueKind::Object:
      case LN_RegisterValueKind::GenericValue:
        return 6;
    }
    return 6;
  };
  const auto kind_value = [&](const uint32_t family, const uint32_t index) -> uint16_t {
    switch (family) {
      case 0:
        return uint16_t(program.GetBoolExpressions()[index].kind);
      case 1:
        return uint16_t(program.GetIntExpressions()[index].kind);
      case 2:
        return uint16_t(program.GetFloatExpressions()[index].kind);
      case 3:
        return uint16_t(program.GetVectorExpressions()[index].kind);
      case 4:
        return uint16_t(program.GetColorExpressions()[index].kind);
      case 5:
        return uint16_t(program.GetStringExpressions()[index].kind);
      default:
        return uint16_t(program.GetValueExpressions()[index].kind);
    }
  };
  const auto inputs_of = [&](const LN_RegisterExpressionOp &op, const auto &visit) {
    for (const LN_RegisterExpressionRef *ref : {&op.input0, &op.input1, &op.input2, &op.input3}) {
      if (ref->expression_index != LN_INVALID_INDEX) {
        visit(*ref);
      }
    }
    for (uint32_t ref_index = op.variable_ref_start;
         ref_index < op.variable_ref_start + op.variable_ref_count &&
         ref_index < ir.variable_refs.size();
         ref_index++)
    {
      if (ir.variable_refs[ref_index].expression_index != LN_INVALID_INDEX) {
        visit(ir.variable_refs[ref_index]);
      }
    }
  };

  /* Each expression's register op, and how often expressions and native ops read it. */
  std::array<std::vector<uint32_t>, kFamilies> op_of;
  std::array<std::vector<uint32_t>, kFamilies> uses;
  for (uint32_t family = 0; family < kFamilies; family++) {
    op_of[family].assign(sizes[family], LN_INVALID_INDEX);
    uses[family].assign(sizes[family], 0);
  }
  const auto use = [&](const uint32_t family, const uint32_t index) {
    if (index < sizes[family]) {
      uses[family][index]++;
    }
  };
  for (uint32_t op_index = 0; op_index < ir.ops.size(); op_index++) {
    const LN_RegisterExpressionOp &op = ir.ops[op_index];
    const uint32_t family = family_of(op.output_kind);
    if (op.expression_index < sizes[family]) {
      op_of[family][op.expression_index] = op_index;
    }
    inputs_of(op, [&](const LN_RegisterExpressionRef &ref) {
      use(family_of(ref.value_kind), ref.expression_index);
    });
  }
  for (size_t event_index = 0; event_index < lean.events.size(); event_index++) {
    const LN_Event event = LN_Event(event_index);
    const LN_ExecBlockProgram &exec = program.GetExecBlockProgram(event);
    const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(event);
    for (uint32_t op_index = 0; op_index < exec.ops.size(); op_index++) {
      const LN_ExecOp &op = exec.ops[op_index];
      use(0, op.guard_bool_expr_index);
      if (op.instruction_index >= headers.size() || op_index >= lean.events[event_index].ops.size())
      {
        continue;
      }
      const LN_Instruction &instruction = program.GetInstructionPayload(event,
                                                                        headers[op.instruction_index]);
      switch (LN_LeanNative(lean.events[event_index].ops[op_index].native)) {
        case LN_LeanNative::BranchRoute:
          use(0, instruction.bool_expr_index);
          break;
        case LN_LeanNative::SetNumber:
        case LN_LeanNative::ModifyProperty:
          if (instruction.command_payload_index < program.GetGamePropertyCommandPayloads().size()) {
            const LN_GamePropertyCommandPayload &payload =
                program.GetGamePropertyCommandPayloads()[instruction.command_payload_index];
            use(0, payload.bool_expr_index);
            use(1, payload.int_expr_index);
            use(2, payload.float_expr_index);
            use(2, payload.modify_min_expr_index);
            use(2, payload.modify_max_expr_index);
          }
          break;
        case LN_LeanNative::VectorCommand:
          if (instruction.command_payload_index < program.GetVectorCommandPayloads().size()) {
            const LN_VectorCommandPayload &payload =
                program.GetVectorCommandPayloads()[instruction.command_payload_index];
            use(3, payload.vector_expr_index);
            use(3, payload.secondary_vector_expr_index);
            use(0, payload.bool_expr_index);
          }
          break;
        default:
          break;
      }
    }
  }

  /* Pure all the way down: a constant, or a kind the memo does not taint whose register op lists
   * its inputs, every one of them pure. 0 unknown, 1 pure, 2 not, 3 being looked at (a loop in
   * the graph: not). */
  std::array<std::vector<uint8_t>, kFamilies> purity;
  for (uint32_t family = 0; family < kFamilies; family++) {
    purity[family].assign(sizes[family], 0);
  }
  std::function<bool(uint32_t, uint32_t)> pure = [&](const uint32_t family,
                                                     const uint32_t index) -> bool {
    if (index >= sizes[family]) {
      return false;
    }
    uint8_t &state = purity[family][index];
    if (state != 0) {
      return state == 1;
    }
    state = 3;
    bool result = false;
    const uint8_t memo_class = index < classes[family]->size() ? (*classes[family])[index] :
                                                                 uint8_t(LN_MEMO_TAINTS);
    if (memo_class != uint8_t(LN_MEMO_TAINTS)) {
      if (kind_value(family, index) == 0) {
        result = true; /* Constant */
      }
      else if (op_of[family][index] != LN_INVALID_INDEX &&
               ir.ops[op_of[family][index]].kind != LN_RegisterExpressionOpKind::Fallback)
      {
        result = true;
        inputs_of(ir.ops[op_of[family][index]], [&](const LN_RegisterExpressionRef &ref) {
          result = result && pure(family_of(ref.value_kind), ref.expression_index);
        });
      }
    }
    state = result ? 1 : 2;
    return result;
  };
  const auto keep = [&](const uint32_t family, const uint32_t index, const bool single_read) {
    return index < classes[family]->size() &&
           (*classes[family])[index] == uint8_t(LN_MEMO_STORE) && uses[family][index] >= 2 &&
           !single_read && pure(family, index);
  };
  lean.memo_bool.assign(sizes[0], 0);
  lean.memo_int.assign(sizes[1], 0);
  lean.memo_float.assign(sizes[2], 0);
  lean.memo_vector.assign(sizes[3], 0);
  for (uint32_t index = 0; index < sizes[0]; index++) {
    lean.memo_bool[index] = keep(0,
                                 index,
                                 program.GetBoolExpressions()[index].kind ==
                                     LN_BoolExpressionKind::SnapshotGameProperty) ?
                                1 :
                                0;
  }
  for (uint32_t index = 0; index < sizes[1]; index++) {
    lean.memo_int[index] = keep(1,
                                index,
                                program.GetIntExpressions()[index].kind ==
                                    LN_IntExpressionKind::SnapshotGameProperty) ?
                               1 :
                               0;
  }
  for (uint32_t index = 0; index < sizes[2]; index++) {
    lean.memo_float[index] = keep(2,
                                  index,
                                  program.GetFloatExpressions()[index].kind ==
                                      LN_FloatExpressionKind::SnapshotGameProperty) ?
                                 1 :
                                 0;
  }
  for (uint32_t index = 0; index < sizes[3]; index++) {
    lean.memo_vector[index] = keep(3, index, false) ? 1 : 0;
  }
  lean.memo_count = 0;
  for (const std::vector<uint8_t> *memo :
       {&lean.memo_bool, &lean.memo_int, &lean.memo_float, &lean.memo_vector})
  {
    lean.memo_count += uint32_t(std::count(memo->begin(), memo->end(), uint8_t(1)));
  }
}

/** Makes `event`'s compiled route (LN_LeanRoute) when every op of it has a native handler and the
 * event has no Loop, no UI event route and no Delay-Done guard: the ops of `ir` one for one, in
 * the same order and the same blocks, with what each handler looks up at every run worked out
 * here. Anything it cannot hold leaves the event without a route (the block walk runs it). */
void BuildRoute(const LN_Program &program,
                const LN_Event event_kind,
                const LN_ExecBlockProgram &ir,
                const uint32_t state_offset,
                LN_LeanEvent &event)
{
  event.has_route = false;
  event.route = LN_LeanRoute();
  if (!event.fully_native || ir.ops.empty() || !ir.ui_event_states.empty()) {
    return;
  }
  for (const LN_ExecBlock &block : ir.blocks) {
    if (block.loop_frame_index != LN_INVALID_INDEX ||
        block.completed_loop_frame_index != LN_INVALID_INDEX ||
        block.first_op + block.op_count > ir.ops.size())
    {
      return;
    }
  }
  const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(event_kind);
  const std::vector<LN_BoolExpression> &bools = program.GetBoolExpressions();
  const std::vector<LN_GamePropertyCommandPayload> &property_payloads =
      program.GetGamePropertyCommandPayloads();
  const std::vector<LN_VectorCommandPayload> &vector_payloads =
      program.GetVectorCommandPayloads();
  LN_LeanRoute route;
  route.ops.resize(ir.ops.size());
  for (uint32_t op_index = 0; op_index < ir.ops.size(); op_index++) {
    const LN_ExecOp &exec = ir.ops[op_index];
    const LN_LeanOp &lean_op = event.ops[op_index];
    LN_RouteOp &op = route.ops[op_index];
    if (lean_op.guard_is_delay_done || exec.ui_event_state_index != LN_INVALID_INDEX ||
        exec.instruction_index >= headers.size())
    {
      return;
    }
    op.native = lean_op.native;
    op.state_index = lean_op.state_index;
    op.constant_index = lean_op.constant_index;
    op.instruction_index = exec.instruction_index;
    op.instruction = &program.GetInstructionPayload(event_kind, headers[exec.instruction_index]);
    switch (LN_LeanNative(op.native)) {
      case LN_LeanNative::Nop:
        break;
      case LN_LeanNative::BranchRoute:
        if (exec.kind != LN_ExecOpKind::BranchRoute) {
          return;
        }
        break;
      case LN_LeanNative::SetNumber:
      case LN_LeanNative::ModifyProperty:
        if (op.instruction->command_payload_index >= property_payloads.size()) {
          return;
        }
        op.property_payload = &property_payloads[op.instruction->command_payload_index];
        break;
      case LN_LeanNative::VectorCommand:
        if (op.instruction->command_payload_index >= vector_payloads.size()) {
          return;
        }
        op.vector_payload = &vector_payloads[op.instruction->command_payload_index];
        break;
      case LN_LeanNative::Generic:
        return;
    }
    if (exec.guard_bool_expr_index == LN_INVALID_INDEX) {
      op.guard = LN_RouteGuard::None;
    }
    else if (exec.guard_bool_expr_index < bools.size() &&
             bools[exec.guard_bool_expr_index].kind == LN_BoolExpressionKind::InstructionExecuted)
    {
      /* InstructionExecutedNow's index: the event's offset plus the instruction. */
      op.guard = LN_RouteGuard::Ran;
      op.guard_state = state_offset + bools[exec.guard_bool_expr_index].input0;
    }
    else {
      op.guard = LN_RouteGuard::Generic;
      op.guard_expr = exec.guard_bool_expr_index;
    }
  }
  for (const LN_ExecBlock &block : ir.blocks) {
    LN_RouteBlock route_block;
    route_block.first_op = block.first_op;
    route_block.op_count = block.op_count;
    route_block.step_gate_index = block.step_gate_index;
    route.blocks.push_back(route_block);
  }
  event.route = std::move(route);
  event.has_route = true;
}

LN_LeanMode mode_from_environment()
{
  static const LN_LeanMode mode = []() {
    const char *value = std::getenv("UPBGE_LN_LEAN");
    /* Unset: Auto since plan phase A27 (the lean VM for the events whose ops are all native). */
    if (value == nullptr || value[0] == '\0') {
      return LN_LeanMode::Auto;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
        std::strcmp(value, "OFF") == 0)
    {
      return LN_LeanMode::Off;
    }
    if (std::strcmp(value, "force") == 0 || std::strcmp(value, "FORCE") == 0 ||
        std::strcmp(value, "2") == 0)
    {
      return LN_LeanMode::Force;
    }
    return LN_LeanMode::Auto; /* "auto", "1", anything else asked for */
  }();
  return mode;
}

}  // namespace

LN_LeanMode LN_GetLeanMode()
{
  const int override_mode = g_mode_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return LN_LeanMode(override_mode);
  }
  return mode_from_environment();
}

void LN_SetLeanModeForTests(const int mode)
{
  g_mode_override.store(mode < 0 ? -1 : (mode > 2 ? 2 : mode), std::memory_order_relaxed);
}

uint64_t LN_LeanTreeRunCount()
{
  uint64_t total = 0;
  for (const RunCounterSlot &slot : g_run_counters) {
    total += slot.count.load(std::memory_order_relaxed);
  }
  return total;
}

void LN_NoteLeanTreeRun()
{
  if (g_run_slot == UINT32_MAX) {
    g_run_slot = g_next_run_slot.fetch_add(1, std::memory_order_relaxed) % kRunCounterSlots;
  }
  std::atomic<uint64_t> &count = g_run_counters[g_run_slot].count;
  count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

LN_RouteMode LN_GetRouteMode()
{
  const int override_mode = g_route_mode_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return LN_RouteMode(override_mode);
  }
  return route_mode_from_environment();
}

void LN_SetRouteModeForTests(const int mode)
{
  g_route_mode_override.store(mode < 0 ? -1 : (mode > 1 ? 1 : mode), std::memory_order_relaxed);
}

uint64_t LN_RouteRunCount()
{
  uint64_t total = 0;
  for (const RunCounterSlot &slot : g_route_counters) {
    total += slot.count.load(std::memory_order_relaxed);
  }
  return total;
}

void LN_NoteRouteRun()
{
  if (g_run_slot == UINT32_MAX) {
    g_run_slot = g_next_run_slot.fetch_add(1, std::memory_order_relaxed) % kRunCounterSlots;
  }
  std::atomic<uint64_t> &count = g_route_counters[g_run_slot].count;
  count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

bool LN_LeanHelperThread()
{
  return g_lean_helper_thread;
}

void LN_SetLeanHelperThread(const bool is_helper)
{
  g_lean_helper_thread = is_helper;
}

std::shared_ptr<const LN_LeanProgram> LN_LowerToLean(const LN_Program &program)
{
  std::shared_ptr<LN_LeanProgram> lean = std::make_shared<LN_LeanProgram>();
  for (size_t event_index = 0; event_index < lean->events.size(); event_index++) {
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event(event_index));
    if (!ir.valid) {
      /* An invalid event runs nothing, in the old interpreter too. */
      lean->valid = false;
      continue;
    }
    LN_LeanEvent &event = lean->events[event_index];
    event.ops.resize(ir.ops.size());
    /* The instruction stamps of the three events follow one another (InstructionStateIndex). */
    uint32_t state_offset = 0;
    if (LN_Event(event_index) != LN_Event::OnInit) {
      state_offset += uint32_t(program.GetInstructionHeaders(LN_Event::OnInit).size());
    }
    if (LN_Event(event_index) == LN_Event::OnFixedUpdate) {
      state_offset += uint32_t(program.GetInstructionHeaders(LN_Event::OnUpdate).size());
    }
    const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(
        LN_Event(event_index));
    for (uint32_t op_index = 0; op_index < ir.ops.size(); op_index++) {
      const LN_ExecOp &exec = ir.ops[op_index];
      LN_LeanOp &op = event.ops[op_index];
      op.exec_op_index = op_index;
      op.state_index = state_offset + exec.instruction_index;
      op.guard_is_delay_done = false;
      if (exec.guard_bool_expr_index != LN_INVALID_INDEX &&
          exec.guard_bool_expr_index < program.GetBoolExpressions().size())
      {
        const LN_BoolExpression &guard = program.GetBoolExpressions()[exec.guard_bool_expr_index];
        op.guard_is_delay_done = guard.kind == LN_BoolExpressionKind::DelayDone &&
                                 guard.int_value >= 0;
      }
      op.native = uint16_t(LN_LeanNative::Generic);
      if (exec.instruction_index < headers.size()) {
        switch (exec.kind) {
          case LN_ExecOpKind::Nop:
            op.native = uint16_t(LN_LeanNative::Nop);
            break;
          case LN_ExecOpKind::BranchRoute:
            op.native = uint16_t(LN_LeanNative::BranchRoute);
            break;
          case LN_ExecOpKind::GamePropertyCommand: {
            /* Set Game Property Bool / Int / Float: a number, no Modify, no value expression. */
            const LN_Instruction &instruction = program.GetInstructionPayload(
                LN_Event(event_index), headers[exec.instruction_index]);
            const std::vector<LN_GamePropertyCommandPayload> &payloads =
                program.GetGamePropertyCommandPayloads();
            if (instruction.command_payload_index < payloads.size()) {
              const LN_GamePropertyCommandPayload &payload =
                  payloads[instruction.command_payload_index];
              if (payload.property_ref_index != LN_INVALID_INDEX &&
                  payload.property_ref_index < program.GetGamePropertyRefs().size() &&
                  payload.modify_op != LN_PropertyModifyOp::None)
              {
                op.native = uint16_t(LN_LeanNative::ModifyProperty);
              }
              else if (payload.property_ref_index != LN_INVALID_INDEX &&
                  payload.property_ref_index < program.GetGamePropertyRefs().size() &&
                  payload.modify_op == LN_PropertyModifyOp::None &&
                  payload.value_expr_index == LN_INVALID_INDEX &&
                  (payload.value_type == LN_ValueType::Bool ||
                   payload.value_type == LN_ValueType::Int ||
                   payload.value_type == LN_ValueType::Float))
              {
                op.native = uint16_t(LN_LeanNative::SetNumber);
              }
            }
            break;
          }
          case LN_ExecOpKind::VectorCommand: {
            /* The commands OpDirectVectorCommand appends, with a payload in the program's table. */
            const LN_Instruction &instruction = program.GetInstructionPayload(
                LN_Event(event_index), headers[exec.instruction_index]);
            const std::vector<LN_VectorCommandPayload> &payloads =
                program.GetVectorCommandPayloads();
            if (instruction.command_payload_index >= payloads.size()) {
              break;
            }
            const LN_VectorCommandPayload &payload = payloads[instruction.command_payload_index];
            const LN_VectorOperationChannel channel = LN_VectorOperationChannel(
                payload.operation_channel);
            switch (instruction.opcode) {
              case LN_OpCode::SetWorldPosition:
              case LN_OpCode::SetLocalPosition:
              case LN_OpCode::SetWorldScale:
              case LN_OpCode::SetLocalScale:
              case LN_OpCode::SetLinearVelocity:
              case LN_OpCode::SetLocalLinearVelocity:
              case LN_OpCode::SetAngularVelocity:
              case LN_OpCode::SetLocalAngularVelocity:
              case LN_OpCode::SetVelocityVector:
              case LN_OpCode::ApplyPhysicsVector:
                op.native = uint16_t(LN_LeanNative::VectorCommand);
                break;
              case LN_OpCode::SetTransformVector:
                op.native = uint16_t(LN_LeanNative::VectorCommand);
                if (channel == LN_VectorOperationChannel::Orientation) {
                  op.constant_index = uint32_t(lean->rotation_constants.size());
                  lean->rotation_constants.push_back(
                      LN_Rotation::FromEulerXYZ(payload.vector_value));
                }
                break;
              case LN_OpCode::ApplyTransformVector:
                op.native = uint16_t(LN_LeanNative::VectorCommand);
                if (channel == LN_VectorOperationChannel::Rotation) {
                  op.constant_index = uint32_t(lean->rotation_constants.size());
                  lean->rotation_constants.push_back(
                      LN_Rotation::FromEulerXYZ(payload.vector_value));
                }
                break;
              case LN_OpCode::SetWorldOrientation:
              case LN_OpCode::SetLocalOrientation:
                op.native = uint16_t(LN_LeanNative::VectorCommand);
                op.constant_index = uint32_t(lean->rotation_constants.size());
                lean->rotation_constants.push_back(
                    LN_Rotation::FromEulerXYZ(payload.vector_value));
                break;
              default:
                break; /* any other opcode: the generic handler */
            }
            break;
          }
          default:
            break;
        }
      }
      /* Ops a block may hold (plan phase B1): the flow core and the writes whose commands go to
       * the typed streams a block regroups. Apply Force / Torque (Apply Physics Vector) append to
       * the physics stream: not yet. */
      switch (LN_LeanNative(op.native)) {
        case LN_LeanNative::Nop:
        case LN_LeanNative::BranchRoute:
        case LN_LeanNative::SetNumber:
        case LN_LeanNative::ModifyProperty:
          op.block_ok = !op.guard_is_delay_done;
          break;
        case LN_LeanNative::VectorCommand:
          op.block_ok = !op.guard_is_delay_done &&
                        program.GetInstructionPayload(LN_Event(event_index),
                                                      headers[exec.instruction_index])
                                .opcode != LN_OpCode::ApplyPhysicsVector;
          break;
        case LN_LeanNative::Generic:
          break;
      }
      if (op.native == uint16_t(LN_LeanNative::Generic)) {
        lean->generic_op_count++;
        event.fully_native = false;
      }
      else {
        lean->native_op_count++;
      }
    }
    /* A plain run of block-ok ops. */
    bool plain = !ir.ops.empty() && ir.ui_event_states.empty() && program.GetStepGates().empty();
    for (const LN_ExecBlock &block : ir.blocks) {
      plain = plain && block.loop_frame_index == LN_INVALID_INDEX &&
              block.completed_loop_frame_index == LN_INVALID_INDEX &&
              block.step_gate_index == LN_INVALID_INDEX;
    }
    for (uint32_t op_index = 0; op_index < ir.ops.size() && plain; op_index++) {
      plain = event.ops[op_index].block_ok && !ir.ops[op_index].ui_event_route &&
              ir.ops[op_index].ui_event_state_index == LN_INVALID_INDEX;
    }
    event.block_ok = plain;
    BuildRoute(program, LN_Event(event_index), ir, state_offset, event);
  }
  BuildLeanMemo(program, *lean);
  const std::vector<LN_QueryExpression> &queries = program.GetQueryExpressions();
  lean->query_fixed_distance.resize(queries.size());
  for (size_t index = 0; index < queries.size(); index++) {
    lean->query_fixed_distance[index] = LN_DistanceQueryEndsAreFixed(program, queries[index]) ? 1 :
                                                                                             0;
  }
  lean->expression_entries = {uint32_t(program.GetBoolExpressions().size()),
                              uint32_t(program.GetIntExpressions().size()),
                              uint32_t(program.GetFloatExpressions().size()),
                              uint32_t(program.GetStringExpressions().size()),
                              uint32_t(program.GetVectorExpressions().size()),
                              uint32_t(program.GetColorExpressions().size()),
                              uint32_t(program.GetValueExpressions().size()),
                              uint32_t(program.GetQueryExpressions().size())};
  return lean;
}

static const char *native_name(const uint16_t native)
{
  switch (LN_LeanNative(native)) {
    case LN_LeanNative::Generic:
      return "0";
    case LN_LeanNative::Nop:
      return "Nop";
    case LN_LeanNative::BranchRoute:
      return "BranchRoute";
    case LN_LeanNative::SetNumber:
      return "SetNumber";
    case LN_LeanNative::VectorCommand:
      return "VectorCommand";
    case LN_LeanNative::ModifyProperty:
      return "ModifyProperty";
  }
  return "?";
}

std::string LN_DumpLeanProgramText(const LN_LeanProgram &lean, const LN_Program &program)
{
  static const char *const event_names[] = {"OnInit", "OnUpdate", "OnFixedUpdate"};
  static const char *const table_names[] = {
      "bool", "int", "float", "string", "vector", "color", "value", "query"};
  std::ostringstream text;
  text << "lean program valid=" << (lean.valid ? 1 : 0) << " generic_ops=" << lean.generic_op_count
       << " native_ops=" << lean.native_op_count
       << " native_expressions=" << lean.native_expression_count << " memo=" << lean.memo_count
       << "\n";
  for (size_t table = 0; table < lean.expression_entries.size(); table++) {
    text << "expressions " << table_names[table] << " " << lean.expression_entries[table] << "\n";
  }
  for (size_t event_index = 0; event_index < lean.events.size(); event_index++) {
    const LN_LeanEvent &event = lean.events[event_index];
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event(event_index));
    text << "event " << event_names[event_index] << " ops=" << event.ops.size()
         << " blocks=" << ir.blocks.size() << " fully_native=" << (event.fully_native ? 1 : 0)
         << " route=" << (event.has_route ? 1 : 0)
         << " block_ok=" << (event.block_ok ? 1 : 0) << "\n";
    for (size_t op_index = 0; op_index < event.ops.size(); op_index++) {
      const LN_LeanOp &op = event.ops[op_index];
      const LN_ExecOp &exec = ir.ops[op.exec_op_index];
      const LN_KindInfo *info = LN_FindKind(LN_KindDomain::Op, uint16_t(exec.opcode));
      text << "  [" << op_index << "] " << (info != nullptr ? info->name : "Op::?")
           << " exec=" << op.exec_op_index << " native=" << native_name(op.native) << "\n";
    }
  }
  return text.str();
}
