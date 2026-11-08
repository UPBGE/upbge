/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Gap tests of node values (lean runtime plan, phase A4): the value rules of the data nodes the
 * lean runtime will take over, with edge inputs, against a model written here from the node's
 * contract. A node is built alone in a tree (inputs read live from game properties so the compiler
 * cannot fold them; the folded twin gets the same numbers as constants), run for one step, and
 * its outputs compared bit for bit with the model and across the modes (production, memo on, memo
 * off, profiled). Where today's result is not the model's (a rule the model could not guess) the
 * test says so in the model.
 *
 * The state nodes (Store Value, Value Changed, Boolean Edge, Cooldown, Flip Flop ...) run over
 * several steps against the sequence their contract gives. */

#include "testing/testing.h"

#include <cmath>
#include <cstdarg>
#include <functional>
#include <limits>
#include <set>
#include <map>

#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/** The numbers the value tests feed to a pin: ordinary, edge and special values. */
const std::vector<float> &Samples()
{
  static const std::vector<float> samples = {
      0.0f,    -0.0f,  1.0f,    -1.0f,  0.5f,   -0.5f,  2.5f,    -2.5f,  3.0f,   -7.25f,
      100.0f,  1.0e-21f, -1.0e-21f, 1.0e-30f, 1.0e30f, -1.0e30f, kInf,   -kInf,  kNaN,   3.14159274f,
  };
  return samples;
}

/** One kind of pin or output the evaluator wires. */
enum class Kind : uint8_t { Float, Int, Bool, Vector };

struct PinSpec {
  const char *pin;
  Kind kind;
};

/** A value fed to a pin or read from an output (a vector uses all three floats). */
struct Value {
  float f[3] = {0.0f, 0.0f, 0.0f};
};

/** [On Logic + Physics Step -> one Set Game Property per output] around the node `idname`
 * (settings c1/c2). Live: the inputs are read from game properties "in<k>..." (the compiler cannot
 * fold them) and the program is made once and run in a rig per mode, row after row; folded: the
 * inputs are constants of one row. An output is written to "out<k>" (a Vector: "out<k>X" ...);
 * a Bool is read back as 0 or 1, an Int as a float. */
class NodeRunner {
 public:
  NodeRunner(const char *idname,
             const int16_t c1,
             const int16_t c2,
             const std::vector<PinSpec> &pins,
             const std::vector<PinSpec> &outputs,
             const std::vector<Value> *folded_inputs = nullptr)
      : m_pins(pins), m_outputs(outputs)
  {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &node = b.Add(idname, c1, c2);
    const char *axes[3] = {"X", "Y", "Z"};
    for (size_t k = 0; k < pins.size(); k++) {
      const PinSpec &spec = pins[k];
      if (folded_inputs != nullptr) {
        const Value &value = (*folded_inputs)[k];
        switch (spec.kind) {
          case Kind::Float:
            b.Float(node, spec.pin, value.f[0]);
            break;
          case Kind::Int:
            b.Int(node, spec.pin, int32_t(value.f[0]));
            break;
          case Kind::Bool:
            b.Bool(node, spec.pin, value.f[0] != 0.0f);
            break;
          case Kind::Vector:
            b.Vector(node, spec.pin, value.f[0], value.f[1], value.f[2]);
            break;
        }
        continue;
      }
      const auto reader = [&](const char *reader_name, const std::string &property) -> GenNode & {
        GenNode &r = b.Add(reader_name);
        b.String(r, "Property", property.c_str());
        m_names.push_back(property);
        return r;
      };
      switch (spec.kind) {
        case Kind::Float:
          b.Link(reader("LogicNativeGetGamePropertyFloat", "in" + std::to_string(k)),
                 "Value",
                 node,
                 spec.pin);
          break;
        case Kind::Int:
          b.Link(reader("LogicNativeGetGamePropertyInt", "in" + std::to_string(k)),
                 "Value",
                 node,
                 spec.pin);
          break;
        case Kind::Bool:
          b.Link(reader("LogicNativeGetGamePropertyBool", "in" + std::to_string(k)),
                 "Value",
                 node,
                 spec.pin);
          break;
        case Kind::Vector: {
          GenNode &combine = b.Add("LogicNativeCombineXYZ");
          for (int axis = 0; axis < 3; axis++) {
            b.Link(reader("LogicNativeGetGamePropertyFloat",
                          "in" + std::to_string(k) + axes[axis]),
                   "Value",
                   combine,
                   axes[axis]);
          }
          b.Link(combine, "Vector", node, spec.pin);
          break;
        }
      }
    }
    GenNode *flow = &event;
    const char *flow_pin = "Out";
    for (size_t k = 0; k < outputs.size(); k++) {
      const PinSpec &spec = outputs[k];
      const auto store = [&](const char *store_name,
                             const std::string &property,
                             GenNode &source,
                             const char *source_pin) {
        GenNode &writer = b.Add(store_name);
        b.String(writer, "Property", property.c_str());
        b.Link(*flow, flow_pin, writer, "Flow");
        b.Link(source, source_pin, writer, "Value");
        flow = &writer;
        flow_pin = "Done";
      };
      switch (spec.kind) {
        case Kind::Float:
          store("LogicNativeSetGamePropertyFloat", "out" + std::to_string(k), node, spec.pin);
          break;
        case Kind::Int:
          store("LogicNativeSetGamePropertyInt", "out" + std::to_string(k), node, spec.pin);
          break;
        case Kind::Bool:
          store("LogicNativeSetGamePropertyBool", "out" + std::to_string(k), node, spec.pin);
          break;
        case Kind::Vector: {
          GenNode &split = b.Add("LogicNativeSeparateXYZ");
          b.Link(node, spec.pin, split, "Vector");
          for (int axis = 0; axis < 3; axis++) {
            store("LogicNativeSetGamePropertyFloat",
                  "out" + std::to_string(k) + axes[axis],
                  split,
                  axes[axis]);
          }
          break;
        }
      }
    }
    std::string report;
    m_program = b.Compile(&report);
    EXPECT_NE(m_program, nullptr) << idname << ": " << report;
    m_builder = std::make_unique<TreeBuilder>(std::move(b));
  }

