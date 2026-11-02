/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "LN_StrictDump.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "DNA_collection_types.h"

#include "KX_GameObject.h"
#include "KX_Parenting.h"
#include "LN_CommandBuffer.h"
#include "LN_CommandDiagnostics.h"
#include "LN_RuntimeTree.h"

/* ---- names ---- */

std::string LN_DumpNames::Object(KX_GameObject *object)
{
  if (object == nullptr) {
    return "-";
  }
  auto item = m_ordinals.find(object);
  if (item == m_ordinals.end()) {
    item = m_ordinals.emplace(object, uint32_t(m_ordinals.size())).first;
  }
  return object->GetName() + "#" + std::to_string(item->second);
}

std::string LN_DumpNames::Tree(const LN_RuntimeTree *tree)
{
  if (tree == nullptr) {
    return "-";
  }
  auto item = m_ordinals.find(tree);
  if (item == m_ordinals.end()) {
    item = m_ordinals.emplace(tree, uint32_t(m_ordinals.size())).first;
  }
  return "tree#" + std::to_string(item->second);
}

std::string LN_DumpNames::Collection(const void *collection)
{
  if (collection == nullptr) {
    return "-";
  }
  const blender::Collection *typed = static_cast<const blender::Collection *>(collection);
  return std::string("collection:") + (typed->id.name + 2);
}

/* ---- numbers and text ---- */

std::string LN_DumpFloatBits(const float value)
{
  uint32_t bits;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", bits);
  return text;
}

std::string LN_DumpDoubleBits(const double value)
{
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(bits));
  return text;
}

std::string LN_DumpQuoted(const std::string &text)
{
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    }
    else if (c == '\n') {
      out += "\\n";
    }
    else if (static_cast<unsigned char>(c) < 0x20) {
      char escape[8];
      std::snprintf(escape, sizeof(escape), "\\x%02x", static_cast<unsigned char>(c));
      out += escape;
    }
    else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string LN_DumpVector3(const MT_Vector3 &value)
{
  return "(" + LN_DumpFloatBits(value.x()) + " " + LN_DumpFloatBits(value.y()) + " " +
         LN_DumpFloatBits(value.z()) + ")";
}

std::string LN_DumpVector4(const MT_Vector4 &value)
{
  return "(" + LN_DumpFloatBits(value.x()) + " " + LN_DumpFloatBits(value.y()) + " " +
         LN_DumpFloatBits(value.z()) + " " + LN_DumpFloatBits(value.w()) + ")";
}

std::string LN_DumpMatrix3(const MT_Matrix3x3 &value)
{
  std::string out = "(";
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      out += LN_DumpFloatBits(value[row][column]);
      if (row != 2 || column != 2) {
        out += ' ';
      }
    }
  }
  return out + ")";
}

std::string LN_DumpRotation(const LN_Rotation &value)
{
  const MT_Quaternion &q = value.Quaternion();
  return "(" + LN_DumpFloatBits(q.x()) + " " + LN_DumpFloatBits(q.y()) + " " +
         LN_DumpFloatBits(q.z()) + " " + LN_DumpFloatBits(q.w()) + ")";
}

std::string LN_DumpRuntimeRef(const LN_RuntimeRef &value)
{
  return "ref{" + std::to_string(int(value.kind)) + "," + std::to_string(value.slot) + "," +
         std::to_string(value.generation) + "," + std::to_string(value.shared_domain) + "," +
         LN_DumpQuoted(value.debug_name) + "}";
}

