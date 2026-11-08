/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Text dump of a compiled LN_Program (lean runtime plan, phase A2). Every struct is written field
 * by field, a field only when it differs from a default-constructed one; the static_asserts
 * below fail when a struct changes size, which means a field was added: add it to the dump.
 *
 * The sizes are the ones of GCC's standard library on 64-bit Linux, where the fork is built and
 * its dump goldens are checked. A struct holding library types has other sizes elsewhere (MSVC's
 * std::map is 16 bytes, not 48, so LN_Value and LN_ValueExpression are smaller there; MSVC Debug
 * builds and macOS's libc++ size strings and vectors differently too), so the reminder is checked
 * only there: one platform is enough to catch a new field. Until 2026-11-08 it was checked
 * everywhere and failed every Windows build since it was added (2026-11-02). */

#include <sstream>
#include <string>

#include "LN_ExecBlockIR.h"
#include "LN_KindCatalog.h"
#include "LN_Program.h"
#include "LN_StrictDump.h"
#include "LN_Types.h"

#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(LN_Instruction) == 188, "LN_Instruction changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_BoolExpression) == 96, "LN_BoolExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_IntExpression) == 32, "LN_IntExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_FloatExpression) == 72, "LN_FloatExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_StringExpression) == 80, "LN_StringExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_VectorExpression) == 80, "LN_VectorExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_ColorExpression) == 40, "LN_ColorExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_ValueExpression) == 368, "LN_ValueExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_QueryExpression) == 224, "LN_QueryExpression changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_VectorCommandPayload) == 44, "LN_VectorCommandPayload changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_GamePropertyCommandPayload) == 48,
              "LN_GamePropertyCommandPayload changed: update LN_ProgramDump.cpp");
static_assert(sizeof(LN_StepGate) == 8, "LN_StepGate changed: update LN_ProgramDump.cpp");
#endif

namespace {

/** Writes " name=value" for a field that is not its default. */
class Fields {
 public:
  explicit Fields(std::ostringstream &out) : m_out(out) {}

  void index(const char *name, const uint32_t value, const uint32_t fallback = LN_INVALID_INDEX)
  {
    if (value != fallback) {
      m_out << ' ' << name << '=' << value;
    }
  }
  void number(const char *name, const int64_t value, const int64_t fallback = 0)
  {
    if (value != fallback) {
      m_out << ' ' << name << '=' << value;
    }
  }
  void flag(const char *name, const bool value, const bool fallback = false)
  {
    if (value != fallback) {
      m_out << ' ' << name << '=' << (value ? 1 : 0);
    }
  }
  void real(const char *name, const float value, const float fallback = 0.0f)
  {
    if (LN_DumpFloatBits(value) != LN_DumpFloatBits(fallback)) {
      m_out << ' ' << name << '=' << LN_DumpFloatBits(value);
    }
  }
  void vector(const char *name, const MT_Vector3 &value, const MT_Vector3 &fallback)
  {
    if (LN_DumpVector3(value) != LN_DumpVector3(fallback)) {
      m_out << ' ' << name << '=' << LN_DumpVector3(value);
    }
  }
  void vector4(const char *name, const MT_Vector4 &value, const MT_Vector4 &fallback)
  {
    if (LN_DumpVector4(value) != LN_DumpVector4(fallback)) {
      m_out << ' ' << name << '=' << LN_DumpVector4(value);
    }
  }
  void rotation(const char *name, const LN_Rotation &value, const LN_Rotation &fallback)
  {
    if (LN_DumpRotation(value) != LN_DumpRotation(fallback)) {
      m_out << ' ' << name << '=' << LN_DumpRotation(value);
    }
  }
  void text(const char *name, const std::string &value)
  {
    if (!value.empty()) {
      m_out << ' ' << name << '=' << LN_DumpQuoted(value);
    }
  }
  void indices(const char *name, const std::vector<uint32_t> &values)
  {
    if (!values.empty()) {
      m_out << ' ' << name << "=[";
      for (size_t i = 0; i < values.size(); i++) {
        m_out << (i != 0 ? "," : "") << values[i];
      }
      m_out << ']';
    }
  }

