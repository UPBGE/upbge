/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The order in which every expression kind reads its inputs (lean runtime plan, phases A13 and
 * A14, decision D20). C++ leaves the order of the operands of `a() + b()` and of function
 * arguments unspecified, so the order this GCC build uses is the behaviour today; it shows only
 * when two inputs read one stateful node, or one applies a pending move that the other reads
 * live. The kind functions of A13 and A14 spell the order out as sequenced locals, and this test
 * keeps them to it: for each kind of the Bool, Int, Float, String, Vector, Color, Value and Query
 * tables
 * an expression is built with every input field pointing at its own live (not foldable)
 * tree-property read, the kind is worked out once, and the reads it made are written down in
 * order ("B3" = bool expression 3; "-" = the invalid index), once with every input reading
 * false / 0 and once with true / nonzero (the reads that depend on a value: And, Or, Select). The text is a golden (`read_order_v0.txt`, recorded from the
 * code before the rewrite).
 *
 * Fields hold these indexes, the same in every family's table (a family's table 0..15 are the
 * property reads): input0..input4 = 0..4, then the other index fields in the order of their
 * struct, the vector of input indexes = 12, 13, 14, 15. */

#include "testing/testing.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "LN_KindCatalog.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

constexpr uint32_t kReads = 16;
/** The index of the expression under test in its table: after the property reads. */
constexpr uint32_t kUnderTest = kReads;

/** `truthy` false: every read gives false, 0, 0.0, "" and the zero vector (And stops after its
 * first input, Select takes its second); true: true, 3, 2.5, "ab", (1,2,3) (the other ways). */
LN_TreePropertyRef MakeRef(const char family,
                           const uint32_t index,
                           const LN_ValueType type,
                           const bool truthy)
{
  LN_TreePropertyRef ref;
  ref.name = std::string(1, family) + std::to_string(index);
  ref.value_type = type;
  ref.default_value.type = type == LN_ValueType::Generic ? LN_ValueType::Int : type;
  ref.default_value.exists = true;
  if (truthy) {
    ref.default_value.bool_value = true;
    ref.default_value.int_value = 3;
    ref.default_value.float_value = 2.5f;
    ref.default_value.string_value = "ab";
    ref.default_value.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
    ref.default_value.color_value = MT_Vector4(1.0f, 0.5f, 0.25f, 1.0f);
  }
  return ref;
}

/** The tables 0..15 of every family: a tree-property read each (live, so nothing folds or
 * merges). */
void AddPropertyReads(LN_Program &program, const bool truthy)
{
  for (uint32_t index = 0; index < kReads; index++) {
    {
      LN_BoolExpression e;
      e.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('B', index, LN_ValueType::Bool, truthy));
      program.AddBoolExpression(e);
    }
    {
      LN_IntExpression e;
      e.kind = LN_IntExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('I', index, LN_ValueType::Int, truthy));
      program.AddIntExpression(e);
    }
    {
      LN_FloatExpression e;
      e.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('F', index, LN_ValueType::Float, truthy));
      program.AddFloatExpression(e);
    }
    {
      LN_StringExpression e;
      e.kind = LN_StringExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('S', index, LN_ValueType::String, truthy));
      program.AddStringExpression(e);
    }
    {
      LN_VectorExpression e;
      e.kind = LN_VectorExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('V', index, LN_ValueType::Vector, truthy));
      program.AddVectorExpression(e);
    }
    {
      LN_ColorExpression e;
      e.kind = LN_ColorExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('C', index, LN_ValueType::Color, truthy));
      program.AddColorExpression(e);
    }
    {
      LN_ValueExpression e;
      e.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
      e.property_ref_index = program.AddTreePropertyRef(MakeRef('A', index, LN_ValueType::Generic, truthy));
      program.AddValueExpression(e);
    }
  }
}

/** The expression of `domain`/`value` with its index fields set; false for a domain this test
 * does not cover. Returns the family letter through `r_family`. */