std::string LN_DumpValue(const LN_Value &value,
                         LN_DumpNames &names,
                         const LN_RuntimeTree *resolver)
{
  std::string out = "V{t=" + std::to_string(int(value.type)) + ",x=" + (value.exists ? "1" : "0") +
                    ",b=" + (value.bool_value ? "1" : "0") +
                    ",eh=" + (value.rotation_euler_hint_valid ? "1" : "0") +
                    ",i=" + std::to_string(value.int_value) +
                    ",f=" + LN_DumpFloatBits(value.float_value) +
                    ",v=" + LN_DumpVector3(value.vector_value) +
                    ",v4=" + LN_DumpVector4(value.vector4_value) +
                    ",m=" + LN_DumpMatrix3(value.matrix_value) +
                    ",c=" + LN_DumpVector4(value.color_value) +
                    ",r=" + LN_DumpRotation(value.rotation_value) +
                    "," + LN_DumpRuntimeRef(value.runtime_ref);
  if (resolver != nullptr && value.runtime_ref.kind == LN_RuntimeRefKind::Object &&
      value.runtime_ref.shared_domain == 0 && value.runtime_ref.IsValid())
  {
    out += ",obj=" + names.Object(resolver->ResolveObjectRef(value.runtime_ref));
  }
  out += ",rn=" + LN_DumpQuoted(value.reference_name) + ",s=" + LN_DumpQuoted(value.string_value);
  out += ",l=[";
  for (size_t index = 0; index < value.list_value.size(); index++) {
    out += (index != 0 ? "," : "") + LN_DumpValue(value.list_value[index], names, resolver);
  }
  out += "],d={";
  bool first = true;
  for (const auto &[key, item] : value.dict_value) { /* std::map: key order */
    out += (first ? "" : ",") + LN_DumpQuoted(key) + ":" + LN_DumpValue(item, names, resolver);
    first = false;
  }
  return out + "}}";
}

/* ---- commands ---- */

std::string LN_CommandBuffer::DumpCommand(const Command &command, LN_DumpNames &names)
{
  std::ostringstream out;
  out << "type=" << LN_CommandTypeName(command.type)
      << " subsystem=" << int(command.subsystem) << " seq=" << command.record_sequence
      << " key=" << command.sort_key << " src=" << command.source_ref_index
      << " obj=" << names.Object(command.object) << " tree=" << names.Tree(command.runtime_tree)
      << " target=" << names.Object(command.event_target_object)
      << " collection=" << names.Collection(command.collection) << " "
      << LN_DumpRuntimeRef(command.runtime_ref) << " h=" << command.object_handle.index << "/"
      << command.object_handle.generation << " target_h=" << command.event_target_handle.index
      << "/" << command.event_target_handle.generation
      << " secondary_h=" << command.secondary_object_handle.index << "/"
      << command.secondary_object_handle.generation << " gp=" << command.game_property_id.index
      << " es=" << command.event_subject_id.index << " name_ptr="
      << (command.property_name_ptr != nullptr ? LN_DumpQuoted(*command.property_name_ptr) : "-")
      << " vec=" << LN_DumpVector3(command.vector_value)
      << " vec2=" << LN_DumpVector3(command.secondary_vector_value)
      << " rot=" << LN_DumpRotation(command.rotation_value)
      << " col=" << LN_DumpVector4(command.color_value)
      << " value=" << LN_DumpValue(command.property_value, names, command.runtime_tree)
      << " modify=" << int(command.modify.op) << "," << command.modify.clamped << ","
      << LN_DumpFloatBits(command.modify.operand) << ","
      << LN_DumpFloatBits(command.modify.min_value) << ","
      << LN_DumpFloatBits(command.modify.max_value) << " name_id=" << command.property_name_id.index
      << " name=" << LN_DumpQuoted(command.property_name)
      << " name2=" << LN_DumpQuoted(command.secondary_property_name)
      << " name3=" << LN_DumpQuoted(command.tertiary_property_name)
      << " name4=" << LN_DumpQuoted(command.quaternary_property_name)
      << " scalar=" << LN_DumpFloatBits(command.scalar_value) << " int=" << command.int_value
      << " int2=" << command.secondary_int_value << " pref=" << command.property_ref_index
      << " bool=" << command.bool_value << " bool2=" << command.secondary_bool_value
      << " parent=";
  if (command.parent_options != nullptr) {
    out << int(command.parent_options->type) << "," << command.parent_options->compound << ","
        << LN_DumpQuoted(command.parent_options->bone_name);
  }
  else {
    out << "-";
  }
  out << " anim=" << command.animation_flags << " action_out=" << int(command.action_output_mask)
      << " sound_group=" << int(command.sound_group);
  return out.str();
}