  /** One step in the rig of `mode` with `inputs` (live programs; a folded program ignores them). */
  std::vector<Value> Run(const SemMode &mode, const std::vector<Value> &inputs)
  {
    std::vector<Value> result(m_outputs.size());
    if (m_program == nullptr) {
      return result;
    }
    std::unique_ptr<SemRig> &rig = m_rigs[mode.name];
    if (!rig) {
      rig = std::make_unique<SemRig>(m_program, mode);
    }
    KX_GameObject &owner = rig->Object(0);
    size_t name_index = 0;
    for (size_t k = 0; k < m_pins.size() && !m_names.empty(); k++) {
      switch (m_pins[k].kind) {
        case Kind::Float:
          rig->SetFloat(owner, m_names[name_index++].c_str(), inputs[k].f[0]);
          break;
        case Kind::Int:
          rig->SetInt(owner, m_names[name_index++].c_str(), int(inputs[k].f[0]));
          break;
        case Kind::Bool:
          rig->SetBool(owner, m_names[name_index++].c_str(), inputs[k].f[0] != 0.0f);
          break;
        case Kind::Vector:
          for (int axis = 0; axis < 3; axis++) {
            rig->SetFloat(owner, m_names[name_index++].c_str(), inputs[k].f[axis]);
          }
          break;
      }
    }
    rig->Step();
    const char *axes[3] = {"X", "Y", "Z"};
    for (size_t k = 0; k < m_outputs.size(); k++) {
      if (m_outputs[k].kind == Kind::Vector) {
        for (int axis = 0; axis < 3; axis++) {
          result[k].f[axis] = rig->Float(
              owner, ("out" + std::to_string(k) + axes[axis]).c_str(), kNaN);
        }
      }
      else {
        result[k].f[0] = rig->Float(owner, ("out" + std::to_string(k)).c_str(), kNaN);
      }
    }
    return result;
  }

 private:
  std::vector<PinSpec> m_pins;
  std::vector<PinSpec> m_outputs;
  std::vector<std::string> m_names;
  std::unique_ptr<TreeBuilder> m_builder;
  std::shared_ptr<LN_Program> m_program;
  std::map<std::string, std::unique_ptr<SemRig>> m_rigs;
};

/** Equal as the runtime means it: the same bits, any NaN equal to any NaN. */
bool SameFloat(const float a, const float b)
{
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return Bits(a) == Bits(b);
}

std::string Describe(const std::vector<Value> &values, const std::vector<PinSpec> &specs)
{
  std::string out;
  for (size_t k = 0; k < values.size(); k++) {
    const int count = specs[k].kind == Kind::Vector ? 3 : 1;
    for (int i = 0; i < count; i++) {
      char text[40];
      std::snprintf(text, sizeof(text), " %g(%s)", values[k].f[i], Bits(values[k].f[i]).c_str());
      out += text;
    }
  }
  return out;
}

/** Runs the node for every input row in every mode and folded, and compares each result with
 * `model` (inputs -> outputs). */
void CheckNode(const char *idname,
               const int16_t c1,
               const int16_t c2,
               const std::vector<PinSpec> &pins,
               const std::vector<PinSpec> &outputs,
               const std::vector<std::vector<Value>> &rows,
               const std::function<std::vector<Value>(const std::vector<Value> &)> &model,
               const bool check_folded = true)
{
  NodeRunner live(idname, c1, c2, pins, outputs);
  for (const std::vector<Value> &row : rows) {
    const std::vector<Value> expected = model(row);
    for (const SemMode &mode : {kProduction, kMemoOn, kMemoOff, kProfiled}) {
      const std::vector<Value> got = live.Run(mode, row);
      for (size_t k = 0; k < outputs.size(); k++) {
        const int count = outputs[k].kind == Kind::Vector ? 3 : 1;
        for (int i = 0; i < count; i++) {
          if (!SameFloat(got[k].f[i], expected[k].f[i])) {
            ADD_FAILURE() << idname << " c1=" << c1 << " c2=" << c2 << " [" << mode.name
                          << "] inputs" << Describe(row, pins) << "\n  gave" << Describe(got, outputs)
                          << "\n  model" << Describe(expected, outputs);
            return;
          }
        }
      }
    }
    if (check_folded) {
      NodeRunner twin(idname, c1, c2, pins, outputs, &row);
      const std::vector<Value> folded = twin.Run(kMemoOff, row);
      for (size_t k = 0; k < outputs.size(); k++) {
        const int count = outputs[k].kind == Kind::Vector ? 3 : 1;
        for (int i = 0; i < count; i++) {
          /* Folding differs from the runtime in the sign of a zero today (plan A4, quirk tests):
           * a zero compares equal to the other zero here, nothing else may differ. */
          const float a = folded[k].f[i], e = expected[k].f[i];
          if (!(SameFloat(a, e) || (a == 0.0f && e == 0.0f))) {
            ADD_FAILURE() << idname << " c1=" << c1 << " c2=" << c2 << " folded inputs"
                          << Describe(row, pins) << "\n  gave" << Describe(folded, outputs)
                          << "\n  model" << Describe(expected, outputs);
            return;
          }
        }
      }
    }
  }
}

std::vector<std::vector<Value>> Rows1()
{
  std::vector<std::vector<Value>> rows;
  for (const float a : Samples()) {
    rows.push_back({Value{{a, 0, 0}}});
  }
  return rows;
}

std::vector<std::vector<Value>> Rows2()
{
  /* Every pair of a smaller set: all of the ordinary values, the specials against a few. */
  const std::vector<float> small = {0.0f, 1.0f, -1.0f, 2.5f, -7.25f, 1.0e-21f, 1.0e30f, kInf, kNaN};
  std::vector<std::vector<Value>> rows;
  for (const float a : Samples()) {
    for (const float b : small) {
      rows.push_back({Value{{a, 0, 0}}, Value{{b, 0, 0}}});
    }
  }
  return rows;
}

