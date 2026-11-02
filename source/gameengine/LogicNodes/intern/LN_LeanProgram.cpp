/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "LN_LeanProgram.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "LN_KindCatalog.h"
#include "LN_Program.h"

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
       << " native_expressions=" << lean.native_expression_count << "\n";
  for (size_t table = 0; table < lean.expression_entries.size(); table++) {
    text << "expressions " << table_names[table] << " " << lean.expression_entries[table] << "\n";
  }
  for (size_t event_index = 0; event_index < lean.events.size(); event_index++) {
    const LN_LeanEvent &event = lean.events[event_index];
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(LN_Event(event_index));
    text << "event " << event_names[event_index] << " ops=" << event.ops.size()
         << " blocks=" << ir.blocks.size() << " fully_native=" << (event.fully_native ? 1 : 0)
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