std::string LN_CommandBuffer::DumpRecordedForTests(const DumpOrder order)
{
  const std::vector<Command> &recorded = GetCommandsForTests();
  /* Name objects and trees in record order whichever order is dumped, so the names are the same
   * in all three dumps of one recording. */
  LN_DumpNames names;
  std::vector<std::string> lines;
  lines.reserve(recorded.size());
  for (const Command &command : recorded) {
    lines.push_back(DumpCommand(command, names));
  }
  std::string out;
  if (order == DumpOrder::Record) {
    for (size_t index = 0; index < lines.size(); index++) {
      out += "#" + std::to_string(index) + " " + lines[index] + "\n";
    }
    return out;
  }
  /* Flush order: as (sort key, record sequence); a command is found by its record sequence,
   * which is unique within a buffer. */
  std::unordered_map<uint64_t, size_t> by_sequence;
  for (size_t index = 0; index < recorded.size(); index++) {
    by_sequence.emplace(recorded[index].record_sequence, index);
  }
  size_t position = 0;
  for (const auto &[sort_key, sequence] :
       DirectFlushOrderForTests(order == DumpOrder::FlushCoalesced))
  {
    const auto found = by_sequence.find(sequence);
    out += "#" + std::to_string(position++) + " ";
    if (found == by_sequence.end()) {
      out += "(not materialized: key=" + std::to_string(sort_key) +
             " seq=" + std::to_string(sequence) + ")\n";
    }
    else {
      out += lines[found->second] + "\n";
    }
  }
  return out;
}

/* ---- a runtime tree's state ---- */

namespace {

std::string tick_text(const uint64_t tick)
{
  return tick == UINT64_MAX ? std::string("-") : std::to_string(tick);
}

/** "ran this tick" ('T'), "ran before" ('t') or "never" ('-'). */
char ran_text(const uint64_t last_tick, const uint64_t current_tick)
{
  return last_tick == UINT64_MAX ? '-' : (last_tick == current_tick ? 'T' : 't');
}

}  // namespace