/* ---------------------------------------------------------------------------------------------
 * Math: the 19 operations, against the model of the runtime's rules (a divisor under 1e-20 gives
 * 0 for Divide and Modulo; Round is floor(x + 0.5); Fraction is x - floor(x); Sign is -1, 0, 1).
 * ------------------------------------------------------------------------------------------- */
TEST(LN_NodeValues, MathOperationsGiveTheirModelWithEdgeInputs)
{
  constexpr float pi = 3.14159265358979323846f;
  using Op = std::function<float(float, float)>;
  const struct {
    int16_t op;
    Op model;
  } ops[] = {
      {blender::NODE_MATH_ADD, [](float a, float b) { return a + b; }},
      {blender::NODE_MATH_SUBTRACT, [](float a, float b) { return a - b; }},
      {blender::NODE_MATH_MULTIPLY, [](float a, float b) { return a * b; }},
      {blender::NODE_MATH_DIVIDE, [](float a, float b) { return std::fabs(b) <= 1.0e-20f ? 0.0f : a / b; }},
      {blender::NODE_MATH_POWER, [](float a, float b) { return std::pow(a, b); }},
      {blender::NODE_MATH_MINIMUM, [](float a, float b) { return std::min(a, b); }},
      {blender::NODE_MATH_MAXIMUM, [](float a, float b) { return std::max(a, b); }},
      {blender::NODE_MATH_ABSOLUTE, [](float a, float) { return std::fabs(a); }},
      {blender::NODE_MATH_SIGN, [](float a, float) { return float((a > 0.0f) - (a < 0.0f)); }},
      {blender::NODE_MATH_ROUND, [](float a, float) { return std::floor(a + 0.5f); }},
      {blender::NODE_MATH_FLOOR, [](float a, float) { return std::floor(a); }},
      {blender::NODE_MATH_CEIL, [](float a, float) { return std::ceil(a); }},
      {blender::NODE_MATH_TRUNC, [](float a, float) { return std::trunc(a); }},
      {blender::NODE_MATH_FRACTION, [](float a, float) { return a - std::floor(a); }},
      {blender::NODE_MATH_MODULO, [](float a, float b) { return std::fabs(b) <= 1.0e-20f ? 0.0f : std::fmod(a, b); }},
      {blender::NODE_MATH_SINE, [](float a, float) { return std::sin(a); }},
      {blender::NODE_MATH_COSINE, [](float a, float) { return std::cos(a); }},
      {blender::NODE_MATH_RADIANS, [pi](float a, float) { return a * (pi / 180.0f); }},
      {blender::NODE_MATH_DEGREES, [pi](float a, float) { return a * (180.0f / pi); }},
  };
  for (const auto &entry : ops) {
    SCOPED_TRACE("Math operation " + std::to_string(entry.op));
    CheckNode("LogicNativeMath",
              entry.op,
              0,
              {{"A", Kind::Float}, {"B", Kind::Float}},
              {{"Result", Kind::Float}},
              Rows2(),
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{Value{{entry.model(in[0].f[0], in[1].f[0]), 0, 0}}};
              });
  }
}


std::vector<std::vector<Value>> RowsOf(const size_t pins, const std::vector<float> &values)
{
  std::vector<std::vector<Value>> rows;
  std::vector<size_t> at(pins, 0);
  for (;;) {
    std::vector<Value> row(pins);
    for (size_t k = 0; k < pins; k++) {
      row[k].f[0] = values[at[k]];
    }
    rows.push_back(row);
    size_t k = 0;
    while (k < pins && ++at[k] == values.size()) {
      at[k++] = 0;
    }
    if (k == pins) {
      break;
    }
  }
  return rows;
}

Value Vec(const float x, const float y, const float z)
{
  return Value{{x, y, z}};
}

const std::vector<Value> &VectorSamples()
{
  static const std::vector<Value> samples = {
      Vec(0, 0, 0), Vec(1, 2, 3),   Vec(-1, 0.5f, 2),        Vec(3, 4, 0),     Vec(1.0e-21f, 0, 0),
      Vec(1.0e30f, 1.0e30f, 1.0e30f), Vec(kInf, 0, -1),       Vec(kNaN, 1, 1), Vec(-0.0f, -0.0f, -0.0f),
      Vec(0, 7, -2.5f),
  };
  return samples;
}

/* ---- Vector Math: the nine operations, per component (Divide gives 0 under a 1e-20 divisor
 * component, Normalize gives (0,0,0) for a vector no longer than 1e-20). ---- */
TEST(LN_NodeValues, VectorMathOperationsGiveTheirModelWithEdgeInputs)
{
  using Model = std::function<Value(const Value &, const Value &, float)>;
  const auto per_component = [](const std::function<float(float, float)> &f) -> Model {
    return [f](const Value &a, const Value &b, float) {
      return Vec(f(a.f[0], b.f[0]), f(a.f[1], b.f[1]), f(a.f[2], b.f[2]));
    };
  };
  const struct {
    int16_t op;
    Model model;
  } ops[] = {
      {blender::NODE_VECTOR_MATH_ADD, per_component([](float a, float b) { return a + b; })},
      {blender::NODE_VECTOR_MATH_SUBTRACT, per_component([](float a, float b) { return a - b; })},
      {blender::NODE_VECTOR_MATH_MULTIPLY, per_component([](float a, float b) { return a * b; })},
      {blender::NODE_VECTOR_MATH_DIVIDE,
       per_component([](float a, float b) { return std::fabs(b) <= 1.0e-20f ? 0.0f : a / b; })},
      {blender::NODE_VECTOR_MATH_ABSOLUTE,
       per_component([](float a, float) { return std::fabs(a); })},
      {blender::NODE_VECTOR_MATH_MINIMUM, per_component([](float a, float b) { return std::min(a, b); })},
      {blender::NODE_VECTOR_MATH_MAXIMUM, per_component([](float a, float b) { return std::max(a, b); })},
      {blender::NODE_VECTOR_MATH_SCALE,
       [](const Value &a, const Value &, float scale) {
         return Vec(a.f[0] * scale, a.f[1] * scale, a.f[2] * scale);
       }},
      {blender::NODE_VECTOR_MATH_NORMALIZE,
       [](const Value &a, const Value &, float) {
         const MT_Vector3 v(a.f[0], a.f[1], a.f[2]);
         const float length = v.length();
         if (length <= 1.0e-20f) {
           return Vec(0, 0, 0);
         }
         const MT_Vector3 n = v * (1.0f / length);
         return Vec(n.x(), n.y(), n.z());
       }},
  };
  const std::vector<float> scales = {0.0f, 1.0f, -2.5f, kNaN};
  for (const auto &entry : ops) {
    SCOPED_TRACE("Vector Math operation " + std::to_string(entry.op));
    std::vector<std::vector<Value>> rows;
    for (const Value &a : VectorSamples()) {
      for (const Value &b : VectorSamples()) {
        for (const float scale : scales) {
          if ((entry.op != blender::NODE_VECTOR_MATH_SCALE) && scale != 1.0f) {
            continue;
          }
          rows.push_back({a, b, Value{{scale, 0, 0}}});
        }
      }
    }
    CheckNode("LogicNativeVectorMath",
              entry.op,
              0,
              {{"A", Kind::Vector}, {"B", Kind::Vector}, {"Scale", Kind::Float}},
              {{"Result", Kind::Vector}},
              rows,
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{entry.model(in[0], in[1], in[2].f[0])};
              });
  }
}