bool AddKind(LN_Program &program,
             const LN_KindDomain domain,
             const uint16_t value,
             char &r_family)
{
  switch (domain) {
    case LN_KindDomain::Bool: {
      LN_BoolExpression e;
      e.kind = LN_BoolExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.float_expr_index = 3;
      e.secondary_float_expr_index = 4;
      e.int_expr_index = 5;
      r_family = 'B';
      EXPECT_EQ(program.AddBoolExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Int: {
      LN_IntExpression e;
      e.kind = LN_IntExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      r_family = 'I';
      EXPECT_EQ(program.AddIntExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Float: {
      LN_FloatExpression e;
      e.kind = LN_FloatExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.input_indices = {12, 13, 14, 15};
      e.bool_expr_index = 3;
      e.string_expr_index = 4;
      r_family = 'F';
      EXPECT_EQ(program.AddFloatExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::String: {
      LN_StringExpression e;
      e.kind = LN_StringExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.input3 = 3;
      e.input4 = 4;
      e.int_expr_index = 5;
      e.value_expr_index = 6;
      r_family = 'S';
      EXPECT_EQ(program.AddStringExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Vector: {
      LN_VectorExpression e;
      e.kind = LN_VectorExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.bool_expr_index = 3;
      e.float_expr_index = 4;
      r_family = 'V';
      EXPECT_EQ(program.AddVectorExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Color: {
      LN_ColorExpression e;
      e.kind = LN_ColorExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.input3 = 3;
      r_family = 'C';
      EXPECT_EQ(program.AddColorExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Value: {
      LN_ValueExpression e;
      e.kind = LN_ValueExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.input_indices = {12, 13, 14, 15};
      e.bool_expr_index = 3;
      e.string_expr_index = 4;
      r_family = 'A';
      EXPECT_EQ(program.AddValueExpression(e), kUnderTest);
      return true;
    }
    case LN_KindDomain::Query: {
      /* The inputs 0..9 keep their own index; the typed fields use 10..15; the condition input is
       * left out (it is read before the kind, and a false one stops the kind). */
      LN_QueryExpression e;
      e.kind = LN_QueryExpressionKind(value);
      e.input0 = 0;
      e.input1 = 1;
      e.input2 = 2;
      e.input3 = 3;
      e.input4 = 4;
      e.input5 = 5;
      e.input6 = 6;
      e.input7 = 7;
      e.input8 = 8;
      e.input9 = 9;
      e.ignore_list_expr_index = 11;
      e.bool_expr_index = 10;
      e.secondary_bool_expr_index = 11;
      e.tertiary_bool_expr_index = 12;
      e.quaternary_bool_expr_index = 13;
      e.quinary_bool_expr_index = 14;
      e.senary_bool_expr_index = 15;
      e.int_expr_index = 10;
      e.secondary_int_expr_index = 11;
      e.tertiary_int_expr_index = 12;
      e.quaternary_int_expr_index = 13;
      e.float_expr_index = 10;
      e.secondary_float_expr_index = 11;
      e.tertiary_float_expr_index = 12;
      e.string_expr_index = 10;
      e.input_indices = {12, 13, 14, 15};
      r_family = 'Q';
      EXPECT_EQ(program.AddQueryExpression(e), 0u);
      return true;
    }
    default:
      return false;
  }
}

/** Kinds that cannot be worked out in a rig (no scene, no window, no file); the reason is in the
 * list. Their order is covered by reading their code. */
bool Unrecordable(const std::string &name, const char **r_why)
{
  static const struct {
    const char *name;
    const char *why;
  } skipped[] = {
      /* filled in while recording, see the test */
  };
  for (const auto &item : skipped) {
    if (name == item.name) {
      *r_why = item.why;
      return true;
    }
  }
  return false;
}

/** The name of the field of the expression under test that held `index` (see the top of the
 * file), for a read of `family`. */
std::string FieldNameOf(const LN_KindDomain domain, const char family, const uint32_t index)
{
  if (index == kUnderTest && family != 'Q') {
    return "(self)";
  }
  if (index < 10 && (domain == LN_KindDomain::Query || index < 5)) {
    return "input" + std::to_string(index);
  }
  if (index >= 12 && index <= 15 && (domain == LN_KindDomain::Float ||
                                    domain == LN_KindDomain::Value ||
                                    domain == LN_KindDomain::Query))
  {
    return "input_indices[" + std::to_string(index - 12) + "]";
  }
  switch (domain) {
    case LN_KindDomain::Bool:
      return index == 3 ? "float_expr_index" :
             index == 4 ? "secondary_float_expr_index" :
             index == 5 ? "int_expr_index" :
                          "?" + std::to_string(index);
    case LN_KindDomain::Float:
    case LN_KindDomain::Value:
      return index == 3 ? "bool_expr_index" :
             index == 4 ? "string_expr_index" :
                          "?" + std::to_string(index);
    case LN_KindDomain::String:
      return index == 5 ? "int_expr_index" :
             index == 6 ? "value_expr_index" :
                          "?" + std::to_string(index);
    case LN_KindDomain::Vector:
      return index == 3 ? "bool_expr_index" :
             index == 4 ? "float_expr_index" :
                          "?" + std::to_string(index);
    case LN_KindDomain::Query:
      if (family == 'B') {
        static const char *const names[] = {"bool_expr_index",
                                            "secondary_bool_expr_index",
                                            "tertiary_bool_expr_index",
                                            "quaternary_bool_expr_index",
                                            "quinary_bool_expr_index",
                                            "senary_bool_expr_index"};
        return index >= 10 && index <= 15 ? names[index - 10] : "?" + std::to_string(index);
      }
      if (family == 'I') {
        static const char *const names[] = {"int_expr_index",
                                            "secondary_int_expr_index",
                                            "tertiary_int_expr_index",
                                            "quaternary_int_expr_index"};
        return index >= 10 && index <= 13 ? names[index - 10] : "?" + std::to_string(index);
      }
      if (family == 'F') {
        static const char *const names[] = {
            "float_expr_index", "secondary_float_expr_index", "tertiary_float_expr_index"};
        return index >= 10 && index <= 12 ? names[index - 10] : "?" + std::to_string(index);
      }
      if (family == 'S' && index == 10) {
        return "string_expr_index";
      }
      if (family == 'A' && index == 11) {
        return "ignore_list_expr_index";
      }
      return "?" + std::to_string(index);
    default:
      return "?" + std::to_string(index);
  }
}

/** kind name -> "field:Family" of every read seen, over all the recordings of a run. */
using KindInputs = std::map<std::string, std::set<std::string>>;

/** The text of the golden: one line per kind, "Float::Add: F0 F1". */
std::string RecordReadOrders(const std::vector<LN_KindDomain> &domains,
                             const bool truthy,
                             const bool as_rotation = false,
                             KindInputs *inputs = nullptr,
                             const bool lean = false)
{
  std::string text;
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  const bool verbose = std::getenv("LN_READ_ORDER_VERBOSE") != nullptr;
  for (size_t row = 0; row < count; row++) {
    const LN_KindInfo &info = rows[row];
    bool wanted = false;
    for (const LN_KindDomain domain : domains) {
      wanted |= info.domain == domain;
    }
    if (!wanted) {
      continue;
    }
    const char *why = nullptr;
    if (Unrecordable(info.name, &why)) {
      text += std::string(info.name) + ": (not recorded: " + why + ")\n";
      continue;
    }
    if (verbose) {
      std::fprintf(stderr, "read order: %s\n", info.name);
      std::fflush(stderr);
    }
    const std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
    AddPropertyReads(*program, truthy);
    char family = '?';
    if (!AddKind(*program, info.domain, info.value, family)) {
      continue;
    }
    if (as_rotation) {
      family = 'R';
    }
    SemRig rig(program, kMemoOff);
    std::vector<LN_RuntimeTree::ReadTraceEntry> trace;
    LN_TickContext context;
    context.use_fixed_timestep = true;
    context.tick_index = 1;
    LN_RuntimeTree::SetReadTraceForTests(&trace);
    rig.Tree(0).EvaluateExpressionForTests(
        family, family == 'Q' ? 0 : kUnderTest, context, lean);
    LN_RuntimeTree::SetReadTraceForTests(nullptr);
    text += std::string(info.name) + ":";
    if (inputs != nullptr) {
      std::set<std::string> &fields = (*inputs)[info.name];
      for (size_t index = 1; index < trace.size(); index++) {
        fields.insert(FieldNameOf(info.domain, trace[index].family, trace[index].index) + ":" +
                      trace[index].family);
      }
    }
    for (size_t index = 1; index < trace.size(); index++) { /* [0] is the call under test */
      text += std::string(" ") + trace[index].family +
              (trace[index].index == LN_INVALID_INDEX ? std::string("-") :
                                                          std::to_string(trace[index].index));
    }
    text += "\n";
  }
  return text;
}

}  // namespace

/** The text of the read-order golden, read through the Resolve functions or the lean reader. */
static std::string ReadOrderText(const bool lean)
{
  const std::vector<LN_KindDomain> domains = {LN_KindDomain::Bool,
                                              LN_KindDomain::Int,
                                              LN_KindDomain::Float,
                                              LN_KindDomain::String,
                                              LN_KindDomain::Vector,
                                              LN_KindDomain::Color,
                                              LN_KindDomain::Value,
                                              LN_KindDomain::Query};
  return "== every input reads false / 0 / empty\n" +
         RecordReadOrders(domains, false, false, nullptr, lean) +
         "== every input reads true / 3 / 2.5 / ab\n" +
         RecordReadOrders(domains, true, false, nullptr, lean) +
         "== vector kinds read as rotations, zero\n" +
         RecordReadOrders({LN_KindDomain::Vector}, false, true, nullptr, lean) +
         "== vector kinds read as rotations, nonzero\n" +
         RecordReadOrders({LN_KindDomain::Vector}, true, true, nullptr, lean);
}

/* Every kind of the covered tables reads its inputs in the order of the golden. */
TEST(LN_ReadOrder, EveryKindReadsItsInputsInTheRecordedOrder)
{
  const std::string text = ReadOrderText(false);
  EXPECT_GT(text.size(), 1000u);
  const std::string verdict = CheckGolden("read_order_v0", text);
  EXPECT_TRUE(verdict.empty()) << verdict;
}

/* The lean reader (plan phase A16) reads in the very same order: its kinds run the same code,
 * and the reads go through the lean reader's functions. */
TEST(LN_ReadOrder, EveryKindReadsItsInputsInTheSameOrderOnTheLeanReader)
{
  const std::string text = ReadOrderText(true);
  const std::string verdict = CheckGolden("read_order_v0", text);
  EXPECT_TRUE(verdict.empty()) << verdict;
}

/* The inputs each kind was seen to read (plan phase A14): the fields of its expression that held
 * an index of another expression, with the table read ("input0:F" = input0 is a float expression).
 * Taken from the same recordings; a field read only on a path the recordings do not take is not in
 * the list (the kinds' code is the whole truth). A new kind needs a line here, written by running
 * the test with LN_RECORD_GOLDEN=1. */
TEST(LN_ReadOrder, TheInputsOfEveryKindAreListed)
{
  KindInputs inputs;
  const std::vector<LN_KindDomain> domains = {LN_KindDomain::Bool,
                                              LN_KindDomain::Int,
                                              LN_KindDomain::Float,
                                              LN_KindDomain::String,
                                              LN_KindDomain::Vector,
                                              LN_KindDomain::Color,
                                              LN_KindDomain::Value,
                                              LN_KindDomain::Query};
  RecordReadOrders(domains, false, false, &inputs);
  RecordReadOrders(domains, true, false, &inputs);
  RecordReadOrders({LN_KindDomain::Vector}, false, true, &inputs);
  RecordReadOrders({LN_KindDomain::Vector}, true, true, &inputs);
  std::string text;
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  for (size_t row = 0; row < count; row++) {
    const auto found = inputs.find(rows[row].name);
    if (found == inputs.end()) {
      continue;
    }
    text += std::string(rows[row].name) + ":";
    for (const std::string &field : found->second) {
      text += " " + field;
    }
    text += "\n";
  }
  EXPECT_GT(text.size(), 1000u);
  const std::string verdict = CheckGolden("kind_inputs_v0", text);
  EXPECT_TRUE(verdict.empty()) << verdict;
}