 private:
  std::ostringstream &m_out;
};

#define IDX(field) f.index(#field, e.field, d.field)
#define NUM(field) f.number(#field, int64_t(e.field), int64_t(d.field))
#define FLAG(field) f.flag(#field, e.field, d.field)
#define REAL(field) f.real(#field, e.field, d.field)

std::string instruction_text(const LN_Instruction &e, LN_DumpNames &names)
{
  const LN_Instruction d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Op, uint16_t(e.opcode));
  Fields f(out);
  NUM(source_ref_index);
  NUM(payload_kind);
  IDX(command_payload_index);
  NUM(flags);
  NUM(vector_operation_mode);
  NUM(vector_operation_channel);
  NUM(vector_operation_mask);
  f.vector("vector_value", e.vector_value, d.vector_value);
  f.vector("secondary_vector_value", e.secondary_vector_value, d.secondary_vector_value);
  f.vector4("color_value", e.color_value, d.color_value);
  IDX(vector_expr_index);
  IDX(secondary_vector_expr_index);
  IDX(bool_guard_expr_index);
  IDX(bool_expr_index);
  IDX(secondary_bool_expr_index);
  IDX(int_expr_index);
  IDX(secondary_int_expr_index);
  IDX(tertiary_int_expr_index);
  IDX(float_expr_index);
  IDX(secondary_float_expr_index);
  IDX(tertiary_float_expr_index);
  IDX(quaternary_float_expr_index);
  IDX(string_expr_index);
  IDX(secondary_string_expr_index);
  IDX(tertiary_string_expr_index);
  IDX(property_ref_index);
  IDX(color_expr_index);
  IDX(value_expr_index);
  IDX(secondary_value_expr_index);
  IDX(tertiary_value_expr_index);
  IDX(quaternary_value_expr_index);
  IDX(tertiary_bool_expr_index);
  IDX(quaternary_bool_expr_index);
  IDX(loop_frame_index);
  IDX(completed_loop_frame_index);
  IDX(ui_event_state_index);
  IDX(step_gate_index);
  NUM(property_value_type);
  NUM(int_value);
  NUM(secondary_int_value);
  FLAG(bool_value);
  (void)names;
  return out.str();
}

std::string bool_text(const LN_BoolExpression &e)
{
  const LN_BoolExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Bool, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(float_expr_index);
  IDX(secondary_float_expr_index);
  IDX(int_expr_index);
  IDX(property_ref_index);
  NUM(float_compare_operation);
  NUM(rigid_body_constraint_match_mode);
  REAL(float_value);
  REAL(secondary_float_value);
  NUM(int_value);
  NUM(secondary_int_value);
  f.text("string_value", e.string_value);
  FLAG(bool_value);
  return out.str();
}

std::string float_text(const LN_FloatExpression &e)
{
  const LN_FloatExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Float, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  f.indices("input_indices", e.input_indices);
  IDX(bool_expr_index);
  IDX(string_expr_index);
  IDX(property_ref_index);
  NUM(rigid_body_constraint_match_mode);
  NUM(int_value);
  NUM(threshold_operation);
  NUM(range_operation);
  REAL(float_value);
  FLAG(bool_value);
  NUM(component_index);
  return out.str();
}

std::string int_text(const LN_IntExpression &e)
{
  const LN_IntExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Int, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(property_ref_index);
  NUM(rigid_body_constraint_match_mode);
  NUM(int_value);
  NUM(float_to_int_mode);
  return out.str();
}

std::string string_text(const LN_StringExpression &e)
{
  const LN_StringExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::String, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(input3);
  IDX(input4);
  IDX(int_expr_index);
  IDX(property_ref_index);
  IDX(value_expr_index);
  NUM(rigid_body_constraint_match_mode);
  f.index("string_id", e.string_id.index, d.string_id.index);
  f.text("string_value", e.string_value);
  return out.str();
}

std::string vector_text(const LN_VectorExpression &e)
{
  const LN_VectorExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Vector, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(bool_expr_index);
  IDX(float_expr_index);
  IDX(property_ref_index);
  NUM(rigid_body_constraint_match_mode);
  f.vector("vector_value", e.vector_value, d.vector_value);
  f.rotation("rotation_value", e.rotation_value, d.rotation_value);
  NUM(output_type);
  REAL(float_value);
  FLAG(bool_value);
  return out.str();
}