/* ---- Compare: Equal and Not Equal use a tolerance of 1e-6; Equal of two equal values is true
 * also for infinities (inf - inf is NaN: Equal(inf, inf) was false until 2026-11-08; Not Equal
 * gives false there, as before); the others are the plain comparisons. ---- */
TEST(LN_NodeValues, CompareGivesItsSixOperationsWithAToleranceForEquality)
{
  using Model = std::function<bool(float, float)>;
  const Model models[6] = {
      [](float a, float b) { return a == b || std::fabs(a - b) <= 1.0e-6f; },
      [](float a, float b) { return std::fabs(a - b) > 1.0e-6f; },
      [](float a, float b) { return a > b; },
      [](float a, float b) { return a < b; },
      [](float a, float b) { return a >= b; },
      [](float a, float b) { return a <= b; },
  };
  for (int16_t op = 0; op < 6; op++) {
    SCOPED_TRACE("Compare operation " + std::to_string(op));
    CheckNode("LogicNativeCompare",
              op,
              0,
              {{"A", Kind::Float}, {"B", Kind::Float}},
              {{"Result", Kind::Bool}},
              Rows2(),
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{Value{{models[op](in[0].f[0], in[1].f[0]) ? 1.0f : 0.0f, 0, 0}}};
              });
  }
}

/* ---- Map Range (Clamp off / on), Clamp and Limit Range (the same node underneath, min and max
 * swapped when reversed), Invert, Value Switch. ---- */
TEST(LN_NodeValues, MapRangeMapsWithAZeroRangeGivingTheStartAndAClampOption)
{
  const std::vector<float> values = {0.0f, 1.0f, -2.5f, 10.0f, 1.0e30f, kInf, kNaN};
  for (const int16_t clamp : {0, 1}) {
    SCOPED_TRACE(clamp ? "clamped" : "not clamped");
    CheckNode("LogicNativeMapRange",
              clamp,
              0,
              {{"Value", Kind::Float},
               {"From Min", Kind::Float},
               {"From Max", Kind::Float},
               {"To Min", Kind::Float},
               {"To Max", Kind::Float}},
              {{"Result", Kind::Float}},
              RowsOf(5, {0.0f, 1.0f, -2.5f, 10.0f, kNaN}),
              [&](const std::vector<Value> &in) {
                const float value = in[0].f[0], from_min = in[1].f[0], from_max = in[2].f[0];
                const float to_min = in[3].f[0], to_max = in[4].f[0];
                const float range = from_max - from_min;
                const float normalized = range == 0.0f ? 0.0f : (value - from_min) / range;
                float mapped = to_min + normalized * (to_max - to_min);
                if (clamp != 0) {
                  mapped = std::min(std::max(mapped, std::min(to_min, to_max)), std::max(to_min, to_max));
                }
                return std::vector<Value>{Value{{mapped, 0, 0}}};
              });
  }
  (void)values;
}

/* (Limit Range did the same until it was removed, 2026-11-08.) */
TEST(LN_NodeValues, ClampSwapsMinAndMaxWhenReversed)
{
  for (const char *idname : {"LogicNativeClampValue"}) {
    SCOPED_TRACE(idname);
    CheckNode(idname,
              0,
              0,
              {{"Value", Kind::Float}, {"Min", Kind::Float}, {"Max", Kind::Float}},
              {{"Value", Kind::Float}},
              RowsOf(3, {0.0f, 1.0f, -2.5f, 10.0f, kInf, kNaN}),
              [](const std::vector<Value> &in) {
                const float lower = std::min(in[1].f[0], in[2].f[0]);
                const float upper = std::max(in[1].f[0], in[2].f[0]);
                return std::vector<Value>{
                    Value{{std::min(std::max(in[0].f[0], lower), upper), 0, 0}}};
              });
  }
}

TEST(LN_NodeValues, InvertNegatesAndValueSwitchSelects)
{
  CheckNode("LogicNativeInvertValue",
            0,
            0,
            {{"Value", Kind::Float}},
            {{"Value", Kind::Float}},
            Rows1(),
            [](const std::vector<Value> &in) {
              return std::vector<Value>{Value{{-in[0].f[0], 0, 0}}};
            });
  CheckNode("LogicNativeValueSwitch",
            0,
            0,
            {{"Condition", Kind::Bool}, {"True", Kind::Float}, {"False", Kind::Float}},
            {{"Value", Kind::Float}},
            RowsOf(3, {0.0f, 1.0f, -2.5f}),
            [](const std::vector<Value> &in) {
              return std::vector<Value>{Value{{in[0].f[0] != 0.0f ? in[1].f[0] : in[2].f[0], 0, 0}}};
            });
}