LN_RuntimeTree::StateDump LN_RuntimeTree::DumpStateForTests(const uint64_t current_tick) const
{
  LN_DumpNames names;
  const auto value = [&](const LN_Value &v) { return LN_DumpValue(v, names, this); };
  const auto values = [&](const std::vector<LN_Value> &list) {
    std::string out = "[";
    for (size_t index = 0; index < list.size(); index++) {
      out += (index != 0 ? "," : "") + value(list[index]);
    }
    return out + "]";
  };
  const auto sorted_keys = [](const auto &map) {
    std::vector<uint32_t> keys;
    for (const auto &item : map) {
      keys.push_back(item.first);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  };

  /* Fields that are the same kind of thing in an inline state and an overflow one are written by
   * one function each. */
  const auto bool_meaning = [&](const BoolExpressionRuntimeState &s) {
    return std::string("init=") + (s.initialized ? "1" : "0") + " prev_bool=" +
           (s.previous_bool ? "1" : "0") + " prev_float=" + LN_DumpFloatBits(s.previous_float) +
           " active=" + (s.active ? "1" : "0") + " cached=" + (s.cached_result ? "1" : "0") +
           " start=" + LN_DumpDoubleBits(s.start_time) + " due=" + LN_DumpDoubleBits(s.due_time) +
           " last_pulse=" + LN_DumpDoubleBits(s.last_pulse_time) +
           " persistence=" + std::to_string(s.pending_persistence_results) + "/" +
           std::to_string(int(s.pending_persistence_count)) + "/" +
           (s.last_persistence_success ? "1" : "0");
  };
  const auto bool_bookkeeping = [&](const BoolExpressionRuntimeState &s) {
    return "update_tick=" + tick_text(s.last_update_tick) +
           " update_phase_tick=" + tick_text(s.last_update_phase_tick);
  };
  const auto float_meaning = [&](const FloatExpressionRuntimeState &s) {
    return std::string("init=") + (s.initialized ? "1" : "0") +
           " stored=" + LN_DumpFloatBits(s.stored_value);
  };
  const auto motion_meaning = [&](const InstructionMotionState &s) {
    std::string out = "mouse_look{smooth=" + LN_DumpFloatBits(s.mouse_look_smooth_x) + "," +
                      LN_DumpFloatBits(s.mouse_look_smooth_y) +
                      " angle=" + LN_DumpFloatBits(s.mouse_look_angle_x) + "," +
                      LN_DumpFloatBits(s.mouse_look_angle_y) + " prev=" +
                      std::to_string(s.mouse_look_previous_x) + "," +
                      std::to_string(s.mouse_look_previous_y) + " init=" +
                      (s.mouse_look_initialized ? "1" : "0") + " has_prev=" +
                      (s.mouse_look_has_previous_position ? "1" : "0") +
                      " ui_serial=" + std::to_string(s.mouse_look_ui_serial) + " centered=" +
                      (s.mouse_look_centered ? "1" : "0") + " settle=" +
                      LN_DumpFloatBits(s.mouse_look_settle) + "/" +
                      std::to_string(s.mouse_look_settle_runs) + "} path{len=" +
                      std::to_string(s.path_len) + " waypoint=" + std::to_string(s.waypoint_index) +
                      " list_point=" + std::to_string(s.list_point_index) + " next=" +
                      LN_DumpVector3(s.next_path_point) +
                      " update_time=" + LN_DumpDoubleBits(s.path_update_time) + " points=";
    const int count = std::clamp(s.path_len, 0, kMaxPathLength);
    for (int index = 0; index < count * 3; index++) {
      out += (index != 0 ? " " : "") + LN_DumpFloatBits(s.path[index]);
    }
    return out + "} reached_scope=" + std::to_string(s.last_reached_scope_serial);
  };
  const auto change_meaning = [&](const ValueChangeState &s) {
    return std::string("has_prev=") + (s.has_previous_value ? "1" : "0") +
           " prev=" + value(s.previous_value) + " old=" + value(s.stored_old_value) +
           " new=" + value(s.stored_new_value);
  };
  const auto query_meaning = [&](const QueryExpressionRuntimeState &s) {
    std::string out = "ui_cursor=" + std::to_string(s.ui_event_cursor) + " hover=";
    for (const auto &hover : s.hover_by_event) {
      out += std::string("{init=") + (hover.initialized ? "1" : "0") +
             " prev=" + (hover.previous ? "1" : "0") +
             " base_init=" + (hover.baseline_initialized ? "1" : "0") +
             " base_prev=" + (hover.baseline_previous ? "1" : "0") + "}";
    }
    return out;
  };
  const auto query_bookkeeping = [&](const QueryExpressionRuntimeState &s) {
    return "cached_tick=" + tick_text(s.cached_tick) +
           " cached_exec_key=" + tick_text(s.cached_command_execution_key) +
           " ordered=" + (s.cached_command_ordered ? "1" : "0") +
           " hover_ticks=" + tick_text(s.hover_by_event[0].baseline_tick) + "," +
           tick_text(s.hover_by_event[1].baseline_tick) + "," +
           tick_text(s.hover_by_event[2].baseline_tick);
  };

  std::ostringstream meaning;
  std::ostringstream book;
  meaning << "enabled=" << m_enabled << " runtime_active=" << m_runtimeActive
          << " init_executed=" << m_initExecuted << " ui_routes_only=" << m_uiEventRoutesOnly
          << " step_spread=" << m_hasStepSpread << "/" << m_stepSpread
          << " step_gates_any_due=" << m_stepGatesAnyDue << " step_gates_forced=" << m_stepGatesForced
          << "\n";
  for (uint32_t index = 0; index < m_stepGates.size(); index++) {
    const StepGateState &gate = m_stepGates[index];
    meaning << "gate[" << index << "] due=" << gate.due
            << " time_passed=" << LN_DumpDoubleBits(gate.time_passed)
            << " unscaled=" << LN_DumpDoubleBits(gate.unscaled_time_passed)
            << " last_run_step=" << tick_text(gate.last_run_step) << "\n";
  }
  for (uint32_t index = 0; index < m_instructionStamps.size(); index++) {
    meaning << "ran[" << index << "]="
            << ran_text(m_instructionStamps[index].last_execution_tick, current_tick) << "\n";
    book << "stamp[" << index << "] tick=" << tick_text(m_instructionStamps[index].last_execution_tick)
         << " scope=" << m_instructionStamps[index].last_execution_scope_serial << "\n";
  }
  for (uint32_t index = 0; index < m_boolStates.size(); index++) {
    meaning << "bool[" << index << "] " << bool_meaning(m_boolStates[index]) << "\n";
    book << "bool[" << index << "] " << bool_bookkeeping(m_boolStates[index]) << "\n";
  }
  for (uint32_t index = 0; index < m_floatStates.size(); index++) {
    meaning << "float[" << index << "] " << float_meaning(m_floatStates[index]) << "\n";
    book << "float[" << index << "] store_tick=" << tick_text(m_floatStates[index].last_store_tick)
         << "\n";
  }
  for (uint32_t index = 0; index < m_timeFlowStates.size(); index++) {
    const TimeFlowRuntimeState &s = m_timeFlowStates[index];
    meaning << "time[" << index << "] kind=" << int(s.kind) << " active=" << s.active
            << " remaining=" << LN_DumpDoubleBits(s.remaining_time)
            << " accumulated=" << LN_DumpDoubleBits(s.accumulated_time)
            << " pulse_now=" << ran_text(s.pulse_tick, current_tick)
            << " stride=" << s.last_update_phase_stride << " ignore_timescale=" << s.ignore_timescale
            << " ui_route=" << s.ui_event_route << " event=" << int(s.event)
            << " has_props=" << s.has_active_tree_properties
            << " active_props=" << values(s.active_tree_properties) << " pending=[";
    for (const TimeFlowRuntimeState::PendingPulse &pulse : s.pending_pulses) {
      meaning << "{remaining=" << LN_DumpDoubleBits(pulse.remaining_time)
              << " props=" << values(pulse.tree_properties) << "}";
    }
    meaning << "]\n";
    book << "time[" << index << "] pulse_tick=" << tick_text(s.pulse_tick)
         << " update_tick=" << tick_text(s.last_update_tick)
         << " update_phase_tick=" << tick_text(s.last_update_phase_tick)
         << " accepted_scope=" << s.accepted_scope_serial << " blocked_scope=" << s.blocked_scope_serial
         << "\n";
  }
  for (uint32_t index = 0; index < m_flowControlStates.size(); index++) {
    const FlowControlRuntimeState &s = m_flowControlStates[index];
    meaning << "flow[" << index << "] next_route=" << s.next_route_index
            << " last_route=" << s.last_route_index << " accepted=" << s.accepted_count
            << " motion{init=" << s.motion_initialized << " valid=" << s.motion_valid
            << " source_mode=" << s.motion_source_mode
            << " sample_time=" << LN_DumpDoubleBits(s.motion_last_sample_time) << " "
            << LN_DumpRuntimeRef(s.motion_source_ref) << " pos=" << LN_DumpVector3(s.motion_position)
            << " prev=" << LN_DumpVector3(s.motion_previous_position)
            << " vel=" << LN_DumpVector3(s.motion_velocity)
            << " dir=" << LN_DumpVector3(s.motion_direction)
            << " predicted=" << LN_DumpVector3(s.motion_predicted_position)
            << " speed=" << LN_DumpFloatBits(s.motion_speed) << "}\n";
    book << "flow[" << index << "] routed_scope=" << s.routed_scope_serial
         << " accepted_scope=" << s.accepted_scope_serial << " blocked_scope=" << s.blocked_scope_serial
         << " sample_tick=" << tick_text(s.motion_last_sample_tick) << "\n";
  }
  for (uint32_t index = 0; index < m_motionStates.size(); index++) {
    meaning << "motion[" << index << "] " << motion_meaning(m_motionStates[index])
            << " reached_now=" << ran_text(m_motionStates[index].last_reached_tick, current_tick)
            << "\n";
    book << "motion[" << index << "] reached_tick=" << tick_text(m_motionStates[index].last_reached_tick)
         << " path_update_time=" << LN_DumpDoubleBits(m_motionStates[index].path_update_time) << "\n";
  }
  for (uint32_t index = 0; index < m_valueChangeStates.size(); index++) {
    meaning << "change[" << index << "] " << change_meaning(m_valueChangeStates[index]) << "\n";
  }
  for (uint32_t index = 0; index < m_queryExpressionStates.size(); index++) {
    meaning << "query[" << index << "] " << query_meaning(m_queryExpressionStates[index]) << "\n";
    book << "query[" << index << "] " << query_bookkeeping(m_queryExpressionStates[index]) << "\n";
  }
  for (size_t index = 0; index < m_loopStates.size(); index++) {
    const LoopRuntimeState &s = m_loopStates[index];
    meaning << "loop[" << index << "] active=" << s.active << " triggered=" << s.triggered
            << " break=" << s.break_requested << " trigger_previous=" << s.trigger_previous
            << " iteration=" << s.iteration_index << "/" << s.iteration_count
            << " batch=" << s.batch_index << "/" << s.batch_count << " batch_end=" << s.batch_end_iteration
            << " items=" << values(s.items) << " current=" << value(s.current_value) << "\n";
    book << "loop[" << index << "] last_batch_tick=" << tick_text(s.last_batch_tick_index)
         << " completed_scope=" << s.completed_scope_serial << "\n";
  }
  for (size_t index = 0; index < m_treeProperties.size(); index++) {
    meaning << "tree_property[" << index << "] " << value(m_treeProperties[index]) << "\n";
  }
  if (m_program != nullptr) {
    const size_t refs = m_program->GetGamePropertyRefs().size();
    for (size_t index = 0; index < refs; index++) {
      LN_Value kept;
      const bool has_kept = m_snapshot.BuildGamePropertyValue(uint32_t(index), kept);
      meaning << "game_property[" << index << "] " << (has_kept ? value(kept) : "-") << "\n";
    }
  }
  for (const uint32_t key : sorted_keys(m_uiCommandCompletions)) {
    const UICommandCompletionState &s = m_uiCommandCompletions.at(key);
    meaning << "ui_completion[" << key << "] current=" << int(s.current.result) << "/"
            << s.current.handle << " has_current=" << s.has_current << " has_value=" << s.has_value
            << " pending=";
    for (const UICommandCompletion &pending : s.pending) {
      meaning << int(pending.result) << "/" << pending.handle << ",";
    }
    meaning << " now=" << ran_text(s.current_tick, current_tick) << "\n";
    book << "ui_completion[" << key << "] tick=" << tick_text(s.current_tick) << "\n";
  }
  for (const uint32_t key : sorted_keys(m_actionCompletions)) {
    const ActionCompletionState &s = m_actionCompletions.at(key);
    meaning << "action_completion[" << key << "] current=" << int(s.current)
            << " has_current=" << s.has_current << " pending=";
    for (const LN_ActionResult pending : s.pending) {
      meaning << int(pending) << ",";
    }
    meaning << " now=" << ran_text(s.current_tick, current_tick) << "\n";
    book << "action_completion[" << key << "] tick=" << tick_text(s.current_tick) << "\n";
  }
  if (m_stateOverflow != nullptr) {
    const StateOverflow &overflow = *m_stateOverflow;
    for (const uint32_t key : sorted_keys(overflow.bool_states)) {
      meaning << "overflow_bool[" << key << "] " << bool_meaning(overflow.bool_states.at(key)) << "\n";
      book << "overflow_bool[" << key << "] " << bool_bookkeeping(overflow.bool_states.at(key)) << "\n";
    }
    for (const uint32_t key : sorted_keys(overflow.float_states)) {
      meaning << "overflow_float[" << key << "] " << float_meaning(overflow.float_states.at(key)) << "\n";
    }
    for (const uint32_t key : sorted_keys(overflow.motion_states)) {
      meaning << "overflow_motion[" << key << "] " << motion_meaning(overflow.motion_states.at(key)) << "\n";
    }
    for (const uint32_t key : sorted_keys(overflow.value_changes)) {
      meaning << "overflow_change[" << key << "] " << change_meaning(overflow.value_changes.at(key)) << "\n";
    }
    for (const uint32_t key : sorted_keys(overflow.queries)) {
      meaning << "overflow_query[" << key << "] " << query_meaning(overflow.queries.at(key)) << "\n";
      book << "overflow_query[" << key << "] " << query_bookkeeping(overflow.queries.at(key)) << "\n";
    }
  }

  book << "snapshot_tick=" << tick_text(m_snapshotTick)
       << " query_warm_tick=" << tick_text(m_queryCacheWarmTick)
       << " scope_serial=" << m_executionScopeSerial << " active_scope_serial=" << m_activeExecutionScopeSerial
       << " command_serial=" << m_commandExecutionSerial << " program_revision=" << m_programRevision
       << " step_gates_step=" << tick_text(m_stepGatesStep)
       << " step_gates_first_step=" << tick_text(m_stepGatesFirstStep)
       << " event_delta=" << LN_DumpDoubleBits(m_eventDeltaTime)
       << " latent_tick=" << tick_text(m_latentTimeFlowTick)
       << "\n"; /* (The per-tick memo's generation is not listed: the lean VM keeps its own.) */
  return {meaning.str(), book.str()};
}