std::string color_text(const LN_ColorExpression &e)
{
  const LN_ColorExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Color, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(input3);
  IDX(property_ref_index);
  f.vector4("color_value", e.color_value, d.color_value);
  return out.str();
}

std::string value_text(const LN_ValueExpression &e, LN_DumpNames &names)
{
  const LN_ValueExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Value, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  f.indices("input_indices", e.input_indices);
  IDX(bool_expr_index);
  IDX(string_expr_index);
  IDX(property_ref_index);
  NUM(rigid_body_constraint_match_mode);
  if (LN_DumpValue(e.value, names) != LN_DumpValue(d.value, names)) {
    out << " value=" << LN_DumpValue(e.value, names);
  }
  return out.str();
}

std::string query_text(const LN_QueryExpression &e)
{
  const LN_QueryExpression d;
  std::ostringstream out;
  out << LN_KindName(LN_KindDomain::Query, uint16_t(e.kind));
  Fields f(out);
  IDX(input0);
  IDX(input1);
  IDX(input2);
  IDX(input3);
  IDX(input4);
  IDX(input5);
  IDX(input6);
  IDX(input7);
  IDX(input8);
  IDX(input9);
  IDX(ignore_list_expr_index);
  IDX(bool_expr_index);
  IDX(secondary_bool_expr_index);
  IDX(tertiary_bool_expr_index);
  IDX(quaternary_bool_expr_index);
  IDX(quinary_bool_expr_index);
  IDX(senary_bool_expr_index);
  IDX(condition_bool_expr_index);
  IDX(int_expr_index);
  IDX(secondary_int_expr_index);
  IDX(tertiary_int_expr_index);
  IDX(quaternary_int_expr_index);
  IDX(float_expr_index);
  IDX(secondary_float_expr_index);
  IDX(tertiary_float_expr_index);
  IDX(string_expr_index);
  f.vector("vector_value", e.vector_value, d.vector_value);
  f.vector("secondary_vector_value", e.secondary_vector_value, d.secondary_vector_value);
  f.vector("tertiary_vector_value", e.tertiary_vector_value, d.tertiary_vector_value);
  f.vector("quaternary_vector_value", e.quaternary_vector_value, d.quaternary_vector_value);
  REAL(float_value);
  REAL(secondary_float_value);
  REAL(tertiary_float_value);
  NUM(int_value);
  NUM(secondary_int_value);
  NUM(ray_input_mode);
  NUM(interaction_view);
  NUM(interaction_domain);
  FLAG(grid_horizontal);
  NUM(ui_propagation);
  NUM(ray_query_detail_flags);
  NUM(shape_cast_type);
  NUM(mesh_ray_domain);
  NUM(mesh_ray_direction);
  NUM(distance_endpoint_a);
  NUM(distance_endpoint_b);
  NUM(average_center_source);
  NUM(cache_key);
  IDX(runtime_state_index);
  FLAG(bool_value);
  f.indices("input_indices", e.input_indices);
  return out.str();
}

template<typename T, typename Text>
void table(std::ostringstream &out, const char *title, const std::vector<T> &items, Text text)
{
  out << title << " (" << items.size() << ")\n";
  for (size_t index = 0; index < items.size(); index++) {
    out << "  [" << index << "] " << text(items[index]) << "\n";
  }
}

}  // namespace