/* ---- Threshold (Greater / Less; Else 0 picks 0 or the threshold when the value fails), Ranged
 * Threshold (Inside / Outside, 0 when it fails), Within Range (the same tests as a bool). Since
 * 2026-11-08 Min and Max are sorted (lo = min(Min, Max), hi = max(Min, Max)) and both ends are
 * inside; Outside is the rest (strict and unsorted before: an end was neither, Max < Min never
 * inside). */
TEST(LN_NodeValues, ThresholdRangedThresholdAndWithinRangeFollowTheirTests)
{
  for (const int16_t op : {0, 1}) {
    SCOPED_TRACE(op == 0 ? "Threshold Greater" : "Threshold Less");
    CheckNode("LogicNativeThreshold",
              op,
              0,
              {{"Else 0", Kind::Bool}, {"Value", Kind::Float}, {"Threshold", Kind::Float}},
              {{"Value", Kind::Float}},
              RowsOf(3, {0.0f, 1.0f, -2.5f, kNaN}),
              [&](const std::vector<Value> &in) {
                const bool else_zero = in[0].f[0] != 0.0f;
                const float value = in[1].f[0], threshold = in[2].f[0];
                const bool passes = op == 0 ? value > threshold : value < threshold;
                return std::vector<Value>{
                    Value{{passes ? value : (else_zero ? 0.0f : threshold), 0, 0}}};
              });
  }
  for (const int16_t op : {0, 1}) {
    SCOPED_TRACE(op == 0 ? "Inside" : "Outside");
    const auto inside_test = [&](float value, float min_value, float max_value) {
      const float lo = std::min(min_value, max_value), hi = std::max(min_value, max_value);
      return op == 0 ? (lo <= value && value <= hi) : (value < lo || value > hi);
    };
    CheckNode("LogicNativeRangedThreshold",
              op,
              0,
              {{"Value", Kind::Float}, {"Min", Kind::Float}, {"Max", Kind::Float}},
              {{"Value", Kind::Float}},
              RowsOf(3, {0.0f, 1.0f, -2.5f, 10.0f, kNaN}),
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{
                    Value{{inside_test(in[0].f[0], in[1].f[0], in[2].f[0]) ? in[0].f[0] : 0.0f, 0, 0}}};
              });
    CheckNode("LogicNativeWithinRange",
              op,
              0,
              {{"Value", Kind::Float}, {"Min", Kind::Float}, {"Max", Kind::Float}},
              {{"Result", Kind::Bool}},
              RowsOf(3, {0.0f, 1.0f, -2.5f, 10.0f, kNaN}),
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{
                    Value{{inside_test(in[0].f[0], in[1].f[0], in[2].f[0]) ? 1.0f : 0.0f, 0, 0}}};
              });
  }
}


/* ---------------------------------------------------------------------------------------------
 * State nodes, over several steps.
 * ------------------------------------------------------------------------------------------- */

/** Compiles `b` or fails the test. */
std::shared_ptr<LN_Program> CompileOrFail(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/* Value Changed: the first value it sees is the baseline (no pulse); a later different value pulses
 * once, Old is the value before, New the current. */
TEST(LN_NodeValues, ValueChangedPulsesWhenTheValueDiffersFromTheLastOneItSaw)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &changed = b.Add("LogicNativeValueChanged");
  b.Link(v, "Value", changed, "Value");
  GenNode &mark = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(mark, "Property", "mark");
  b.Float(mark, "Value", 1.0f);
  b.Link(changed, "If Changed", mark, "Flow");
  GenNode &old_value = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(old_value, "Property", "old");
  b.Link(event, "Out", old_value, "Flow");
  b.Link(changed, "Old", old_value, "Value");
  GenNode &new_value = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(new_value, "Property", "new");
  b.Link(old_value, "Done", new_value, "Flow");
  b.Link(changed, "New", new_value, "Value");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  const struct {
    float v, mark, old_value, new_value;
  } steps[] = {{1, 0, 1, 1}, {1, 0, 1, 1}, {2, 1, 1, 2}, {2, 0, 2, 2}, {3, 1, 2, 3}};
  for (const auto &step : steps) {
    rig.SetFloat(rig.Object(0), "v", step.v);
    rig.SetFloat(rig.Object(0), "mark", 0.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "mark"), step.mark) << "v = " << step.v;
    EXPECT_EQ(rig.Float(rig.Object(0), "old"), step.old_value) << "v = " << step.v;
    EXPECT_EQ(rig.Float(rig.Object(0), "new"), step.new_value) << "v = " << step.v;
  }
}

/* Boolean Edge: Rising on false -> true, Falling on true -> false, nothing on the first sample. */
TEST(LN_NodeValues, BooleanEdgePulsesOnTheChangeOfAConditionAndNotOnTheFirstSample)
{
  TreeBuilder b;
  GenNode &c = b.Add("LogicNativeGetGamePropertyBool");
  b.String(c, "Property", "c");
  GenNode &edge = b.Add("LogicNativeBooleanEdge");
  b.Link(c, "Value", edge, "Condition");
  GenNode &rising = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(rising, "Property", "rise");
  b.Float(rising, "Value", 1.0f);
  b.Link(edge, "Rising", rising, "Flow");
  GenNode &falling = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(falling, "Property", "fall");
  b.Float(falling, "Value", 1.0f);
  b.Link(edge, "Falling", falling, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  const struct {
    bool c;
    float rise, fall;
  } steps[] = {{false, 0, 0}, {true, 1, 0}, {true, 0, 0}, {false, 0, 1}, {true, 1, 0}, {true, 0, 0}};
  for (const auto &step : steps) {
    rig.SetBool(rig.Object(0), "c", step.c);
    rig.SetFloat(rig.Object(0), "rise", 0.0f);
    rig.SetFloat(rig.Object(0), "fall", 0.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "rise"), step.rise);
    EXPECT_EQ(rig.Float(rig.Object(0), "fall"), step.fall);
  }
}

/* Cooldown (0.05 s = 3 steps): accepts the first Flow, blocks while it runs (Remaining counts
 * down), completes and accepts the next Flow in the same step. */
TEST(LN_NodeValues, CooldownAcceptsBlocksAndCompletesInThreeStepCycles)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &cool = b.Add("LogicNativeCooldown");
  b.Float(cool, "Duration", 0.05f);
  b.Link(event, "Out", cool, "Flow");
  const auto mark = [&](const char *out_pin, const char *name) {
    GenNode &m = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(m, "Property", name);
    b.Float(m, "Value", 1.0f);
    b.Link(cool, out_pin, m, "Flow");
  };
  mark("Accepted", "acc");
  mark("Blocked", "blk");
  mark("Completed", "cmp");
  GenNode &remaining = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(remaining, "Property", "rem");
  b.Link(event, "Out", remaining, "Flow");
  b.Link(cool, "Remaining", remaining, "Value");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  const struct {
    float acc, blk, cmp, rem;
  } steps[] = {{1, 0, 0, 0.05f},
               {0, 1, 0, 0.05f - 1.0f / 60.0f},
               {0, 1, 0, 0.05f - 2.0f / 60.0f},
               {1, 0, 1, 0.05f},
               {0, 1, 0, 0.05f - 1.0f / 60.0f},
               {0, 1, 0, 0.05f - 2.0f / 60.0f},
               {1, 0, 1, 0.05f}};
  int index = 0;
  for (const auto &step : steps) {
    for (const char *name : {"acc", "blk", "cmp"}) {
      rig.SetFloat(rig.Object(0), name, 0.0f);
    }
    rig.Step();
    SCOPED_TRACE("step " + std::to_string(index++));
    EXPECT_EQ(rig.Float(rig.Object(0), "acc"), step.acc);
    EXPECT_EQ(rig.Float(rig.Object(0), "blk"), step.blk);
    EXPECT_EQ(rig.Float(rig.Object(0), "cmp"), step.cmp);
    EXPECT_NEAR(rig.Float(rig.Object(0), "rem"), step.rem, 1.0e-6f);
  }
}

/* Flip Flop sends its Flow to A, then B, then A ...; Is A says which it was. Toggle Property flips a
 * bool game property each time its Flow arrives. */
TEST(LN_NodeValues, FlipFlopAlternatesAndTogglePropertyFlips)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &flip = b.Add("LogicNativeFlipFlop");
  b.Link(event, "Out", flip, "Flow");
  GenNode &a = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(a, "Property", "a");
  b.Float(a, "Value", 1.0f);
  b.Link(flip, "A", a, "Flow");
  GenNode &bb = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(bb, "Property", "b");
  b.Float(bb, "Value", 1.0f);
  b.Link(flip, "B", bb, "Flow");
  GenNode &is_a = b.Add("LogicNativeSetGamePropertyBool");
  b.String(is_a, "Property", "isa");
  b.Link(event, "Out", is_a, "Flow");
  b.Link(flip, "Is A", is_a, "Value");
  GenNode &toggle = b.Add("LogicNativeToggleProperty");
  b.String(toggle, "Property", "t");
  b.Link(event, "Out", toggle, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  rig.SetBool(rig.Object(0), "t", false);
  for (int step = 0; step < 4; step++) {
    SCOPED_TRACE("step " + std::to_string(step));
    rig.SetFloat(rig.Object(0), "a", 0.0f);
    rig.SetFloat(rig.Object(0), "b", 0.0f);
    rig.Step();
    const bool first = step % 2 == 0;
    EXPECT_EQ(rig.Float(rig.Object(0), "a"), first ? 1.0f : 0.0f);
    EXPECT_EQ(rig.Float(rig.Object(0), "b"), first ? 0.0f : 1.0f);
    EXPECT_EQ(rig.Text(rig.Object(0), "isa"), first ? "TRUE" : "FALSE");
    EXPECT_EQ(rig.Text(rig.Object(0), "t"), first ? "TRUE" : "FALSE");
  }
}

/* Set Game Property Int makes an int property, writes into an existing float property as a float
 * (the property keeps its type); Get Game Property String gives a string property's text, "" for a
 * missing property and a number's text as every value's text is made ("2.5": as it reads another
 * object's; the owner's gave EXP's own "2.500000" until 2026-11-08). */
TEST(LN_NodeValues, SetGamePropertyIntAndGetGamePropertyStringKeepTheirTypes)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set_new = b.Add("LogicNativeSetGamePropertyInt");
  b.String(set_new, "Property", "ni");
  b.Int(set_new, "Value", 42);
  b.Link(event, "Out", set_new, "Flow");
  GenNode &set_old = b.Add("LogicNativeSetGamePropertyInt");
  b.String(set_old, "Property", "existing_float");
  b.Int(set_old, "Value", 7);
  b.Link(set_new, "Done", set_old, "Flow");
  GenNode *flow = &set_old;
  const struct {
    const char *read;
    const char *write;
  } copies[] = {{"name", "copy"}, {"missing", "copy_missing"}, {"number", "copy_number"}};
  for (const auto &entry : copies) {
    GenNode &read = b.Add("LogicNativeGetGamePropertyString");
    b.String(read, "Property", entry.read);
    GenNode &write = b.Add("LogicNativeSetGamePropertyString");
    b.String(write, "Property", entry.write);
    b.Link(*flow, "Done", write, "Flow");
    b.Link(read, "Value", write, "Value");
    flow = &write;
  }
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &owner = rig.Object(0);
  rig.SetFloat(owner, "existing_float", 1.5f);
  rig.SetString(owner, "name", "hello");
  rig.SetFloat(owner, "number", 2.5f);
  rig.Step();
  EXPECT_EQ(rig.Text(owner, "ni"), "42");
  EXPECT_EQ(owner.GetProperty("ni")->GetValueType(), VALUE_INT_TYPE);
  EXPECT_EQ(rig.Text(owner, "existing_float"), "7.000000");
  EXPECT_EQ(owner.GetProperty("existing_float")->GetValueType(), VALUE_FLOAT_TYPE);
  EXPECT_EQ(rig.Text(owner, "copy"), "hello");
  EXPECT_EQ(rig.Text(owner, "copy_missing"), "");
  EXPECT_EQ(rig.Text(owner, "copy_number"), "2.5");
}