std::string LN_DumpProgramText(const LN_Program &program)
{
  std::ostringstream out;
  LN_DumpNames names;
  out << "program " << LN_DumpQuoted(program.GetSourceTreeName()) << " library "
      << LN_DumpQuoted(program.GetSourceTreeLibraryPath()) << " version "
      << program.GetProgramVersion() << "/" << program.GetSchemaVersion() << "\n";
  out << "step_gates (" << program.GetStepGates().size() << ")";
  for (const LN_StepGate &gate : program.GetStepGates()) {
    out << " every" << gate.interval << (gate.spread ? "+spread" : "");
  }
  out << "\n";
  table(out, "game_properties", program.GetGamePropertyRefs(), [&](const LN_GamePropertyRef &p) {
    return LN_DumpQuoted(p.name) + " type=" + std::to_string(int(p.value_type)) +
           " default=" + LN_DumpValue(p.default_value, names);
  });
  table(out, "tree_properties", program.GetTreePropertyRefs(), [&](const LN_TreePropertyRef &p) {
    return LN_DumpQuoted(p.name) + " type=" + std::to_string(int(p.value_type)) +
           " default=" + LN_DumpValue(p.default_value, names);
  });
  for (const LN_Event event : {LN_Event::OnInit, LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    const char *title = event == LN_Event::OnInit   ? "instructions OnInit" :
                        event == LN_Event::OnUpdate ? "instructions OnUpdate" :
                                                      "instructions OnFixedUpdate";
    table(out, title, program.GetInstructions(event), [&](const LN_Instruction &e) {
      return instruction_text(e, names);
    });
    const LN_ExecBlockProgram &blocks = program.GetExecBlockProgram(event);
    out << "blocks " << (event == LN_Event::OnInit   ? "OnInit" :
                         event == LN_Event::OnUpdate ? "OnUpdate" :
                                                       "OnFixedUpdate")
        << " (" << blocks.blocks.size() << ") direct=" << blocks.direct_instruction_count
        << " fallback=" << blocks.fallback_instruction_count
        << " all_gated=" << blocks.all_blocks_step_gated << " valid=" << blocks.valid << "\n";
    for (size_t index = 0; index < blocks.blocks.size(); index++) {
      const LN_ExecBlock &block = blocks.blocks[index];
      out << "  [" << index << "] instructions " << block.first_instruction << "+"
          << block.instruction_count << " ops " << block.first_op << "+" << block.op_count;
      if (block.loop_frame_index != LN_INVALID_INDEX) {
        out << " loop=" << block.loop_frame_index;
      }
      if (block.completed_loop_frame_index != LN_INVALID_INDEX) {
        out << " completed_loop=" << block.completed_loop_frame_index;
      }
      if (block.step_gate_index != LN_INVALID_INDEX) {
        out << " gate=" << block.step_gate_index;
      }
      out << (block.ui_event_route ? " ui_route" : "") << "\n";
    }
  }
  table(out, "bool", program.GetBoolExpressions(), bool_text);
  table(out, "float", program.GetFloatExpressions(), float_text);
  table(out, "int", program.GetIntExpressions(), int_text);
  table(out, "string", program.GetStringExpressions(), string_text);
  table(out, "vector", program.GetVectorExpressions(), vector_text);
  table(out, "color", program.GetColorExpressions(), color_text);
  table(out, "value", program.GetValueExpressions(),
        [&](const LN_ValueExpression &e) { return value_text(e, names); });
  table(out, "query", program.GetQueryExpressions(), query_text);
  table(out, "vector_payloads", program.GetVectorCommandPayloads(),
        [&](const LN_VectorCommandPayload &e) {
          const LN_VectorCommandPayload d;
          std::ostringstream text;
          Fields f(text);
          IDX(object_value_expr_index);
          IDX(vector_expr_index);
          IDX(secondary_vector_expr_index);
          IDX(bool_expr_index);
          f.vector("vector_value", e.vector_value, d.vector_value);
          f.vector("secondary_vector_value", e.secondary_vector_value, d.secondary_vector_value);
          NUM(operation_mode);
          NUM(operation_channel);
          NUM(operation_mask);
          FLAG(bool_value);
          return text.str();
        });
  table(out, "game_property_payloads", program.GetGamePropertyCommandPayloads(),
        [&](const LN_GamePropertyCommandPayload &e) {
          const LN_GamePropertyCommandPayload d;
          std::ostringstream text;
          Fields f(text);
          IDX(object_value_expr_index);
          IDX(property_ref_index);
          IDX(value_expr_index);
          IDX(bool_expr_index);
          IDX(int_expr_index);
          IDX(float_expr_index);
          IDX(string_expr_index);
          IDX(vector_expr_index);
          IDX(color_expr_index);
          NUM(value_type);
          NUM(modify_op);
          IDX(modify_min_expr_index);
          IDX(modify_max_expr_index);
          return text.str();
        });
  return out.str();
}