/* ---------------------------------------------------------------------------------------------
 * Get Distance in every end mode, Apply Rotation local and world, Random.
 * ------------------------------------------------------------------------------------------- */

/* Get Distance: each end is an object (empty: the owner) or a position; Distance, Squared Distance
 * and Valid. A named object that does not exist makes the result invalid (0). */
TEST(LN_NodeValues, GetDistanceMeasuresBetweenObjectsAndPositionsInEveryEndMode)
{
  struct Case {
    int16_t mode_a, mode_b;
    bool target_named;
    float a[3], b[3];
    float distance;
    bool valid;
  };
  /* The owner stands at the origin, Target at (3, 4, 0). */
  const Case cases[] = {
      {0, 0, true, {0, 0, 0}, {0, 0, 0}, 5.0f, true},     /* owner to Target */
      {0, 1, false, {0, 0, 0}, {0, 0, 12}, 12.0f, true},  /* owner to a position */
      {1, 0, true, {6, 8, 0}, {0, 0, 0}, 5.0f, true},     /* a position to Target */
      {1, 1, false, {1, 1, 1}, {4, 5, 1}, 5.0f, true},    /* two positions */
      {0, 0, false, {0, 0, 0}, {0, 0, 0}, 0.0f, false},   /* End B is an empty object: invalid */
  };
  int number = 0;
  for (const Case &c : cases) {
    SCOPED_TRACE("case " + std::to_string(number++));
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      TreeBuilder b;
      GenNode &event = b.Add("LogicNativeOnFixedUpdate");
      GenNode &distance = b.Add("LogicNativeGetDistance", c.mode_a, c.mode_b);
      b.Vector(distance, "Position A", c.a[0], c.a[1], c.a[2]);
      b.Vector(distance, "Position B", c.b[0], c.b[1], c.b[2]);
      if (c.target_named) {
        b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
      }
      GenNode &d = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(d, "Property", "d");
      b.Link(event, "Out", d, "Flow");
      b.Link(distance, "Distance", d, "Value");
      GenNode &q = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(q, "Property", "q");
      b.Link(d, "Done", q, "Flow");
      b.Link(distance, "Squared Distance", q, "Value");
      GenNode &v = b.Add("LogicNativeSetGamePropertyBool");
      b.String(v, "Property", "v");
      b.Link(q, "Done", v, "Flow");
      b.Link(distance, "Valid", v, "Value");
      const std::shared_ptr<LN_Program> program = CompileOrFail(b);
      ASSERT_NE(program, nullptr);
      SemRig rig(program, mode);
      rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
      rig.Step();
      EXPECT_NEAR(rig.Float(rig.Object(0), "d"), c.distance, 1.0e-5f);
      EXPECT_NEAR(rig.Float(rig.Object(0), "q"), c.distance * c.distance, 1.0e-4f);
      EXPECT_EQ(rig.Text(rig.Object(0), "v"), c.valid ? "TRUE" : "FALSE");
    }
  }
}

/* Apply Rotation: Local turns about the object's own axes (new = old * turn), world about the
 * world axes (new = turn * old). */
TEST(LN_NodeValues, ApplyRotationLocalTurnsAboutTheObjectsAxesAndWorldAboutTheWorldAxes)
{
  for (const bool local : {true, false}) {
    SCOPED_TRACE(local ? "local" : "world");
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.Rotation(turn, "Rotation", 1.5707964f, 0.0f, 0.0f);
    b.Bool(turn, "Local", local);
    b.Link(event, "Out", turn, "Flow");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kMemoOff);
    const MT_Matrix3x3 start(MT_Vector3(0.0f, 0.0f, 1.5707964f));
    rig.Object(0).NodeSetLocalOrientation(start);
    rig.Object(0).NodeUpdateGS(0.0);
    rig.Step();
    const MT_Matrix3x3 x_turn(MT_Vector3(1.5707964f, 0.0f, 0.0f));
    const MT_Matrix3x3 expected = local ? start * x_turn : x_turn * start;
    const MT_Matrix3x3 got = rig.Object(0).NodeGetLocalOrientation();
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        EXPECT_NEAR(got[row][column], expected[row][column], 1.0e-5f) << row << "," << column;
      }
    }
  }
}

/* Random Value's Float / Integer / Vector (the Random Float / Int / Vector nodes until 2026-11-08):
 * a number in the range for the tick, a new one every tick; the range is the two ends in either
 * order; Integer includes both ends; Vector is zero on an axis whose Min and Max are zero. */
TEST(LN_NodeValues, RandomNodesStayInTheirRangeAndChangeEveryTick)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &rf = b.Add("LogicNativeRandomValue", 0, 0);
  b.Float(rf, "Min", 5.0f);
  b.Float(rf, "Max", -3.0f); /* reversed on purpose */
  GenNode &ri = b.Add("LogicNativeRandomValue", 1, 0);
  b.Int(ri, "Min_Int", 2);
  b.Int(ri, "Max_Int", 4);
  GenNode &rv = b.Add("LogicNativeRandomValue", 2, 0);
  b.Vector(rv, "Min_Vector", 0.0f, 0.0f, 0.0f);
  b.Vector(rv, "Max_Vector", 1.0f, 0.0f, 1.0f);
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(rv, "Value_Vector", split, "Vector");
  GenNode *flow = &event;
  const char *flow_pin = "Out";
  const auto store = [&](const char *kind, const char *name, GenNode &source, const char *pin) {
    GenNode &writer = b.Add(kind);
    b.String(writer, "Property", name);
    b.Link(*flow, flow_pin, writer, "Flow");
    b.Link(source, pin, writer, "Value");
    flow = &writer;
    flow_pin = "Done";
  };
  store("LogicNativeSetGamePropertyFloat", "rf", rf, "Value");
  store("LogicNativeSetGamePropertyInt", "ri", ri, "Value_Int");
  store("LogicNativeSetGamePropertyFloat", "rx", split, "X");
  store("LogicNativeSetGamePropertyFloat", "ry", split, "Y");
  store("LogicNativeSetGamePropertyFloat", "rz", split, "Z");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    std::set<std::string> floats;
    std::set<int> ints;
    for (int step = 0; step < 40; step++) {
      rig.Step();
      const float f = rig.Float(rig.Object(0), "rf");
      EXPECT_GE(f, -3.0f);
      EXPECT_LE(f, 5.0f);
      floats.insert(Bits(f));
      const int i = int(rig.Float(rig.Object(0), "ri"));
      EXPECT_GE(i, 2);
      EXPECT_LE(i, 4);
      ints.insert(i);
      EXPECT_GE(rig.Float(rig.Object(0), "rx"), 0.0f);
      EXPECT_LT(rig.Float(rig.Object(0), "rx"), 1.0f);
      EXPECT_EQ(rig.Float(rig.Object(0), "ry"), 0.0f);
      EXPECT_LT(rig.Float(rig.Object(0), "rz"), 1.0f);
    }
    EXPECT_GT(floats.size(), 30u) << "a new number almost every tick";
    EXPECT_EQ(ints.size(), 3u) << "2, 3 and 4 all come up in 40 ticks";
  }
}


/* The defaults of a kind: a Get Game Property of a missing property gives the kind's default (0, 0,
 * FALSE); one holding text gives the text's number when it is one ("5": 5 as a Float and an Int)
 * and the default when not ("abc": 0), empty text is FALSE (2026-11-08, as a read of another
 * object converts; the owner's read gave EXP's -1 for any text and TRUE for empty text); Math
 * with unlinked inputs computes with the pin defaults (0 and 0); Clamp's Max defaults to 1. */
TEST(LN_NodeValues, KindsGiveTheirDefaultsForMissingPropertiesAndUnlinkedInputs)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &event;
  const char *flow_pin = "Out";
  const auto store = [&](const char *read_kind, const char *write_kind, const char *property, const char *out) {
    GenNode &read = b.Add(read_kind);
    b.String(read, "Property", property);
    GenNode &write = b.Add(write_kind);
    b.String(write, "Property", out);
    b.Link(*flow, flow_pin, write, "Flow");
    b.Link(read, "Value", write, "Value");
    flow = &write;
    flow_pin = "Done";
  };
  store("LogicNativeGetGamePropertyFloat", "LogicNativeSetGamePropertyFloat", "missing", "f_missing");
  store("LogicNativeGetGamePropertyInt", "LogicNativeSetGamePropertyInt", "missing", "i_missing");
  store("LogicNativeGetGamePropertyBool", "LogicNativeSetGamePropertyBool", "missing", "b_missing");
  store("LogicNativeGetGamePropertyFloat", "LogicNativeSetGamePropertyFloat", "text", "f_of_text");
  store("LogicNativeGetGamePropertyFloat", "LogicNativeSetGamePropertyFloat", "text5", "f_of_5");
  store("LogicNativeGetGamePropertyInt", "LogicNativeSetGamePropertyInt", "text5", "i_of_5");
  store("LogicNativeGetGamePropertyBool", "LogicNativeSetGamePropertyBool", "empty", "b_of_empty");
  store("LogicNativeGetGamePropertyInt", "LogicNativeSetGamePropertyInt", "number", "i_of_number");
  GenNode &add = b.Add("LogicNativeMath", blender::NODE_MATH_ADD);
  GenNode &math_out = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(math_out, "Property", "math_default");
  b.Link(*flow, flow_pin, math_out, "Flow");
  b.Link(add, "Result", math_out, "Value");
  GenNode &clamp = b.Add("LogicNativeClampValue");
  b.Float(clamp, "Value", 5.0f);
  GenNode &clamp_out = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(clamp_out, "Property", "clamp_default");
  b.Link(math_out, "Done", clamp_out, "Flow");
  b.Link(clamp, "Value", clamp_out, "Value");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    rig.SetString(rig.Object(0), "text", "abc");
    rig.SetString(rig.Object(0), "text5", "5");
    rig.SetString(rig.Object(0), "empty", "");
    rig.SetFloat(rig.Object(0), "number", 2.75f);
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    EXPECT_EQ(rig.Float(owner, "f_missing"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "i_missing"), 0.0f);
    EXPECT_EQ(rig.Text(owner, "b_missing"), "FALSE");
    /* Text that is no number: the kind's default, as for another object. */
    EXPECT_EQ(rig.Float(owner, "f_of_text"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "f_of_5"), 5.0f);
    EXPECT_EQ(rig.Text(owner, "i_of_5"), "5");
    EXPECT_EQ(rig.Text(owner, "b_of_empty"), "FALSE");
    EXPECT_EQ(rig.Text(owner, "i_of_number"), "2"); /* (truncated) */
    EXPECT_EQ(rig.Float(owner, "math_default"), 0.0f);
    EXPECT_EQ(rig.Float(owner, "clamp_default"), 1.0f);
  }
}


/* Value Changed To pulses when the value becomes the target (not while it stays there). */
TEST(LN_NodeValues, ValueChangedToPulsesWhenTheValueBecomesTheTarget)
{
  TreeBuilder b;
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &changed = b.Add("LogicNativeValueChangedTo");
  b.Link(v, "Value", changed, "Value");
  GenNode &target = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(target, "Property", "target");
  b.Link(target, "Value", changed, "Target");
  GenNode &mark = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(mark, "Property", "mark");
  b.Float(mark, "Value", 1.0f);
  b.Link(changed, "Result", mark, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kMemoOff);
  rig.SetFloat(rig.Object(0), "target", 2.0f);
  std::string seen;
  for (const float value : {1.0f, 2.0f, 2.0f, 3.0f, 2.0f, 2.0f}) {
    rig.SetFloat(rig.Object(0), "v", value);
    rig.SetFloat(rig.Object(0), "mark", 0.0f);
    rig.Step();
    seen += rig.Float(rig.Object(0), "mark") > 0.5f ? '1' : '0';
  }
  EXPECT_EQ(seen, "010010");
}

}  // namespace
