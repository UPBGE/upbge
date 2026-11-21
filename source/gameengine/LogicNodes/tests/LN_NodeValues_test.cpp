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
#include <tuple>
#include <map>

#include "LN_ActionFrames.h"
#include "LN_AimMath.h"
#include "LN_Columns.h"
#include "LN_FindObjects.h"
#include "LN_StrictDump.h"
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

/* ---- Map Range's Interpolation (custom2, 2026-11-16): Smooth Step and Smoother Step ease the
 * factor clamped to 0..1 (Blender's formulas), then map it as Linear does; with or without
 * Clamp. ---- */
TEST(LN_NodeValues, MapRangeSmoothStepsEaseTheClampedFactor)
{
  for (const int16_t interpolation : {1, 2}) {
    for (const int16_t clamp : {0, 1}) {
      SCOPED_TRACE(std::string(interpolation == 1 ? "smooth step" : "smoother step") +
                   (clamp ? ", clamped" : ""));
      CheckNode("LogicNativeMapRange",
                clamp,
                interpolation,
                {{"Value", Kind::Float},
                 {"From Min", Kind::Float},
                 {"From Max", Kind::Float},
                 {"To Min", Kind::Float},
                 {"To Max", Kind::Float}},
                {{"Result", Kind::Float}},
                RowsOf(5, {0.0f, 1.0f, -2.5f, 0.3f, 10.0f, kNaN}),
                [&](const std::vector<Value> &in) {
                  const float value = in[0].f[0], from_min = in[1].f[0], from_max = in[2].f[0];
                  const float to_min = in[3].f[0], to_max = in[4].f[0];
                  const float range = from_max - from_min;
                  const float normalized = range == 0.0f ? 0.0f : (value - from_min) / range;
                  const float t = normalized <= 0.0f ? 0.0f : (normalized >= 1.0f ? 1.0f : normalized);
                  const float eased = interpolation == 1 ?
                                          (3.0f - 2.0f * t) * t * t :
                                          t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                  float mapped = to_min + eased * (to_max - to_min);
                  if (clamp != 0) {
                    mapped = std::min(std::max(mapped, std::min(to_min, to_max)),
                                      std::max(to_min, to_max));
                  }
                  return std::vector<Value>{Value{{mapped, 0, 0}}};
                });
    }
  }
}

/* A Map Range of constants is also worked out while compiling (EvaluateOutputValue), where a
 * node folds its constant inputs first (Compare): with its Interpolation, as the runtime's. Each
 * Compare asks whether the folded value is the eased one. */
TEST(LN_NodeValues, AConstantMapRangeFoldsWithItsInterpolation)
{
  const struct {
    int16_t interpolation;
    float expected;
  } cases[] = {{0, 2.5f}, {1, 10.0f * ((3.0f - 2.0f * 0.25f) * 0.25f * 0.25f)},
               {2, 10.0f * (0.25f * 0.25f * 0.25f * (0.25f * (0.25f * 6.0f - 15.0f) + 10.0f))}};
  for (const auto &entry : cases) {
    SCOPED_TRACE("interpolation " + std::to_string(entry.interpolation));
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &map = b.Add("LogicNativeMapRange", 0, entry.interpolation);
    b.Float(map, "Value", 0.25f);
    b.Float(map, "From Min", 0.0f);
    b.Float(map, "From Max", 1.0f);
    b.Float(map, "To Min", 0.0f);
    b.Float(map, "To Max", 10.0f);
    GenNode &compare = b.Add("LogicNativeCompare", 0 /* Equal */);
    b.Link(map, "Result", compare, "A");
    b.Float(compare, "B", entry.expected);
    GenNode &write = b.Add("LogicNativeSetGamePropertyBool");
    b.String(write, "Property", "eased");
    b.Link(compare, "Result", write, "Value");
    b.Link(step, "Out", write, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    SemRig rig(program, kMemoOff);
    rig.SetBool(rig.Object(0), "eased", false);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "eased"), 1.0f);
  }
}

/* ---- Math's newer operations (2026-11-16), by their definitions (Blender's where it has the
 * operation): Square Root of a negative is 0, Arcsine and Arccosine clamp A to -1..1, Wrap puts
 * A in [C, B) (C when B equals C), Snap and Ping-Pong give 0 for B 0, Angle Difference is B - A
 * wrapped into [-pi, pi). ---- */
TEST(LN_NodeValues, MathNewerOperationsGiveTheirDefinitions)
{
  constexpr float pi = 3.14159265358979323846f;
  const auto wrap = [](const float value, const float max, const float min) {
    const float range = max - min;
    return range != 0.0f ? value - range * std::floor((value - min) / range) : min;
  };
  using Op = std::function<float(float, float, float)>;
  const struct {
    int16_t op;
    Op model;
  } ops[] = {
      {blender::NODE_MATH_SQRT, [](float a, float, float) { return a > 0.0f ? std::sqrt(a) : 0.0f; }},
      {blender::NODE_MATH_TANGENT, [](float a, float, float) { return std::tan(a); }},
      {blender::NODE_MATH_ARCSINE,
       [pi](float a, float, float) {
         return a <= -1.0f ? -pi * 0.5f : (a >= 1.0f ? pi * 0.5f : std::asin(a));
       }},
      {blender::NODE_MATH_ARCCOSINE,
       [pi](float a, float, float) { return a <= -1.0f ? pi : (a >= 1.0f ? 0.0f : std::acos(a)); }},
      {blender::NODE_MATH_ARCTANGENT, [](float a, float, float) { return std::atan(a); }},
      {blender::NODE_MATH_ARCTAN2, [](float a, float b, float) { return std::atan2(a, b); }},
      {blender::NODE_MATH_WRAP, [wrap](float a, float b, float c) { return wrap(a, b, c); }},
      {blender::NODE_MATH_SNAP,
       [](float a, float b, float) { return b != 0.0f ? std::floor(a / b) * b : 0.0f; }},
      {blender::NODE_MATH_PINGPONG,
       [](float a, float b, float) {
         if (b == 0.0f) {
           return 0.0f;
         }
         const float t = (a - b) / (b * 2.0f);
         return std::fabs((t - std::floor(t)) * b * 2.0f - b);
       }},
      {int16_t(100), [wrap, pi](float a, float b, float) { return wrap(b - a, pi, -pi); }},
  };
  const std::vector<float> values = {0.0f, 1.0f, -1.0f, 2.5f, -7.25f, 0.5f, 6.5f, 1.0e30f, kInf, kNaN};
  for (const auto &entry : ops) {
    SCOPED_TRACE("Math operation " + std::to_string(entry.op));
    CheckNode("LogicNativeMath",
              entry.op,
              0,
              {{"A", Kind::Float}, {"B", Kind::Float}, {"C", Kind::Float}},
              {{"Result", Kind::Float}},
              RowsOf(3, values),
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{
                    Value{{entry.model(in[0].f[0], in[1].f[0], in[2].f[0]), 0, 0}}};
              });
  }
}

/* ---- Vector Math's newer operations (2026-11-16), by their definitions: the number ones at
 * Value (Dot, Distance, Length, Angle = atan2(|A x B|, A . B), Signed Angle negative when A x B
 * points against the Axis), the vector ones at Result (Cross; Project is zero onto a zero B;
 * Reflect around B made unit, A itself for a zero B; Limit Length to max(Scale, 0); Move
 * Towards gives B once nearer than Scale). ---- */
TEST(LN_NodeValues, VectorMathNewerOperationsGiveTheirDefinitions)
{
  const auto dot = [](const Value &a, const Value &b) {
    return a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2];
  };
  const auto cross = [](const Value &a, const Value &b) {
    return Vec(a.f[1] * b.f[2] - a.f[2] * b.f[1],
               a.f[2] * b.f[0] - a.f[0] * b.f[2],
               a.f[0] * b.f[1] - a.f[1] * b.f[0]);
  };
  const auto norm = [&](const Value &a) { return std::sqrt(dot(a, a)); };
  using Measure = std::function<float(const Value &, const Value &, const Value &)>;
  const struct {
    int16_t op;
    Measure model;
  } measures[] = {
      {blender::NODE_VECTOR_MATH_DOT_PRODUCT,
       [&](const Value &a, const Value &b, const Value &) { return dot(a, b); }},
      {blender::NODE_VECTOR_MATH_DISTANCE,
       [&](const Value &a, const Value &b, const Value &) {
         return norm(Vec(a.f[0] - b.f[0], a.f[1] - b.f[1], a.f[2] - b.f[2]));
       }},
      {blender::NODE_VECTOR_MATH_LENGTH,
       [](const Value &a, const Value &, const Value &) {
         return MT_Vector3(a.f[0], a.f[1], a.f[2]).length();
       }},
      {int16_t(100),
       [&](const Value &a, const Value &b, const Value &) {
         return std::atan2(norm(cross(a, b)), dot(a, b));
       }},
      {int16_t(101),
       [&](const Value &a, const Value &b, const Value &axis) {
         const Value c = cross(a, b);
         const float angle = std::atan2(norm(c), dot(a, b));
         return dot(c, axis) < 0.0f ? -angle : angle;
       }},
  };
  std::vector<std::vector<Value>> rows;
  const std::vector<Value> axes = {Vec(0, 0, 1), Vec(0, 0, -1), Vec(1, 0, 0)};
  for (const Value &a : VectorSamples()) {
    for (const Value &b : VectorSamples()) {
      for (const Value &axis : axes) {
        rows.push_back({a, b, axis});
      }
    }
  }
  for (const auto &entry : measures) {
    SCOPED_TRACE("Vector Math operation " + std::to_string(entry.op));
    CheckNode("LogicNativeVectorMath",
              entry.op,
              0,
              {{"A", Kind::Vector}, {"B", Kind::Vector}, {"Axis", Kind::Vector}},
              {{"Value", Kind::Float}},
              rows,
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{Value{{entry.model(in[0], in[1], in[2]), 0, 0}}};
              });
  }

  using Function = std::function<Value(const Value &, const Value &, float)>;
  const struct {
    int16_t op;
    Function model;
  } functions[] = {
      {blender::NODE_VECTOR_MATH_CROSS_PRODUCT,
       [&](const Value &a, const Value &b, float) { return cross(a, b); }},
      {blender::NODE_VECTOR_MATH_PROJECT,
       [&](const Value &a, const Value &b, float) {
         const float length_squared = dot(b, b);
         if (length_squared == 0.0f) {
           return Vec(0, 0, 0);
         }
         const float factor = dot(a, b) / length_squared;
         return Vec(b.f[0] * factor, b.f[1] * factor, b.f[2] * factor);
       }},
      {blender::NODE_VECTOR_MATH_REFLECT,
       [&](const Value &a, const Value &b, float) {
         const float length = norm(b);
         if (length == 0.0f) {
           return a;
         }
         const Value n = Vec(b.f[0] / length, b.f[1] / length, b.f[2] / length);
         const float twice = 2.0f * dot(a, n);
         return Vec(a.f[0] - twice * n.f[0], a.f[1] - twice * n.f[1], a.f[2] - twice * n.f[2]);
       }},
      {int16_t(102),
       [&](const Value &a, const Value &, float scale) {
         const float max_length = scale > 0.0f ? scale : 0.0f;
         const float length = norm(a);
         if (length <= max_length) {
           return a;
         }
         const float factor = max_length / length;
         return Vec(a.f[0] * factor, a.f[1] * factor, a.f[2] * factor);
       }},
      {int16_t(103),
       [&](const Value &a, const Value &b, float scale) {
         const Value d = Vec(b.f[0] - a.f[0], b.f[1] - a.f[1], b.f[2] - a.f[2]);
         const float distance = norm(d);
         if (distance <= scale || distance == 0.0f) {
           return b;
         }
         const float factor = scale / distance;
         return Vec(a.f[0] + d.f[0] * factor, a.f[1] + d.f[1] * factor, a.f[2] + d.f[2] * factor);
       }},
  };
  const std::vector<float> scales = {0.0f, 1.0f, 2.5f, -2.5f, kNaN};
  for (const auto &entry : functions) {
    SCOPED_TRACE("Vector Math operation " + std::to_string(entry.op));
    std::vector<std::vector<Value>> scaled_rows;
    for (const Value &a : VectorSamples()) {
      for (const Value &b : VectorSamples()) {
        for (const float scale : scales) {
          scaled_rows.push_back({a, b, Value{{scale, 0, 0}}});
        }
      }
    }
    CheckNode("LogicNativeVectorMath",
              entry.op,
              0,
              {{"A", Kind::Vector}, {"B", Kind::Vector}, {"Scale", Kind::Float}},
              {{"Result", Kind::Vector}},
              scaled_rows,
              [&](const std::vector<Value> &in) {
                return std::vector<Value>{entry.model(in[0], in[1], in[2].f[0])};
              });
  }
}

/* Two Vector Math operations of one kind on the same live inputs stay two expressions: the
 * expression merge compares the operation (int_value; Vector::VectorFunction, 2026-11-16, was
 * the first mergeable vector kind holding one). */
TEST(LN_NodeValues, VectorMathOperationsOnTheSameInputsStayApart)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &ax = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(ax, "Property", "ax");
  GenNode &bz = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(bz, "Property", "bz");
  GenNode &a = b.Add("LogicNativeCombineXYZ");
  b.Link(ax, "Value", a, "X");
  b.Float(a, "Y", 2.0f);
  b.Float(a, "Z", 3.0f);
  GenNode &bv = b.Add("LogicNativeCombineXYZ");
  b.Float(bv, "X", 0.0f);
  b.Float(bv, "Y", 0.0f);
  b.Link(bz, "Value", bv, "Z");
  const struct {
    int16_t op;
    const char *output, *property;
  } ops[] = {{blender::NODE_VECTOR_MATH_CROSS_PRODUCT, "Result", "cross_x"},
             {blender::NODE_VECTOR_MATH_PROJECT, "Result", "project_x"},
             {blender::NODE_VECTOR_MATH_DOT_PRODUCT, "Value", "dot"},
             {int16_t(100), "Value", "angle"}};
  for (const auto &entry : ops) {
    GenNode &math = b.Add("LogicNativeVectorMath", entry.op);
    b.Link(a, "Vector", math, "A");
    b.Link(bv, "Vector", math, "B");
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", entry.property);
    if (std::string(entry.output) == "Result") {
      GenNode &separate = b.Add("LogicNativeSeparateXYZ");
      b.Link(math, "Result", separate, "Vector");
      b.Link(separate, "X", write, "Value");
    }
    else {
      b.Link(math, "Value", write, "Value");
    }
    b.Link(step, "Out", write, "Flow");
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kMemoOff);
  rig.SetFloat(rig.Object(0), "ax", 1.0f);
  rig.SetFloat(rig.Object(0), "bz", 2.0f);
  rig.Step();
  /* A (1, 2, 3), B (0, 0, 2): A x B = (4, -2, 0); A onto B = (0, 0, 3); A . B = 6. */
  EXPECT_EQ(rig.Float(rig.Object(0), "cross_x"), 4.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "project_x"), 0.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "dot"), 6.0f);
  EXPECT_NEAR(rig.Float(rig.Object(0), "angle"), std::atan2(std::sqrt(20.0f), 6.0f), 1.0e-6f);
}

/* The values each owner of `rig` wrote into `properties` after two steps (the first runs On
 * Init: no tree runs wide in it). */
static std::vector<float> StepValues(SemRig &rig,
                                     const uint32_t owners,
                                     const std::vector<const char *> &properties)
{
  rig.Step();
  rig.Step();
  std::vector<float> values;
  for (uint32_t index = 0; index < owners; index++) {
    for (const char *property : properties) {
      values.push_back(rig.Float(rig.Object(index), property));
    }
  }
  return values;
}

/* ---- Rotation Math (2026-11-16): Combine applies B after A around the world's axes, Combine
 * Local around A's own, Invert undoes A, Difference is the turn from A to B. Checked against
 * matrices (the runtime goes through quaternions: within 1e-5), and a wide run gives the VM's
 * bits. ---- */
TEST(LN_NodeValues, RotationMathCombinesInTheWorldOrInTheOwnFrame)
{
  /* (Turns pointing straight up or down: LN_RotationAngles.) */
  const MT_Vector3 euler_a(0.4f, -0.3f, 1.2f), euler_b(1.1f, 0.25f, 0.3f);
  const MT_Matrix3x3 a(euler_a);
  const MT_Matrix3x3 b(euler_b);
  const MT_Matrix3x3 expected[4] = {b * a, a * b, a.transposed(), b * a.transposed()};
  for (int16_t op = 0; op < 4; op++) {
    SCOPED_TRACE("Rotation Math operation " + std::to_string(op));
    TreeBuilder builder;
    GenNode &step = builder.Add("LogicNativeOnFixedUpdate");
    GenNode &rotation_a = builder.Add("LogicNativeEuler");
    GenNode &rotation_b = builder.Add("LogicNativeEuler");
    for (int axis = 0; axis < 3; axis++) {
      const char *name = axis == 0 ? "X" : (axis == 1 ? "Y" : "Z");
      builder.Float(rotation_a, name, euler_a[axis]);
      builder.Float(rotation_b, name, euler_b[axis]);
    }
    GenNode &math = builder.Add("LogicNativeRotationMath", op);
    builder.Link(rotation_a, "Rotation", math, "A");
    builder.Link(rotation_b, "Rotation", math, "B");
    GenNode &separate = builder.Add("LogicNativeSeparateEuler");
    builder.Link(math, "Rotation", separate, "Rotation");
    for (const char *axis : {"X", "Y", "Z"}) {
      GenNode &write = builder.Add("LogicNativeSetGamePropertyFloat");
      builder.String(write, "Property", (std::string("r") + axis).c_str());
      builder.Link(separate, axis, write, "Value");
      builder.Link(step, "Out", write, "Flow");
    }
    std::string report;
    const std::shared_ptr<LN_Program> program = builder.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::vector<float> vm;
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode, 16);
      const std::vector<float> values = StepValues(rig, 16, {"rX", "rY", "rZ"});
      const MT_Matrix3x3 got(MT_Vector3(values[0], values[1], values[2]));
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          EXPECT_NEAR(got[row][column], expected[op][row][column], 1.0e-5f);
        }
      }
      if (vm.empty()) {
        vm = values;
      }
      EXPECT_EQ(values, vm);
    }
    SemRig wide(program, kProduction, 16);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(StepValues(wide, 16, {"rX", "rY", "rZ"}), vm) << "a wide run's rotation";
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      /* (UPBGE_LN_COLUMN_DIRECT=force runs every tree as a column pass: no lanes.) */
      EXPECT_EQ(wide.LastColumnLanes(), 16u)
          << "the owners ran wide (RRotFn): " << wide.LastColumnRefusal();
    }
  }
}

/* ---- Transform Vector (2026-11-16): an object's own space and the world, for points (moved,
 * turned, scaled) and directions (turned). Sixteen owners in sixteen poses: each against
 * matrices (within 1e-5), and a wide run (the kernels VAttr, RAttr, VRotFn) gives the VM's
 * bits. ---- */
TEST(LN_NodeValues, TransformVectorCarriesPointsAndDirectionsBetweenSpaces)
{
  const MT_Vector3 v(0.5f, -1.0f, 2.0f);
  constexpr uint32_t owners = 16;
  const auto pose = [](const uint32_t index, MT_Vector3 &position, MT_Vector3 &euler,
                       MT_Vector3 &scale) {
    const float f = float(index);
    position = MT_Vector3(1.0f + f, 2.0f - 0.5f * f, 3.0f);
    euler = MT_Vector3(0.3f + 0.1f * f, -0.2f, 1.1f - 0.07f * f);
    scale = MT_Vector3(2.0f, 0.5f + 0.1f * f, 1.5f);
  };
  for (const int16_t space : {0, 1}) {
    for (const int16_t type : {0, 1}) {
      SCOPED_TRACE(std::string(space == 0 ? "local to world" : "world to local") +
                   (type == 0 ? ", point" : ", direction"));
      TreeBuilder builder;
      GenNode &step = builder.Add("LogicNativeOnFixedUpdate");
      GenNode &transform = builder.Add("LogicNativeTransformVector", space, type);
      builder.Vector(transform, "Vector", v.x(), v.y(), v.z());
      GenNode &separate = builder.Add("LogicNativeSeparateXYZ");
      builder.Link(transform, "Vector", separate, "Vector");
      for (const char *axis : {"X", "Y", "Z"}) {
        GenNode &write = builder.Add("LogicNativeSetGamePropertyFloat");
        builder.String(write, "Property", (std::string("t") + axis).c_str());
        builder.Link(separate, axis, write, "Value");
        builder.Link(step, "Out", write, "Flow");
      }
      std::string report;
      const std::shared_ptr<LN_Program> program = builder.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      const auto place = [&](SemRig &rig) {
        for (uint32_t index = 0; index < owners; index++) {
          MT_Vector3 position, euler, scale;
          pose(index, position, euler, scale);
          KX_GameObject &object = rig.Object(index);
          object.NodeSetLocalPosition(position);
          object.NodeSetLocalOrientation(MT_Matrix3x3(euler));
          object.NodeSetLocalScale(scale);
          object.NodeUpdateGS(0.0);
        }
      };
      SemRig rig(program, kProduction, owners);
      place(rig);
      const std::vector<float> vm = StepValues(rig, owners, {"tX", "tY", "tZ"});
      for (uint32_t index = 0; index < owners; index++) {
        MT_Vector3 position, euler, scale;
        pose(index, position, euler, scale);
        const MT_Matrix3x3 r(euler);
        MT_Vector3 expected;
        if (space == 0) {
          expected = type == 0 ? position + r * MT_Vector3(v.x() * scale.x(),
                                                           v.y() * scale.y(),
                                                           v.z() * scale.z()) :
                                 r * v;
        }
        else {
          const MT_Vector3 local = r.transposed() * (type == 0 ? v - position : v);
          expected = type == 0 ? MT_Vector3(local.x() / scale.x(),
                                            local.y() / scale.y(),
                                            local.z() / scale.z()) :
                                 local;
        }
        for (int axis = 0; axis < 3; axis++) {
          EXPECT_NEAR(vm[index * 3 + axis], expected[axis], 1.0e-5f) << "owner " << index;
        }
      }
      SemRig wide(program, kProduction, owners);
      place(wide);
      wide.SetColumns(true, 2, 1);
      EXPECT_EQ(StepValues(wide, owners, {"tX", "tY", "tZ"}), vm) << "a wide run's transform";
      if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
        EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
            << "the owners ran wide (VAttr, RAttr, VRotFn): " << wide.LastColumnRefusal();
      }
    }
  }
}

/* ---- Compare's Vector type (custom2 1, 2026-11-16): Equal when at most Tolerance apart, Not
 * Equal farther; Greater / Less compare the lengths. ---- */
TEST(LN_NodeValues, CompareVectorsByDistanceAndLength)
{
  const auto length = [](const Value &v) { return MT_Vector3(v.f[0], v.f[1], v.f[2]).length(); };
  const auto distance = [](const Value &a, const Value &b) {
    const float x = a.f[0] - b.f[0], y = a.f[1] - b.f[1], z = a.f[2] - b.f[2];
    return std::sqrt(x * x + y * y + z * z);
  };
  std::vector<std::vector<Value>> rows;
  for (const Value &a : VectorSamples()) {
    for (const Value &b : VectorSamples()) {
      for (const float tolerance : {0.0001f, 0.0f, 3.0f, kNaN}) {
        rows.push_back({a, b, Value{{tolerance, 0, 0}}});
      }
    }
  }
  for (int16_t op = 0; op < 6; op++) {
    SCOPED_TRACE("Compare vectors, operation " + std::to_string(op));
    CheckNode("LogicNativeCompare",
              op,
              1,
              {{"Vector A", Kind::Vector}, {"Vector B", Kind::Vector}, {"Tolerance", Kind::Float}},
              {{"Result", Kind::Bool}},
              rows,
              [&](const std::vector<Value> &in) {
                bool result = false;
                const float d = distance(in[0], in[1]), tolerance = in[2].f[0];
                const float la = length(in[0]), lb = length(in[1]);
                switch (op) {
                  case 0:
                    result = d <= tolerance;
                    break;
                  case 1:
                    result = d > tolerance;
                    break;
                  case 2:
                    result = la > lb;
                    break;
                  case 3:
                    result = la < lb;
                    break;
                  case 4:
                    result = la >= lb;
                    break;
                  case 5:
                    result = la <= lb;
                    break;
                }
                return std::vector<Value>{Value{{result ? 1.0f : 0.0f, 0, 0}}};
              });
  }
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

/* Value Changed of ANOTHER object's typed game property (2026-11-19: Get Game Property Int of
 * another object into On Value Change never pulsed, while Float and Any did; the navigation
 * example 15's units never took orders). Every type, the own object's too, in every rig mode. */
TEST(LN_NodeValues, ValueChangedSeesAnotherObjectsTypedPropertyChange)
{
  const char *readers[] = {"LogicNativeGetGamePropertyInt",
                           "LogicNativeGetGamePropertyFloat",
                           "LogicNativeGetGamePropertyBool",
                           "LogicNativeGetGameProperty"};
  for (const char *reader : readers) {
    for (const bool other : {true, false}) {
      SCOPED_TRACE(std::string(reader) + (other ? " of Target" : " of the owner"));
      for (const SemMode &mode : AllModes()) {
        SCOPED_TRACE(mode.name);
        TreeBuilder b;
        GenNode &v = b.Add(reader);
        b.String(v, "Property", "n");
        if (other) {
          b.ObjectNamed(v, "Object", &diff::TargetBlenderObject());
        }
        GenNode &changed = b.Add("LogicNativeValueChanged");
        b.Link(v, "Value", changed, "Value");
        GenNode &mark = b.Add("LogicNativeSetGamePropertyFloat");
        b.String(mark, "Property", "mark");
        b.Float(mark, "Value", 1.0f);
        b.Link(changed, "If Changed", mark, "Flow");
        const std::shared_ptr<LN_Program> program = CompileOrFail(b);
        ASSERT_NE(program, nullptr);
        SemRig rig(program, mode);
        KX_GameObject &holder = other ? rig.AddObject("Target", MT_Vector3()) : rig.Object(0);
        const bool boolean = std::string(reader) == "LogicNativeGetGamePropertyBool";
        const struct {
          int n;
          float mark;
        } steps[] = {{1, 0}, {1, 0}, {2, 1}, {2, 0}, {0, 1}, {0, 0}, {5, 1}};
        int step_number = 0;
        for (const auto &step : steps) {
          SCOPED_TRACE("step " + std::to_string(step_number++));
          if (boolean) {
            rig.SetBool(holder, "n", step.n != 0);
          }
          else {
            rig.SetInt(holder, "n", step.n);
          }
          rig.SetFloat(rig.Object(0), "mark", 0.0f);
          rig.Step();
          const bool bool_same = boolean && step_number > 1 &&
                                 (steps[step_number - 2].n != 0) == (step.n != 0);
          EXPECT_EQ(rig.Float(rig.Object(0), "mark"), bool_same ? 0.0f : step.mark)
              << LN_DumpProgramText(*program);
        }
      }
    }
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

/* -------------------------------------------------------------------- */
/** \name Aim for Arc and Ray to Plane (2026-11-11): the nodes give LN_AimMath.h's answers in every
 * mode and folded (LN_AimMath_test checks the math itself by flying the throws).
 * \{ */

namespace {

MT_Vector3 V(const Value &value)
{
  return MT_Vector3(value.f[0], value.f[1], value.f[2]);
}

Value FromVector(const MT_Vector3 &vector)
{
  return Value{{vector.x(), vector.y(), vector.z()}};
}

Value Scalar(const float value)
{
  return Value{{value, 0.0f, 0.0f}};
}

}  // namespace

TEST(LN_NodeValues, AimForArcGivesItsSolutionBySpeedAndByArcHeight)
{
  const std::vector<PinSpec> outputs = {
      {"Launch Velocity", Kind::Vector}, {"Can Reach", Kind::Bool}, {"Flight Time", Kind::Float}};
  const std::vector<Value> starts = {
      Value{{0, 0, 0}}, Value{{1, 2, 3}}, Value{{0, 0, 10}}, Value{{kNaN, 0, 0}}};
  const std::vector<Value> targets = {
      Value{{20, 0, 0}}, Value{{-5, 4, 9}}, Value{{0, 0, 3}}, Value{{900, 0, 0}}, Value{{1, 2, 3}}};
  const std::vector<Value> gravities = {
      Value{{0, 0, -9.81f}}, Value{{3, 0, -4}}, Value{{0, 0, 0}}, Value{{0, 0, kInf}}};
  /* By speed, custom gravity (c2 1). */
  std::vector<std::vector<Value>> rows;
  for (const Value &start : starts) {
    for (const Value &target : targets) {
      for (const Value &gravity : gravities) {
        for (const float speed : {0.0f, 15.0f, -1.0f}) {
          for (const float high : {0.0f, 1.0f}) {
            rows.push_back({start, target, Scalar(speed), Scalar(high), gravity});
          }
        }
      }
    }
  }
  CheckNode("LogicNativeAimForArc",
            0,
            1,
            {{"Start", Kind::Vector},
             {"Target", Kind::Vector},
             {"Speed", Kind::Float},
             {"High Arc", Kind::Bool},
             {"Gravity", Kind::Vector}},
            outputs,
            rows,
            [](const std::vector<Value> &in) {
              const LN_ArcSolution s = LN_SolveArc(V(in[0]),
                                                   V(in[1]),
                                                   V(in[4]),
                                                   LN_ArcSolveMode::Speed,
                                                   in[2].f[0],
                                                   in[3].f[0] != 0.0f);
              return std::vector<Value>{
                  FromVector(s.velocity), Scalar(s.can_reach ? 1.0f : 0.0f), Scalar(s.flight_time)};
            });
  /* By arc height (c1 1), custom gravity. */
  rows.clear();
  for (const Value &start : starts) {
    for (const Value &target : targets) {
      for (const Value &gravity : gravities) {
        for (const float height : {0.0f, 2.5f, -1.0f, kNaN}) {
          rows.push_back({start, target, Scalar(height), gravity});
        }
      }
    }
  }
  CheckNode("LogicNativeAimForArc",
            1,
            1,
            {{"Start", Kind::Vector},
             {"Target", Kind::Vector},
             {"Arc Height", Kind::Float},
             {"Gravity", Kind::Vector}},
            outputs,
            rows,
            [](const std::vector<Value> &in) {
              const LN_ArcSolution s = LN_SolveArc(
                  V(in[0]), V(in[1]), V(in[3]), LN_ArcSolveMode::ArcHeight, in[2].f[0], false);
              return std::vector<Value>{
                  FromVector(s.velocity), Scalar(s.can_reach ? 1.0f : 0.0f), Scalar(s.flight_time)};
            });
  /* The scene's gravity (c2 0): the step's snapshot, (0, 0, -9.8) in the rig. */
  rows.clear();
  for (const Value &target : targets) {
    rows.push_back({Value{{0, 0, 0}}, target, Scalar(15.0f), Scalar(0.0f)});
  }
  CheckNode("LogicNativeAimForArc",
            0,
            0,
            {{"Start", Kind::Vector},
             {"Target", Kind::Vector},
             {"Speed", Kind::Float},
             {"High Arc", Kind::Bool}},
            outputs,
            rows,
            [](const std::vector<Value> &in) {
              const LN_ArcSolution s = LN_SolveArc(V(in[0]),
                                                   V(in[1]),
                                                   MT_Vector3(0.0f, 0.0f, -9.8f),
                                                   LN_ArcSolveMode::Speed,
                                                   in[2].f[0],
                                                   in[3].f[0] != 0.0f);
              return std::vector<Value>{
                  FromVector(s.velocity), Scalar(s.can_reach ? 1.0f : 0.0f), Scalar(s.flight_time)};
            });
}

TEST(LN_NodeValues, RayToPlaneGivesWhereTheRayMeetsThePlane)
{
  const std::vector<Value> origins = {
      Value{{0, 0, 10}}, Value{{3, -2, -5}}, Value{{1, 1, 1}}, Value{{kNaN, 0, 0}}};
  const std::vector<Value> directions = {
      Value{{1, 0, -1}}, Value{{0, 0, 3}}, Value{{1, 0, 0}}, Value{{0, 0, 0}}, Value{{0.2f, 0.5f, -2}}};
  const std::vector<Value> points = {Value{{0, 0, 0}}, Value{{2, 3, 4}}};
  const std::vector<Value> normals = {
      Value{{0, 0, 1}}, Value{{0, 0, -7}}, Value{{1, 1, 0}}, Value{{0, 0, 0}}};
  std::vector<std::vector<Value>> rows;
  for (const Value &origin : origins) {
    for (const Value &direction : directions) {
      for (const Value &point : points) {
        for (const Value &normal : normals) {
          rows.push_back({origin, direction, point, normal});
        }
      }
    }
  }
  CheckNode("LogicNativeRayToPlane",
            0,
            0,
            {{"Ray Origin", Kind::Vector},
             {"Ray Direction", Kind::Vector},
             {"Plane Point", Kind::Vector},
             {"Plane Normal", Kind::Vector}},
            {{"Has Result", Kind::Bool},
             {"Point", Kind::Vector},
             {"Normal", Kind::Vector},
             {"Distance", Kind::Float}},
            rows,
            [](const std::vector<Value> &in) {
              const MT_Vector3 origin = V(in[0]);
              const float t = LN_RayPlaneDistance(origin - V(in[2]), V(in[1]), V(in[3]));
              return std::vector<Value>{Scalar(t >= 0.0f ? 1.0f : 0.0f),
                                        FromVector(LN_RayPlanePoint(origin, V(in[1]), t)),
                                        FromVector(LN_RayPlaneNormal(V(in[1]), V(in[3]), t)),
                                        Scalar(std::max(t, 0.0f))};
            });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The "build first" list, phase B (2026-11-16): Smooth Value, Noise, Random Value's
 * shapes, Get Random List Item's Weights, Rotate To's Turn Speed.
 * \{ */

namespace {

/** Smooth Value's contract in double precision (each mode's formula as its tooltip gives it; the
 * runtime works in floats through LN_SmoothValueAdvance). */
struct SmoothModel {
  bool initialized = false;
  double value[3] = {0.0, 0.0, 0.0};
  double velocity[3] = {0.0, 0.0, 0.0};

  void Jump(const double target[3])
  {
    for (int k = 0; k < 3; k++) {
      value[k] = target[k];
      velocity[k] = 0.0;
    }
    initialized = true;
  }
  void Move(const int mode,
            const int size,
            const double target[3],
            const double speed,
            const double max_speed,
            const double dt)
  {
    if (!initialized) {
      Jump(target);
      return;
    }
    if (mode == 0) {
      /* Closes 1 - e^(-speed dt) of the gap. */
      const double share = 1.0 - std::exp(-speed * dt);
      for (int k = 0; k < size; k++) {
        value[k] += (target[k] - value[k]) * share;
      }
    }
    else if (mode == 1) {
      /* Moves speed dt toward the target, stops on it. */
      double distance = 0.0;
      for (int k = 0; k < size; k++) {
        distance += (target[k] - value[k]) * (target[k] - value[k]);
      }
      distance = std::sqrt(distance);
      const double step = speed * dt;
      for (int k = 0; k < size; k++) {
        value[k] = distance <= step ? target[k] :
                                      value[k] + (target[k] - value[k]) * step / distance;
      }
    }
    else {
      /* Game Programming Gems 4's critically damped spring (Unity's SmoothDamp). */
      const double time = std::max(0.0001, speed);
      const double omega = 2.0 / time;
      const double x = omega * dt;
      const double decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
      double change[3] = {0, 0, 0}, length = 0.0;
      for (int k = 0; k < size; k++) {
        change[k] = value[k] - target[k];
        length += change[k] * change[k];
      }
      length = std::sqrt(length);
      if (max_speed > 0.0 && length > max_speed * time) {
        for (int k = 0; k < size; k++) {
          change[k] *= max_speed * time / length;
        }
      }
      double result[3] = {0, 0, 0}, overshoot = 0.0;
      for (int k = 0; k < size; k++) {
        const double temp = (velocity[k] + omega * change[k]) * dt;
        velocity[k] = (velocity[k] - omega * temp) * decay;
        result[k] = value[k] - change[k] + (change[k] + temp) * decay;
        overshoot += (target[k] - value[k]) * (result[k] - target[k]);
      }
      for (int k = 0; k < size; k++) {
        if (overshoot > 0.0) {
          result[k] = target[k];
          velocity[k] = 0.0;
        }
        value[k] = result[k];
      }
    }
  }
};

/** [Step -> Set "o0" (, "o1", "o2") <- Smooth Value(Target from "tx" (, "ty", 0.5)), and the same
 * output once more into "dup"]: the type (0 Float, 1 Vector) and mode; Reset from "reset". */
std::shared_ptr<LN_Program> SmoothValueProgram(const int16_t type,
                                               const int16_t mode,
                                               const float speed,
                                               const float max_speed,
                                               const int16_t every = 0)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate", every);
  GenNode &smooth = b.Add("LogicNativeSmoothValue", type, mode);
  const auto live = [&](const char *idname, const char *name) -> GenNode & {
    GenNode &read = b.Add(idname);
    b.String(read, "Property", name);
    return read;
  };
  if (type == 0) {
    b.Link(live("LogicNativeGetGamePropertyFloat", "tx"), "Value", smooth, "Target");
  }
  else {
    GenNode &combine = b.Add("LogicNativeCombineXYZ");
    b.Link(live("LogicNativeGetGamePropertyFloat", "tx"), "Value", combine, "X");
    b.Link(live("LogicNativeGetGamePropertyFloat", "ty"), "Value", combine, "Y");
    b.Float(combine, "Z", 0.5f);
    b.Link(combine, "Vector", smooth, "Target_Vector");
  }
  b.Float(smooth, mode == 2 ? "Smooth Time" : "Speed", speed);
  b.Float(smooth, "Max Speed", max_speed);
  b.Link(live("LogicNativeGetGamePropertyBool", "reset"), "Value", smooth, "Reset");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *property, GenNode &source, const char *pin) {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", property);
    b.Link(source, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  if (type == 0) {
    write("o0", smooth, "Value");
    write("dup", smooth, "Value");
  }
  else {
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(smooth, "Value_Vector", split, "Vector");
    write("o0", split, "X");
    write("o1", split, "Y");
    write("o2", split, "Z");
    GenNode &split_again = b.Add("LogicNativeSeparateXYZ");
    b.Link(smooth, "Value_Vector", split_again, "Vector");
    write("dup", split_again, "X");
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** Owner `owner`'s Target and Reset at step `step` of the Smooth Value sequence: 0 for two
 * steps, then far away, then elsewhere (a moving target), a Reset at step 30. */
void SmoothTargetAt(const uint32_t owner, const int step, float &tx, float &ty, bool &reset)
{
  tx = step < 2 ? 0.0f : (step < 20 ? 10.0f + float(owner) : -3.0f);
  ty = step < 2 ? 0.0f : (step < 20 ? -4.0f : 2.0f + 0.5f * float(owner));
  reset = step == 30;
}

}  // namespace

/* Smooth Value follows its Target as its mode's formula says (a model in doubles: within 1e-4),
 * in both types, every mode, with and without Max Speed; a second read in the same step gives
 * the same value (it moves once a step); Reset jumps; every mode of the rig gives the same bits,
 * and sixteen owners in a wide run (NSmooth) give the column pass's bits. */
TEST(LN_NodeValues, SmoothValueFollowsItsTargetAsItsModeSays)
{
  constexpr uint32_t owners = 16;
  constexpr int steps = 40;
  struct Case {
    int16_t mode;
    float speed, max_speed;
  };
  const Case cases[] = {{0, 4.0f, 0.0f}, {1, 6.0f, 0.0f}, {2, 0.25f, 0.0f}, {2, 0.25f, 9.0f}};
  for (const int16_t type : {0, 1}) {
    for (const Case &c : cases) {
      SCOPED_TRACE(std::string(type == 0 ? "Float" : "Vector") + " mode " +
                   std::to_string(c.mode) + " max " + std::to_string(c.max_speed));
      const std::shared_ptr<LN_Program> program = SmoothValueProgram(
          type, c.mode, c.speed, c.max_speed);
      ASSERT_NE(program, nullptr);
      const int size = type == 0 ? 1 : 3;
      const auto run = [&](SemRig &rig) {
        std::vector<float> values;
        for (int step = 0; step < steps; step++) {
          for (uint32_t owner = 0; owner < owners; owner++) {
            float tx, ty;
            bool reset;
            SmoothTargetAt(owner, step, tx, ty, reset);
            rig.SetFloat(rig.Object(owner), "tx", tx);
            rig.SetFloat(rig.Object(owner), "ty", ty);
            rig.SetBool(rig.Object(owner), "reset", reset);
          }
          rig.Step();
          for (uint32_t owner = 0; owner < owners; owner++) {
            for (int k = 0; k < size; k++) {
              values.push_back(
                  rig.Float(rig.Object(owner), (std::string("o") + char('0' + k)).c_str(), kNaN));
            }
            EXPECT_TRUE(SameFloat(rig.Float(rig.Object(owner), "dup", kNaN), values[values.size() - size]))
                << "a second read in the same step moved it again (step " << step << ")";
          }
        }
        return values;
      };
      std::vector<float> vm;
      for (const SemMode &mode : AllModes()) {
        SCOPED_TRACE(mode.name);
        SemRig rig(program, mode, owners);
        const std::vector<float> values = run(rig);
        if (vm.empty()) {
          vm = values;
        }
        EXPECT_EQ(values.size(), vm.size());
        for (size_t index = 0; index < values.size() && index < vm.size(); index++) {
          ASSERT_TRUE(SameFloat(values[index], vm[index])) << "value " << index;
        }
      }
      /* The model: the first evaluated step jumps, each later step moves 1/60 s. */
      std::vector<SmoothModel> models(owners);
      size_t index = 0;
      for (int step = 0; step < steps; step++) {
        for (uint32_t owner = 0; owner < owners; owner++) {
          float tx, ty;
          bool reset;
          SmoothTargetAt(owner, step, tx, ty, reset);
          const double target[3] = {tx, type == 0 ? 0.0 : ty, type == 0 ? 0.0 : 0.5};
          if (std::isnan(vm[index])) {
            index += size; /* not evaluated yet (On Init's step) */
            continue;
          }
          if (reset) {
            models[owner].Jump(target);
          }
          else {
            models[owner].Move(c.mode, size, target, c.speed, c.max_speed, SemRig::kStep);
          }
          for (int k = 0; k < size; k++) {
            const double expected = models[owner].value[k];
            EXPECT_NEAR(vm[index], expected, 1.0e-4 * (1.0 + std::fabs(expected)))
                << "step " << step << " owner " << owner << " axis " << k;
            if (reset) {
              EXPECT_EQ(vm[index], float(target[k])) << "Reset jumps to the target exactly";
            }
            index++;
          }
        }
      }
      SemRig wide(program, kProduction, owners);
      wide.SetColumns(true, 2, 1);
      const std::vector<float> columns = run(wide);
      ASSERT_EQ(columns.size(), vm.size());
      for (size_t k = 0; k < vm.size(); k++) {
        ASSERT_TRUE(SameFloat(columns[k], vm[k])) << "a wide run's value " << k;
      }
      if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
        EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
            << "the owners ran wide (NSmooth): " << wide.LastColumnRefusal();
      }
    }
  }
}

/* A Smooth Value read only in a Branch's True route moves only on the steps that read it (the
 * VM evaluates it there: one move of that step's time), as the model says; the wide run's kernel
 * moves it only where its eval mask is set, giving the column pass's bits. */
TEST(LN_NodeValues, SmoothValueReadOnlyInABranchMovesOnlyWhereItIsRead)
{
  constexpr uint32_t owners = 16;
  constexpr int steps = 30;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &branch = b.Add("LogicNativeBranch");
  GenNode &go = b.Add("LogicNativeGetGamePropertyBool");
  b.String(go, "Property", "go");
  b.Link(go, "Value", branch, "Condition");
  b.Link(step, "Out", branch, "Flow");
  GenNode &smooth = b.Add("LogicNativeSmoothValue", 0, 1);
  GenNode &target = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(target, "Property", "tx");
  b.Link(target, "Value", smooth, "Target");
  b.Float(smooth, "Speed", 3.0f);
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "o0");
  b.Link(smooth, "Value", set, "Value");
  b.Link(branch, "True", set, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  /* Owner i reads it on the steps where (step + i) % 3 != 0. */
  const auto go_at = [](const uint32_t owner, const int s) { return (s + int(owner)) % 3 != 0; };
  const auto run = [&](SemRig &rig) {
    std::vector<float> values;
    for (int s = 0; s < steps; s++) {
      for (uint32_t owner = 0; owner < owners; owner++) {
        rig.SetBool(rig.Object(owner), "go", go_at(owner, s));
        rig.SetFloat(rig.Object(owner), "tx", s < 2 ? 0.0f : 5.0f + float(owner));
      }
      rig.Step();
      for (uint32_t owner = 0; owner < owners; owner++) {
        values.push_back(rig.Float(rig.Object(owner), "o0", kNaN));
      }
    }
    return values;
  };
  SemRig rig(program, kProduction, owners);
  const std::vector<float> vm = run(rig);
  for (uint32_t owner = 0; owner < owners; owner++) {
    SmoothModel model;
    float last = kNaN;
    for (int s = 0; s < steps; s++) {
      const float got = vm[size_t(s) * owners + owner];
      if (go_at(owner, s) && !std::isnan(got)) {
        const double target[3] = {s < 2 ? 0.0 : 5.0 + owner, 0.0, 0.0};
        model.Move(1, 1, target, 3.0, 0.0, SemRig::kStep);
        EXPECT_NEAR(got, model.value[0], 1.0e-5) << "owner " << owner << " step " << s;
        last = got;
      }
      else if (!std::isnan(last)) {
        EXPECT_EQ(got, last) << "not read, not written: owner " << owner << " step " << s;
      }
    }
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  const std::vector<float> columns = run(wide);
  ASSERT_EQ(columns.size(), vm.size());
  for (size_t k = 0; k < vm.size(); k++) {
    ASSERT_TRUE(SameFloat(columns[k], vm[k])) << "a wide run's value " << k;
  }
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the owners ran wide (NSmooth with its eval mask): " << wide.LastColumnRefusal();
  }
}

/* Behind Every N Steps a run covers the steps since the last one (the gate's time): Constant
 * Speed moves 3 steps' way at each run of Every 3 Steps, and Smooth closes in one run what it
 * closes in three steps without the gate (e^(-3 s dt), within 1e-5). */
TEST(LN_NodeValues, SmoothValueBehindEveryNStepsCoversTheStepsSinceItsLastRun)
{
  for (const int16_t mode : {0, 1}) {
    SCOPED_TRACE(mode == 0 ? "Smooth" : "Constant Speed");
    const std::shared_ptr<LN_Program> gated = SmoothValueProgram(0, mode, 6.0f, 0.0f, 3);
    const std::shared_ptr<LN_Program> plain = SmoothValueProgram(0, mode, 6.0f, 0.0f);
    ASSERT_NE(gated, nullptr);
    ASSERT_NE(plain, nullptr);
    SemRig a(gated), b(plain);
    std::vector<float> gated_values, plain_values;
    for (int step = 0; step < 31; step++) {
      for (SemRig *rig : {&a, &b}) {
        /* (The Target moves right after the gate's run at step 0: its next run, at step 3,
         * covers the plain tree's steps 1 to 3.) */
        rig->SetFloat(rig->Object(0), "tx", step < 1 ? 0.0f : 100.0f);
        rig->SetFloat(rig->Object(0), "ty", 0.0f);
        rig->SetBool(rig->Object(0), "reset", false);
        rig->Step();
      }
      gated_values.push_back(a.Float(a.Object(0), "o0", kNaN));
      plain_values.push_back(b.Float(b.Object(0), "o0", kNaN));
    }
    /* The gated tree's value changes only on its runs, three steps apart, and then matches the
     * plain tree's at the same step. */
    int changes = 0;
    for (int step = 4; step < 31; step++) {
      if (gated_values[step] != gated_values[step - 1]) {
        changes++;
        EXPECT_NEAR(gated_values[step], plain_values[step], 1.0e-4f)
            << "step " << step << " gated " << gated_values[step - 3] << " -> "
            << gated_values[step] << ", plain " << plain_values[step];
        if (mode == 1) {
          EXPECT_NEAR(gated_values[step] - gated_values[step - 3], 0.3f, 1.0e-5f)
              << "three steps' way (6 units a second) at step " << step;
        }
      }
      else {
        EXPECT_EQ(gated_values[step], gated_values[step - 1]);
      }
    }
    EXPECT_GE(changes, 8);
    EXPECT_LE(changes, 10);
  }
}

/* Noise itself (LN_NoiseValue): from -1 to 1, smooth (a step of 0.001 changes it by little),
 * not 0 on whole numbers (each octave has its own offset), the same for the same inputs, and
 * another seed or channel gives another pattern; NaN and huge inputs give numbers. */
TEST(LN_NodeValues, NoiseIsBoundedSmoothAndSeeded)
{
  for (int dimensions = 1; dimensions <= 4; dimensions++) {
    for (const int octaves : {1, 2, 5, 8}) {
      SCOPED_TRACE("dimensions " + std::to_string(dimensions) + " octaves " +
                   std::to_string(octaves));
      double sum = 0.0, squares = 0.0;
      int count = 0, zeros = 0, seed_same = 0, channel_same = 0;
      float worst_jump = 0.0f;
      for (int i = 0; i < 400; i++) {
        const MT_Vector3 p(0.37f * float(i) - 40.0f, 0.11f * float(i % 17), 0.53f * float(i % 7));
        const float w = 0.29f * float(i);
        const float value = LN_NoiseValue(dimensions, octaves, p, w, 1.0f, 0.5f, 3, 0);
        EXPECT_GE(value, -1.0f);
        EXPECT_LE(value, 1.0f);
        EXPECT_EQ(value, LN_NoiseValue(dimensions, octaves, p, w, 1.0f, 0.5f, 3, 0));
        const float nearby = LN_NoiseValue(
            dimensions, octaves, p + MT_Vector3(0.001f, 0.001f, 0.001f), w + 0.001f, 1.0f, 0.5f, 3, 0);
        worst_jump = std::max(worst_jump, std::fabs(nearby - value));
        seed_same += value == LN_NoiseValue(dimensions, octaves, p, w, 1.0f, 0.5f, 4, 0);
        channel_same += value == LN_NoiseValue(dimensions, octaves, p, w, 1.0f, 0.5f, 3, 1);
        const MT_Vector3 whole(float(i % 9), float(i % 5), float(i % 3));
        zeros += LN_NoiseValue(dimensions, octaves, whole, float(i % 4), 1.0f, 0.5f, 0, 0) == 0.0f;
        sum += value;
        squares += double(value) * value;
        count++;
      }
      const double mean = sum / count;
      const double deviation = std::sqrt(squares / count - mean * mean);
      EXPECT_LT(std::fabs(mean), 0.15);
      EXPECT_GT(deviation, 0.05) << "it varies";
      EXPECT_LT(worst_jump, 0.05f) << "it is smooth";
      EXPECT_LT(seed_same, 5);
      EXPECT_LT(channel_same, 5);
      EXPECT_LT(zeros, 5) << "whole numbers are not all 0";
    }
  }
  for (const float bad : {kNaN, kInf, -kInf, 3.0e9f, -1.0e30f}) {
    const float value = LN_NoiseValue(4, 3, MT_Vector3(bad, 1.0f, bad), bad, 1.0f, 0.5f, 0, 0);
    EXPECT_TRUE(std::isfinite(value));
    EXPECT_GE(value, -1.0f);
    EXPECT_LE(value, 1.0f);
  }
}

/* The Noise node gives LN_NoiseValue of its inputs in every mode and folded (each dimension
 * reads only its inputs; Value is the Vector's X), and sixteen owners in a wide run (FNoise)
 * give the column pass's bits. */
TEST(LN_NodeValues, NoiseNodeGivesItsFunctionEverywhere)
{
  std::vector<std::vector<Value>> rows;
  const std::vector<Value> places = {
      Vec(0, 0, 0), Vec(1.5f, -2.25f, 3), Vec(100.25f, 7, -0.5f), Vec(kNaN, 1, 1)};
  for (const Value &place : places) {
    for (const float w : {0.0f, 2.75f, -11.5f}) {
      for (const float scale : {1.0f, 0.3f}) {
        for (const float roughness : {0.5f, 0.8f}) {
          for (const float seed : {0.0f, 7.0f}) {
            rows.push_back({place, Scalar(w), Scalar(scale), Scalar(roughness), Scalar(seed)});
          }
        }
      }
    }
  }
  for (int16_t dimensions = 1; dimensions <= 4; dimensions++) {
    for (const int16_t octaves : {1, 3}) {
      CheckNode("LogicNativeNoise",
                dimensions,
                octaves,
                {{"Vector", Kind::Vector},
                 {"W", Kind::Float},
                 {"Scale", Kind::Float},
                 {"Roughness", Kind::Float},
                 {"Seed", Kind::Int}},
                {{"Value", Kind::Float}, {"Vector", Kind::Vector}},
                rows,
                [&](const std::vector<Value> &in) {
                  const MT_Vector3 place = dimensions == 1 ? MT_Vector3(0, 0, 0) : V(in[0]);
                  const float w = (dimensions == 1 || dimensions == 4) ? in[1].f[0] : 0.0f;
                  const float roughness = octaves > 1 ? in[3].f[0] : 0.5f;
                  const int32_t seed = int32_t(in[4].f[0]);
                  const auto channel = [&](const uint32_t c) {
                    return LN_NoiseValue(
                        dimensions, octaves, place, w, in[2].f[0], roughness, seed, c);
                  };
                  return std::vector<Value>{Scalar(channel(0)),
                                            Value{{channel(0), channel(1), channel(2)}}};
                });
    }
  }
  /* Wide: each owner's own place. */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &noise = b.Add("LogicNativeNoise", 4, 3);
  GenNode &place = b.Add("LogicNativeCombineXYZ");
  for (const char *axis : {"X", "Y"}) {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", (std::string("p") + axis).c_str());
    b.Link(read, "Value", place, axis);
  }
  b.Link(place, "Vector", noise, "Vector");
  GenNode &time = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(time, "Property", "time");
  b.Link(time, "Value", noise, "W");
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(noise, "Vector", split, "Vector");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const auto &[property, source, pin] :
       std::vector<std::tuple<const char *, GenNode *, const char *>>{
           {"v", &noise, "Value"}, {"x", &split, "X"}, {"y", &split, "Y"}, {"z", &split, "Z"}})
  {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", property);
    b.Link(*source, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  constexpr uint32_t owners = 16;
  const auto place_owners = [&](SemRig &rig) {
    for (uint32_t owner = 0; owner < owners; owner++) {
      rig.SetFloat(rig.Object(owner), "pX", 0.7f * float(owner));
      rig.SetFloat(rig.Object(owner), "pY", -0.3f * float(owner));
      rig.SetFloat(rig.Object(owner), "time", 1.25f);
    }
  };
  SemRig rig(program, kProduction, owners);
  place_owners(rig);
  const std::vector<float> vm = StepValues(rig, owners, {"v", "x", "y", "z"});
  for (uint32_t owner = 0; owner < owners; owner++) {
    EXPECT_EQ(vm[owner * 4], vm[owner * 4 + 1]) << "Value is the Vector's X";
  }
  SemRig wide(program, kProduction, owners);
  place_owners(wide);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(StepValues(wide, owners, {"v", "x", "y", "z"}), vm) << "a wide run's noise";
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the owners ran wide (FNoise): " << wide.LastColumnRefusal();
  }
}

/* Random Value's shapes stay in their shapes (Direction one unit long, In Sphere inside the
 * Radius, In Circle on the ground inside it, In Cone within Angle of the Axis), spread over
 * them (the averages of 640 draws), and a wide run (VRandomShape) gives the column pass's bits. */
TEST(LN_NodeValues, RandomValueShapesStayInsideTheirShapes)
{
  constexpr uint32_t owners = 16;
  constexpr int steps = 40;
  const MT_Vector3 axis = MT_Vector3(1.0f, 1.0f, 0.5f).normalized();
  const float angle = 0.4f, radius = 2.5f;
  for (int16_t type = 4; type <= 7; type++) {
    SCOPED_TRACE("type " + std::to_string(type));
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &random = b.Add("LogicNativeRandomValue", type, 0);
    b.Float(random, "Radius", radius);
    b.Vector(random, "Axis", 1.0f, 1.0f, 0.5f);
    b.Float(random, "Angle", angle);
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(random, "Value_Vector", split, "Vector");
    GenNode *flow = &step;
    const char *flow_pin = "Out";
    for (const char *axis_name : {"X", "Y", "Z"}) {
      GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(set, "Property", axis_name);
      b.Link(split, axis_name, set, "Value");
      b.Link(*flow, flow_pin, set, "Flow");
      flow = &set;
      flow_pin = "Done";
    }
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    const auto run = [&](SemRig &rig) {
      std::vector<float> values;
      for (int s = 0; s < steps; s++) {
        rig.Step();
        for (uint32_t owner = 0; owner < owners; owner++) {
          for (const char *axis_name : {"X", "Y", "Z"}) {
            values.push_back(rig.Float(rig.Object(owner), axis_name, kNaN));
          }
        }
      }
      return values;
    };
    SemRig rig(program, kProduction, owners);
    const std::vector<float> vm = run(rig);
    MT_Vector3 mean(0, 0, 0);
    int inner = 0, count = 0;
    std::set<std::string> distinct;
    for (size_t k = 0; k + 2 < vm.size(); k += 3) {
      const MT_Vector3 v(vm[k], vm[k + 1], vm[k + 2]);
      ASSERT_TRUE(std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()));
      distinct.insert(Bits(v.x()) + Bits(v.y()));
      count++;
      mean += v;
      switch (type) {
        case 4:
          EXPECT_NEAR(v.length(), 1.0f, 1.0e-5f);
          break;
        case 5:
          EXPECT_LE(v.length(), radius * (1.0f + 1.0e-6f));
          inner += v.length() < radius * 0.5f;
          break;
        case 6:
          EXPECT_EQ(v.z(), 0.0f);
          EXPECT_LE(v.length(), radius * (1.0f + 1.0e-6f));
          inner += v.length() < radius * 0.5f;
          break;
        case 7:
          EXPECT_NEAR(v.length(), 1.0f, 1.0e-5f);
          EXPECT_LE(std::acos(std::clamp(float(v.dot(axis)), -1.0f, 1.0f)), angle + 1.0e-3f);
          break;
      }
    }
    mean /= float(count);
    if (type == 7) {
      EXPECT_GT(mean.dot(axis), std::cos(angle)) << "the cone's average points along the Axis";
    }
    else {
      EXPECT_LT(mean.length(), (type == 4 ? 1.0f : radius) * 0.12f) << "spread all around";
    }
    if (type == 5) {
      /* A ball: an eighth of the draws within half the radius. */
      EXPECT_NEAR(float(inner) / float(count), 0.125f, 0.05f);
    }
    if (type == 6) {
      /* A disc: a quarter. */
      EXPECT_NEAR(float(inner) / float(count), 0.25f, 0.06f);
    }
    EXPECT_GT(distinct.size(), size_t(count) / 2) << "new values every step and object";
    SemRig wide(program, kProduction, owners);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(run(wide), vm) << "a wide run's shapes";
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
          << "the owners ran wide (VRandomShape): " << wide.LastColumnRefusal();
    }
  }
}

/* Get Random List Item's Weights: 3, 1, 0 pick the first item three times as often as the
 * second and never the third; Index is the place Value took; weights that are all 0 give -1.
 * Without Weights the node is the one before (the goldens hold its numbers). A wide run gives
 * the column pass's picks (the bridge). */
TEST(LN_NodeValues, RandomListItemFollowsItsWeights)
{
  constexpr uint32_t owners = 16;
  constexpr int steps = 60;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const auto int_list = [&](const char *a, const char *bb, const char *c) -> GenNode & {
    GenNode &list = b.Add("LogicNativeListFromItems", 2); /* custom1 2: the Integers input */
    for (const char *name : {a, bb, c}) {
      GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
      b.String(read, "Property", name);
      b.Link(read, "Value", list, "Integers");
    }
    return list;
  };
  GenNode &items = int_list("i0", "i1", "i2");
  GenNode &weights = int_list("w0", "w1", "w2");
  GenNode &pick = b.Add("LogicNativeListRandomItem");
  b.Link(items, "Integers", pick, "List");
  b.Link(weights, "Integers", pick, "Weights");
  GenNode &set_item = b.Add("LogicNativeSetGamePropertyInt");
  b.String(set_item, "Property", "item");
  b.Link(pick, "Value", set_item, "Value");
  b.Link(step, "Out", set_item, "Flow");
  GenNode &set_index = b.Add("LogicNativeSetGamePropertyInt");
  b.String(set_index, "Property", "index");
  b.Link(pick, "Index", set_index, "Value");
  b.Link(set_item, "Done", set_index, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  const auto run = [&](SemRig &rig, const int w0, const int w1, const int w2) {
    std::vector<float> values;
    for (int s = 0; s < steps; s++) {
      for (uint32_t owner = 0; owner < owners; owner++) {
        KX_GameObject &object = rig.Object(owner);
        rig.SetInt(object, "i0", 10);
        rig.SetInt(object, "i1", 20);
        rig.SetInt(object, "i2", 30);
        rig.SetInt(object, "w0", w0);
        rig.SetInt(object, "w1", w1);
        rig.SetInt(object, "w2", w2);
        rig.SetInt(object, "item", -7);
        rig.SetInt(object, "index", -7);
      }
      rig.Step();
      for (uint32_t owner = 0; owner < owners; owner++) {
        values.push_back(rig.Float(rig.Object(owner), "index", kNaN));
        values.push_back(rig.Float(rig.Object(owner), "item", kNaN));
      }
    }
    return values;
  };
  SemRig rig(program, kProduction, owners);
  const std::vector<float> vm = run(rig, 3, 1, 0);
  int counts[3] = {0, 0, 0}, total = 0;
  for (size_t k = 2 * owners; k + 1 < vm.size(); k += 2) {
    const int index = int(vm[k]);
    ASSERT_TRUE(index == 0 || index == 1) << "the third item has weight 0: " << index;
    EXPECT_EQ(vm[k + 1], float(10 * (index + 1))) << "Value is the item at Index";
    counts[index]++;
    total++;
  }
  EXPECT_NEAR(float(counts[0]) / float(total), 0.75f, 0.06f);
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(run(wide, 3, 1, 0), vm) << "a wide run's picks";
  SemRig none(program, kProduction, owners);
  const std::vector<float> nothing = run(none, 0, 0, 0);
  for (size_t k = 2 * owners; k < nothing.size(); k += 2) {
    EXPECT_EQ(nothing[k], -1.0f) << "no positive weight: no place";
  }
}

/* Rotate To's Turn Speed (custom1 1): each step turns at most Turn Speed / 60 and stops exactly
 * on the target; Factor (custom1 0) is the node before. Sixteen owners aiming at targets around
 * them; a wide run (the motion write) gives the column pass's poses. */
TEST(LN_NodeValues, RotateToTurnsAtItsTurnSpeedAndStopsOnTheTarget)
{
  constexpr uint32_t owners = 16;
  const float speed = float(M_PI) / 2.0f; /* 90 degrees a second: 1.5 a step */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &rotate = b.Add("LogicNativeRotateToward", 1);
  GenNode &target = b.Add("LogicNativeCombineXYZ");
  for (const char *axis : {"X", "Y"}) {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", (std::string("t") + axis).c_str());
    b.Link(read, "Value", target, axis);
  }
  b.Link(target, "Vector", rotate, "Target");
  b.Float(rotate, "Turn Speed", speed);
  b.Int(rotate, "Rot Axis", 2);
  b.Int(rotate, "Front Axis", 1);
  b.Link(step, "Out", rotate, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  /* Owner i's target lies at angle (i - 7.5) * 0.15 from its +Y (front). */
  const auto target_angle = [](const uint32_t owner) { return (float(owner) - 7.5f) * 0.15f; };
  const auto run = [&](SemRig &rig, const int steps) {
    std::vector<float> values;
    for (uint32_t owner = 0; owner < owners; owner++) {
      const float a = target_angle(owner);
      rig.SetFloat(rig.Object(owner), "tX", -std::sin(a) * 5.0f);
      rig.SetFloat(rig.Object(owner), "tY", std::cos(a) * 5.0f);
    }
    for (int s = 0; s < steps; s++) {
      rig.Step();
      for (uint32_t owner = 0; owner < owners; owner++) {
        rig.Object(owner).NodeUpdateGS(0.0);
      }
    }
    for (uint32_t owner = 0; owner < owners; owner++) {
      const MT_Matrix3x3 m = rig.Object(owner).NodeGetWorldOrientation();
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          values.push_back(m[row][column]);
        }
      }
    }
    return values;
  };
  for (const int steps : {1, 4, 30}) {
    SCOPED_TRACE(std::to_string(steps) + " steps");
    SemRig rig(program, kProduction, owners);
    const std::vector<float> vm = run(rig, steps);
    for (uint32_t owner = 0; owner < owners; owner++) {
      /* The front (+Y) turned around Z: its angle from +Y. */
      const MT_Vector3 front(vm[owner * 9 + 1], vm[owner * 9 + 4], vm[owner * 9 + 7]);
      const float turned = std::atan2(-front.x(), front.y());
      const float wanted = target_angle(owner);
      const float most = speed * float(SemRig::kStep) * float(steps);
      const float expected = std::fabs(wanted) <= most ? wanted : std::copysign(most, wanted);
      EXPECT_NEAR(turned, expected, 2.0e-4f) << "owner " << owner;
    }
    SemRig wide(program, kProduction, owners);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(run(wide, steps), vm) << "a wide run's turns";
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force && steps > 1) {
      EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
          << "the owners ran wide: " << wide.LastColumnRefusal();
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rotation angles of turns pointing straight up or down (2026-11-17)
 *
 * MT_Matrix3x3::getEuler gave a turn with a Y angle of -90 degrees (within 0.0135 degrees)
 * the +90 case's X, 180 degrees off with Z folded in, and lost precision within a few degrees
 * of either pole (through a quaternion, as LN_Rotation keeps turns, it was off by up to 2 in
 * an axis there). Near the poles it now finds Z first and X and Y from what is left; elsewhere
 * its result is the old formula's to the bit.
 * \{ */

namespace {

/** The angles' turn against `m`: the largest difference of an entry. */
float AnglesRebuildError(const MT_Matrix3x3 &m, const MT_Vector3 &angles)
{
  const MT_Matrix3x3 rebuilt(angles);
  float error = 0.0f;
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      error = std::max(error, std::fabs(m[row][column] - rebuilt[row][column]));
    }
  }
  return std::isfinite(error) ? error : 9.0f;
}

/** getEuler's formula before 2026-11-17 away from the poles (what it must still give there). */
MT_Vector3 OldEulerAwayFromThePoles(const MT_Matrix3x3 &m)
{
  const float pitch = -asinf(m[2][0]);
  return MT_Vector3(atan2f(m[2][1] / cosf(pitch), m[2][2] / cosf(pitch)),
                    pitch,
                    atan2f(m[1][0] / cosf(pitch), m[0][0] / cosf(pitch)));
}

}  // namespace

/* Every turn within 3 degrees of a pole, made directly and through a quaternion (as Get Object
 * Attribute's rotation and every Rotation socket keep it), gives angles that rebuild it; at a pole
 * Z is 0 and X carries the turn around the vertical (Blender's choice: an action keying only Z
 * keeps X); away from the poles the angles are the old formula's, bit for bit. */
TEST(LN_RotationAngles, ATurnPointingStraightUpOrDownGivesAnglesThatRebuildIt)
{
  constexpr double d = M_PI / 180.0;
  float worst[2] = {0.0f, 0.0f};
  size_t turns = 0;
  for (int x = -180; x <= 180; x += 30) {
    for (int z = -180; z <= 180; z += 30) {
      for (const double pole : {-90.0, 90.0}) {
        for (double off = 0.0; off <= 3.0; off += 0.0005) {
          const double y = pole < 0.0 ? pole + off : pole - off;
          MT_Matrix3x3 direct(MT_Vector3(float(x * d), float(y * d), float(z * d)));
          const MT_Matrix3x3 quaternion = LN_Rotation::FromMatrix(direct).ToMatrix();
          const MT_Matrix3x3 *matrices[2] = {&direct, &quaternion};
          for (int way = 0; way < 2; way++) {
            MT_Scalar ax, ay, az;
            matrices[way]->getEuler(ax, ay, az);
            worst[way] = std::max(worst[way],
                                  AnglesRebuildError(*matrices[way], MT_Vector3(ax, ay, az)));
          }
          turns++;
        }
      }
    }
  }
  /* (Float noise: up to 6e-6 through a quaternion where the old formula still works, 2.6 to 3
   * degrees from a pole; the old code was off by 3e-4 at 0.05 degrees and by 2 at the pole.) */
  EXPECT_LT(worst[0], 1.0e-5f) << "made directly, " << turns << " turns";
  EXPECT_LT(worst[1], 1.0e-5f) << "through a quaternion";

  /* At the poles: Z 0, X the turn around the vertical. */
  const struct {
    float x, y, z, expected_x;
  } poles[] = {{0.0f, -90.0f, 0.0f, 0.0f},
               {30.0f, -90.0f, 0.0f, 30.0f},
               {30.0f, -90.0f, 45.0f, 75.0f},
               {30.0f, 90.0f, 45.0f, -15.0f},
               {-20.0f, 90.0f, 0.0f, -20.0f}};
  for (const auto &pole : poles) {
    MT_Matrix3x3 m(MT_Vector3(float(pole.x * d), float(pole.y * d), float(pole.z * d)));
    MT_Scalar ax, ay, az;
    m.getEuler(ax, ay, az);
    EXPECT_NEAR(ax / d, pole.expected_x, 1.0e-3) << pole.x << " " << pole.y << " " << pole.z;
    EXPECT_NEAR(ay / d, pole.y, 1.0e-3);
    EXPECT_EQ(az, 0.0f);
  }

  /* Away from the poles: the old formula's bits. */
  size_t compared = 0;
  for (int x = -180; x <= 180; x += 15) {
    for (int z = -180; z <= 180; z += 15) {
      for (double y = -87.0; y <= 87.0; y += 0.37) {
        for (int way = 0; way < 2; way++) {
          MT_Matrix3x3 m(MT_Vector3(float(x * d), float(y * d), float(z * d)));
          if (way == 1) {
            m = LN_Rotation::FromMatrix(m).ToMatrix();
          }
          if (std::fabs(m[2][0]) >= 0.999f) {
            continue;
          }
          MT_Scalar ax, ay, az;
          m.getEuler(ax, ay, az);
          const MT_Vector3 old = OldEulerAwayFromThePoles(m);
          ASSERT_TRUE(SameFloat(ax, old.x()) && SameFloat(ay, old.y()) && SameFloat(az, old.z()))
              << x << " " << y << " " << z;
          compared++;
        }
      }
    }
  }
  EXPECT_GT(compared, size_t(500000));
}

/* The game's path (the check euler_check/check_euler.py plays it): an object's World Rotation
 * linked into a vector input is its angles; the angles of objects turned straight down, nearly
 * down, straight up and elsewhere rebuild their turns, in every mode and in a wide run. */
TEST(LN_RotationAngles, GetObjectAttributesRotationAsAnglesRebuildsTheTurn)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &rotation = b.Add("LogicNativeGetObjectAttribute", 8); /* World Rotation */
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(rotation, "Orientation", split, "Vector");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const auto &[name, pin] : std::vector<std::pair<const char *, const char *>>{
           {"ax", "X"}, {"ay", "Y"}, {"az", "Z"}})
  {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", name);
    b.Link(split, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  constexpr double d = M_PI / 180.0;
  struct Turn {
    float x, y, z;
  };
  const std::vector<Turn> turns = {
      {0.0f, -90.0f, 0.0f},   {30.0f, -90.0f, 0.0f},  {30.0f, -90.0f, 45.0f},
      {-20.0f, -90.005f, 10.0f}, {30.0f, -89.99f, 45.0f}, {12.0f, -88.0f, 70.0f},
      {30.0f, 90.0f, 45.0f},  {5.0f, 89.995f, -60.0f}, {-150.0f, 87.5f, 20.0f},
      {30.0f, -60.0f, 45.0f}, {10.0f, 20.0f, 30.0f},  {170.0f, -45.0f, -100.0f},
      {0.0f, -90.0f, 90.0f},  {90.0f, -90.0f, 0.0f},  {-45.0f, 90.0f, -45.0f},
      {60.0f, -89.0f, 0.0f}};
  const uint32_t owners = uint32_t(turns.size());
  const auto turn_of = [&](const uint32_t index) {
    const Turn &t = turns[index];
    const MT_Matrix3x3 m(MT_Vector3(float(t.x * d), float(t.y * d), float(t.z * d)));
    /* Every other one as a turn that went through a quaternion (a hair off the pole). */
    return index % 2 ? LN_Rotation::FromMatrix(m).ToMatrix() : m;
  };
  const auto run = [&](SemRig &rig) {
    for (uint32_t index = 0; index < owners; index++) {
      rig.Object(index).NodeSetLocalOrientation(turn_of(index));
      rig.Object(index).NodeUpdateGS(0.0);
    }
    /* (Two steps: a wide run forms once On Init ran.) */
    rig.Step();
    rig.Step();
    std::vector<float> values;
    for (uint32_t index = 0; index < owners; index++) {
      for (const char *name : {"ax", "ay", "az"}) {
        values.push_back(rig.Float(rig.Object(index), name, kNaN));
      }
    }
    return values;
  };
  std::vector<float> vm;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::vector<float> got = run(rig);
    for (uint32_t index = 0; index < owners; index++) {
      const MT_Vector3 angles(got[index * 3], got[index * 3 + 1], got[index * 3 + 2]);
      EXPECT_LT(AnglesRebuildError(rig.Object(index).NodeGetWorldOrientation(), angles), 2.0e-5f)
          << "turn " << turns[index].x << " " << turns[index].y << " " << turns[index].z
          << " gave " << angles.x() / d << " " << angles.y() / d << " " << angles.z() / d;
    }
    if (vm.empty()) {
      vm = got;
    }
    ASSERT_EQ(got.size(), vm.size());
    for (size_t k = 0; k < got.size(); k++) {
      ASSERT_TRUE(SameFloat(got[k], vm[k])) << "value " << k;
    }
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  const std::vector<float> columns = run(wide);
  ASSERT_EQ(columns.size(), vm.size());
  for (size_t k = 0; k < vm.size(); k++) {
    ASSERT_TRUE(SameFloat(columns[k], vm[k])) << "a wide run's value " << k;
  }
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the objects ran wide: " << wide.LastColumnRefusal();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The "build first" list, phase C (2026-11-16): Play Action's Layer Weight and Action
 * Name, Set Action Layer, On Action Frame.
 * \{ */

/* On Action Frame's rule (LN_ActionFrameCrossed): played frames from the previous read to this
 * one, the start included on an action's first read and after a loop, Ping Pong's turn, a
 * reversed action, a restart, a stopped action; and the Layer Weight's mapping. */
TEST(LN_ActionFrames, AFrameIsPassedOnceOnTheWayTheActionPlays)
{
  const auto read = [](const float frame, const float start, const float end, const int16_t mode,
                       const char *action = "Walk") {
    LN_ActionLayerRead r;
    r.action = action;
    r.frame = frame;
    r.start = start;
    r.end = end;
    r.play_mode = mode;
    return r;
  };
  const LN_ActionLayerRead first = read(3.0f, 1.0f, 20.0f, 0);
  /* The first read: from the start frame (included) to now. */
  EXPECT_TRUE(LN_ActionFrameCrossed(nullptr, first, 1.0f));
  EXPECT_TRUE(LN_ActionFrameCrossed(nullptr, first, 3.0f));
  EXPECT_FALSE(LN_ActionFrameCrossed(nullptr, first, 3.5f));
  /* Forward: (previous, now]. */
  const LN_ActionLayerRead next = read(5.0f, 1.0f, 20.0f, 0);
  EXPECT_FALSE(LN_ActionFrameCrossed(&first, next, 3.0f)) << "not twice";
  EXPECT_TRUE(LN_ActionFrameCrossed(&first, next, 4.0f));
  EXPECT_TRUE(LN_ActionFrameCrossed(&first, next, 5.0f));
  EXPECT_FALSE(LN_ActionFrameCrossed(&first, next, 5.5f));
  /* Stopped on its end: nothing more. */
  const LN_ActionLayerRead done = read(20.0f, 1.0f, 20.0f, 0);
  EXPECT_FALSE(LN_ActionFrameCrossed(&done, done, 20.0f));
  /* Loop: on to the end, then from the start (included). */
  const LN_ActionLayerRead before_wrap = read(19.0f, 1.0f, 20.0f, 1);
  const LN_ActionLayerRead wrapped = read(1.0f, 1.0f, 20.0f, 1);
  EXPECT_TRUE(LN_ActionFrameCrossed(&before_wrap, wrapped, 20.0f));
  EXPECT_TRUE(LN_ActionFrameCrossed(&before_wrap, wrapped, 1.0f)) << "the loop's start frame";
  EXPECT_FALSE(LN_ActionFrameCrossed(&before_wrap, wrapped, 10.0f));
  const LN_ActionLayerRead wrapped_on = read(2.5f, 1.0f, 20.0f, 1);
  EXPECT_TRUE(LN_ActionFrameCrossed(&before_wrap, wrapped_on, 2.0f));
  /* Ping Pong: on to the old end, then back. */
  const LN_ActionLayerRead going = read(18.0f, 1.0f, 20.0f, 2);
  const LN_ActionLayerRead turned = read(19.0f, 20.0f, 1.0f, 2);
  EXPECT_TRUE(LN_ActionFrameCrossed(&going, turned, 20.0f));
  EXPECT_TRUE(LN_ActionFrameCrossed(&going, turned, 19.5f));
  EXPECT_FALSE(LN_ActionFrameCrossed(&going, turned, 18.0f));
  const LN_ActionLayerRead back = read(15.0f, 20.0f, 1.0f, 2);
  EXPECT_TRUE(LN_ActionFrameCrossed(&turned, back, 16.0f)) << "the way back";
  EXPECT_FALSE(LN_ActionFrameCrossed(&turned, back, 19.0f));
  /* A reversed action (start above end) plays down. */
  const LN_ActionLayerRead down_a = read(15.0f, 20.0f, 1.0f, 0);
  const LN_ActionLayerRead down_b = read(12.0f, 20.0f, 1.0f, 0);
  EXPECT_TRUE(LN_ActionFrameCrossed(&down_a, down_b, 13.0f));
  EXPECT_FALSE(LN_ActionFrameCrossed(&down_a, down_b, 15.0f));
  /* Played again (Play mode, the frame back near the start): from the start. */
  const LN_ActionLayerRead again = read(2.0f, 1.0f, 20.0f, 0);
  EXPECT_TRUE(LN_ActionFrameCrossed(&done, again, 1.5f));
  EXPECT_FALSE(LN_ActionFrameCrossed(&done, again, 10.0f));
  /* Another action on the layer: its first read. */
  const LN_ActionLayerRead other = read(4.0f, 1.0f, 20.0f, 0, "Run");
  EXPECT_TRUE(LN_ActionFrameCrossed(&next, other, 2.0f));
  /* A hair back (a start time set again: Set Action Layer's speed, 2026-11-17) is rounding, no
   * loop: frame 11 is not passed again; a move back of a frame still is. */
  const LN_ActionLayerRead looping = read(13.0f, 1.0f, 20.0f, 1);
  const LN_ActionLayerRead hair_back = read(13.0f - 2.0e-3f, 1.0f, 20.0f, 1);
  EXPECT_FALSE(LN_ActionFrameCrossed(&looping, hair_back, 11.0f));
  EXPECT_FALSE(LN_ActionFrameCrossed(&looping, hair_back, 1.0f));
  const LN_ActionLayerRead frame_back = read(12.0f, 1.0f, 20.0f, 1);
  EXPECT_TRUE(LN_ActionFrameCrossed(&looping, frame_back, 11.0f)) << "a loop";
  /* Layer Weight. */
  EXPECT_EQ(LN_ActionLayerWeight(0.0f), -1.0f);
  EXPECT_EQ(LN_ActionLayerWeight(-2.0f), -1.0f);
  EXPECT_EQ(LN_ActionLayerWeight(kNaN), -1.0f);
  EXPECT_EQ(LN_ActionLayerWeight(0.25f), 0.25f);
  EXPECT_EQ(LN_ActionLayerWeight(3.0f), 1.0f);
}

namespace {

/** The rig's action layers (no armatures or actions there): per owner a frame the test sets. */
struct FakeActionLayers {
  std::map<const KX_GameObject *, LN_ActionLayerRead> layers;
};
FakeActionLayers *g_fake_layers = nullptr;

bool ReadFakeActionLayer(const KX_GameObject *object, const short layer, LN_ActionLayerRead &r_read)
{
  if (g_fake_layers == nullptr || layer != 2) {
    return false;
  }
  const auto found = g_fake_layers->layers.find(object);
  if (found == g_fake_layers->layers.end()) {
    return false;
  }
  r_read = found->second;
  return true;
}

}  // namespace

/* On Action Frame through a tree: each owner's looping action (each its own speed) pulses On
 * Frame in the steps it passes frame 10, as the rule says, read from the layer set in the node;
 * the Action filter keeps other actions out; sixteen owners in a wide run give the column pass's
 * pulses. */
TEST(LN_NodeValues, OnActionFramePulsesWhereTheActionPassesTheFrame)
{
  constexpr uint32_t owners = 16;
  constexpr int steps = 60;
  for (const char *filter : {"", "Walk", "Run"}) {
    SCOPED_TRACE(std::string("Action filter '") + filter + "'");
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnActionFrame");
    b.Int(event, "Layer", 2);
    b.Float(event, "Frame", 10.0f);
    b.String(event, "Action", filter);
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "fired");
    b.Float(set, "Value", 1.0f);
    b.Link(event, "On Frame", set, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    const auto frame_at = [](const uint32_t owner, const int step) {
      /* 1 to 20, looping, 0.7 + owner / 10 frames a step. */
      const float speed = 0.7f + 0.1f * float(owner);
      return 1.0f + std::fmod(speed * float(step), 19.0f);
    };
    FakeActionLayers fake;
    g_fake_layers = &fake;
    LN_SetActionLayerReaderForTests(ReadFakeActionLayer);
    const auto run = [&](SemRig &rig) {
      std::vector<float> fired;
      for (int step = 0; step < steps; step++) {
        for (uint32_t owner = 0; owner < owners; owner++) {
          LN_ActionLayerRead &layer = fake.layers[&rig.Object(owner)];
          layer.action = "Walk";
          layer.frame = frame_at(owner, step);
          layer.start = 1.0f;
          layer.end = 20.0f;
          layer.play_mode = 1;
          rig.SetFloat(rig.Object(owner), "fired", 0.0f);
        }
        rig.Step();
        for (uint32_t owner = 0; owner < owners; owner++) {
          fired.push_back(rig.Float(rig.Object(owner), "fired", kNaN));
        }
      }
      return fired;
    };
    SemRig rig(program, kProduction, owners);
    const std::vector<float> vm = run(rig);
    /* The rule on the same reads (On Init's step reads too: every step does). */
    const bool passes = std::string(filter) != "Run";
    for (uint32_t owner = 0; owner < owners; owner++) {
      LN_ActionLayerRead previous;
      bool has_previous = false;
      int pulses = 0;
      for (int step = 0; step < steps; step++) {
        LN_ActionLayerRead now;
        now.action = "Walk";
        now.frame = frame_at(owner, step);
        now.start = 1.0f;
        now.end = 20.0f;
        now.play_mode = 1;
        const bool expected = passes &&
                              LN_ActionFrameCrossed(has_previous ? &previous : nullptr, now, 10.0f);
        previous = now;
        has_previous = true;
        EXPECT_EQ(vm[size_t(step) * owners + owner], expected ? 1.0f : 0.0f)
            << "owner " << owner << " step " << step;
        pulses += expected;
      }
      if (passes) {
        EXPECT_GE(pulses, 2) << "loops pass frame 10 again";
      }
    }
    SemRig wide(program, kProduction, owners);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(run(wide), vm) << "a wide run's pulses";
    LN_SetActionLayerReaderForTests(nullptr);
    g_fake_layers = nullptr;
  }
}

/* Play Action records its Layer Weight as BL_Action takes it (0: -1, the layer covers the ones
 * below, as every Play Action before; 0.4: 0.4) and its Action Name in place of the picked
 * action; Set Action Layer records Set Action Frame's command with its attribute (1 Speed, 2
 * Layer Weight). */
TEST(LN_NodeValues, PlayActionAndSetActionLayerRecordTheirValues)
{
  const auto recorded = [](const std::function<void(TreeBuilder &, GenNode &)> &build) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    build(b, event);
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << report;
    if (program == nullptr) {
      return std::string();
    }
    SemRig rig(program, kProduction);
    rig.SetFlush(false);
    rig.Step();
    return rig.LastRecorded();
  };
  for (const float weight : {0.0f, 0.4f, 1.5f}) {
    const std::string dump = recorded([&](TreeBuilder &b, GenNode &event) {
      GenNode &play = b.Add("LogicNativePlayAction");
      b.Float(play, "Layer Weight", weight);
      b.String(play, "Action Name", "Run");
      b.Float(play, "Speed", 1.25f);
      b.Link(event, "Out", play, "Flow");
    });
    const float expected = weight == 0.0f ? -1.0f : std::min(weight, 1.0f);
    EXPECT_NE(dump.find("type=PlayAction"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vec2=" + LN_DumpVector3(MT_Vector3(expected, 1.25f, 0.0f))),
              std::string::npos)
        << "Layer Weight " << weight << ": " << dump;
    EXPECT_NE(dump.find("name=" + LN_DumpQuoted("Run")), std::string::npos) << dump;
  }
  /* A linked Action Name (a game property chooses the action). */
  const std::string linked = recorded([](TreeBuilder &b, GenNode &event) {
    GenNode &play = b.Add("LogicNativePlayAction");
    GenNode &name = b.Add("LogicNativeGetGamePropertyString");
    b.String(name, "Property", "anim");
    b.Link(name, "Value", play, "Action Name");
    b.Link(event, "Out", play, "Flow");
  });
  EXPECT_NE(linked.find("type=PlayAction"), std::string::npos) << linked;
  for (const int16_t attribute : {0, 1}) {
    const std::string dump = recorded([&](TreeBuilder &b, GenNode &event) {
      GenNode &set = b.Add("LogicNativeSetActionLayer", attribute);
      b.Int(set, "Layer", 3);
      b.Float(set, "Speed", 2.0f);
      b.Float(set, "Layer Weight", 0.5f);
      b.Link(event, "Out", set, "Flow");
    });
    EXPECT_NE(dump.find("type=SetActionFrame"), std::string::npos) << dump;
    EXPECT_NE(dump.find(" scalar=" + LN_DumpFloatBits(attribute == 0 ? 2.0f : 0.5f) + " int=3"),
              std::string::npos)
        << dump;
    EXPECT_NE(dump.find(" anim=" + std::to_string(attribute + 1)), std::string::npos) << dump;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The "build first" list, phase D (2026-11-16): Find Objects.
 * \{ */

namespace {

/** A place from a hash (deterministic, spread over a box). */
MT_Vector3 HashedPlace(const uint64_t seed, const float size, const float height)
{
  const uint64_t a = LN_RandomMix(seed * 3 + 1), b = LN_RandomMix(seed * 3 + 2),
                 c = LN_RandomMix(seed * 3 + 3);
  return MT_Vector3((LN_RandomUnitFloat(a) - 0.5f) * size,
                    (LN_RandomUnitFloat(b) - 0.5f) * size,
                    (LN_RandomUnitFloat(c) - 0.5f) * height);
}

/** Find Objects' answer worked out by looking at every object (the model of LN_FindIndex). */
LN_FindAnswer ScanEveryObject(const std::vector<KX_GameObject *> &objects,
                              const std::vector<bool> &member,
                              const LN_FindRequest &request)
{
  LN_FindAnswer answer;
  struct Candidate {
    uint32_t order;
    float squared;
  };
  std::vector<Candidate> candidates;
  for (uint32_t index = 0; index < objects.size(); index++) {
    if (!member[index] || objects[index] == request.exclude_a ||
        objects[index] == request.exclude_b)
    {
      continue;
    }
    const MT_Vector3 d = objects[index]->NodeGetWorldPosition() - request.center;
    const float squared = d.x() * d.x() + d.y() * d.y() + d.z() * d.z();
    if (request.radius > 0.0f && !(squared <= request.radius * request.radius)) {
      continue;
    }
    if (request.cos_half_angle > -1.0f && squared > 0.0f) {
      const float along = d.x() * request.forward.x() + d.y() * request.forward.y() +
                          d.z() * request.forward.z();
      if (along < request.cos_half_angle * std::sqrt(squared)) {
        continue;
      }
    }
    candidates.push_back({index, squared});
  }
  answer.count = int32_t(candidates.size());
  if (candidates.empty()) {
    return answer;
  }
  uint32_t best = candidates.front().order;
  float best_squared = candidates.front().squared;
  for (const Candidate &c : candidates) {
    const bool better = request.pick == LN_FindPick::Farthest ? c.squared > best_squared :
                                                                c.squared < best_squared;
    if (better || (c.squared == best_squared && c.order < best)) {
      best = c.order;
      best_squared = c.squared;
    }
  }
  if (request.pick == LN_FindPick::Random) {
    best = candidates[std::min(size_t(request.random_unit * float(candidates.size())),
                               candidates.size() - 1)]
               .order;
  }
  const auto hit = [&](const uint32_t order) {
    LN_FindHit h;
    h.object = objects[order];
    h.position = objects[order]->NodeGetWorldPosition();
    const MT_Vector3 d = h.position - request.center;
    h.distance = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
    h.order = order;
    return h;
  };
  if (request.pick == LN_FindPick::All) {
    for (const Candidate &c : candidates) {
      answer.list.push_back(hit(c.order));
    }
    std::sort(answer.list.begin(), answer.list.end(), [](const LN_FindHit &a, const LN_FindHit &b) {
      return a.distance < b.distance || (a.distance == b.distance && a.order < b.order);
    });
    best = answer.list.front().order;
  }
  answer.found = true;
  answer.best = hit(best);
  return answer;
}

std::shared_ptr<LN_Program> FindNearestProgram(const char *property, const int16_t pick)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &find = b.Add("LogicNativeFindObjects", 2, pick);
  b.String(find, "Property", property);
  b.Float(find, "Radius", 0.0f);
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(find, "Position", split, "Vector");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const auto &[name, source, pin] :
       std::vector<std::tuple<const char *, GenNode *, const char *>>{{"dist", &find, "Distance"},
                                                                     {"count", &find, "Count"},
                                                                     {"px", &split, "X"},
                                                                     {"py", &split, "Y"}})
  {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", name);
    b.Link(*source, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

}  // namespace

/* LN_FindIndex's grids answer as a scan of every object does (every pick, radius, view cone,
 * exclusion, with and without Count; a spread-out world and a flat one, the All source and a
 * property's): the grid is only a faster way to the same answer. */
/* Objects a float's range apart (finite, their spread not) make one grid cell: the grid's cell
 * count overflowed and its vector threw (the column soak's seed 74692, 2026-11-17); an object
 * with no finite position is never found (LN_FindIndex::Begin). The others are found as before. */
TEST(LN_FindObjects, ObjectsWithoutAFinitePositionAreNeverFound)
{
  for (const int16_t pick : {int16_t(0), int16_t(3)}) {
    SCOPED_TRACE(pick);
    const std::shared_ptr<LN_Program> program = FindNearestProgram("pickup", pick);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction, 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto &[name, position] :
         std::vector<std::pair<const char *, MT_Vector3>>{{"Lost", MT_Vector3(nan, 0.0f, 0.0f)},
                                                          {"FarA", MT_Vector3(3.0e38f, 0, 0)},
                                                          {"FarB", MT_Vector3(-3.0e38f, 0, 0)},
                                                          {"Near", MT_Vector3(2.0f, 1.0f, 0)}})
    {
      rig.SetFloat(rig.AddObject(name, position), "pickup", 1.0f);
    }
    rig.Step();
    KX_GameObject &owner = rig.Object(0);
    EXPECT_FLOAT_EQ(rig.Float(owner, "dist"), std::sqrt(5.0f));
    EXPECT_EQ(rig.Float(owner, "count"), 3.0f);
    EXPECT_EQ(rig.Float(owner, "px"), 2.0f);
    EXPECT_EQ(rig.Float(owner, "py"), 1.0f);
  }
}

TEST(LN_FindObjects, TheGridAnswersAsAScanOfEveryObject)
{
  const std::shared_ptr<LN_Program> program = FindNearestProgram("pickup", 0);
  ASSERT_NE(program, nullptr);
  for (const float height : {40.0f, 0.0f}) {
    SCOPED_TRACE(height == 0.0f ? "a flat world" : "a spread-out world");
    SemRig rig(program, kProduction, 1);
    std::vector<KX_GameObject *> objects;
    std::vector<bool> tagged;
    for (uint64_t k = 0; k < 400; k++) {
      /* Clusters and loners. */
      const MT_Vector3 place = k % 5 == 0 ? HashedPlace(k, 200.0f, height) :
                                            HashedPlace(k % 7, 30.0f, height * 0.25f) +
                                                HashedPlace(k + 1000, 6.0f, height * 0.05f);
      KX_GameObject &object = rig.AddObject("Thing" + std::to_string(k), place);
      object.NodeSetLocalPosition(place);
      object.NodeUpdateGS(0.0);
      if (k % 3 == 0) {
        rig.SetFloat(object, "tag", 1.0f);
      }
      objects.push_back(&object);
      tagged.push_back(k % 3 == 0);
    }
    LN_FindIndex index;
    index.Begin(7, objects);
    LN_FindSourceKey all, tag;
    tag.source = LN_FindSource::Property;
    tag.name = "tag";
    index.AddSource(all, [](KX_GameObject *) { return true; });
    index.AddSource(tag, [&](KX_GameObject *object) { return object->GetProperty("tag") != nullptr; });
    const std::vector<bool> everyone(objects.size(), true);
    int compared = 0, found = 0;
    for (uint64_t q = 0; q < 1500; q++) {
      LN_FindRequest request;
      request.center = HashedPlace(q + 5000, 260.0f, height * 1.2f);
      const float radii[] = {0.0f, 3.0f, 12.0f, 60.0f};
      request.radius = radii[q % 4];
      const float cones[] = {-2.0f, 0.0f, 0.7f};
      request.cos_half_angle = cones[(q / 4) % 3];
      request.forward = HashedPlace(q + 9000, 2.0f, 2.0f);
      if (request.forward.length() < 1.0e-3f) {
        request.forward = MT_Vector3(0.0f, 1.0f, 0.0f);
      }
      request.forward.normalize();
      request.pick = LN_FindPick((q / 12) % 4);
      request.need_count = (q / 48) % 2 == 0;
      request.random_unit = LN_RandomUnitFloat(LN_RandomMix(q));
      request.exclude_a = objects[q % objects.size()];
      const bool by_tag = q % 2 == 0;
      LN_FindAnswer grid;
      index.Search(index.FindSource(by_tag ? tag : all), request, grid);
      const LN_FindAnswer scan = ScanEveryObject(objects, by_tag ? tagged : everyone, request);
      ASSERT_EQ(grid.found, scan.found) << "request " << q;
      compared++;
      if (!scan.found) {
        continue;
      }
      found++;
      EXPECT_EQ(grid.best.object, scan.best.object) << "request " << q;
      EXPECT_EQ(grid.best.distance, scan.best.distance) << "request " << q;
      const bool count_known = request.need_count || request.pick != LN_FindPick::Nearest ||
                               request.radius > 0.0f;
      if (count_known) {
        EXPECT_EQ(grid.count, scan.count) << "request " << q;
      }
      ASSERT_EQ(grid.list.size(), scan.list.size()) << "request " << q;
      for (size_t k = 0; k < scan.list.size(); k++) {
        EXPECT_EQ(grid.list[k].object, scan.list[k].object) << "request " << q << " item " << k;
      }
    }
    EXPECT_EQ(compared, 1500);
    EXPECT_GT(found, 500); /* (many requests have a small radius or cone far from everything) */
  }
}

/* Find Objects through a tree: sixteen NPCs each find the nearest of thirty objects with a game
 * property (Distance, Count, Position as a scan gives them), the same in every mode, and a wide
 * run gives the column pass's values (the query on the helpers' bridge). */
TEST(LN_NodeValues, FindObjectsGivesEachNPCItsNearestTaggedObject)
{
  constexpr uint32_t owners = 16;
  for (const int16_t pick : {0, 1}) {
    SCOPED_TRACE(pick == 0 ? "Nearest" : "Farthest");
    const std::shared_ptr<LN_Program> program = FindNearestProgram("pickup", pick);
    ASSERT_NE(program, nullptr);
    const auto build = [&](SemRig &rig) {
      for (uint32_t owner = 0; owner < owners; owner++) {
        const MT_Vector3 place(float(owner % 4) * 9.0f, float(owner / 4) * 7.0f, 0.0f);
        rig.Object(owner).NodeSetLocalPosition(place);
        rig.Object(owner).NodeUpdateGS(0.0);
      }
      std::vector<KX_GameObject *> pickups;
      for (uint64_t k = 0; k < 30; k++) {
        const MT_Vector3 place = HashedPlace(k + 77, 60.0f, 2.0f) + MT_Vector3(14.0f, 10.0f, 0.0f);
        KX_GameObject &pickup = rig.AddObject("Pickup" + std::to_string(k), place);
        pickup.NodeSetLocalPosition(place);
        pickup.NodeUpdateGS(0.0);
        rig.SetFloat(pickup, "pickup", 1.0f);
        pickups.push_back(&pickup);
      }
      return pickups;
    };
    const auto values = [&](SemRig &rig) {
      std::vector<float> out;
      for (int s = 0; s < 2; s++) {
        rig.Step();
      }
      for (uint32_t owner = 0; owner < owners; owner++) {
        for (const char *name : {"dist", "count", "px", "py"}) {
          out.push_back(rig.Float(rig.Object(owner), name, kNaN));
        }
      }
      return out;
    };
    std::vector<float> vm;
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode, owners);
      const std::vector<KX_GameObject *> pickups = build(rig);
      const std::vector<float> got = values(rig);
      for (uint32_t owner = 0; owner < owners; owner++) {
        LN_FindRequest request;
        request.center = rig.Object(owner).NodeGetWorldPosition();
        request.pick = LN_FindPick(pick);
        request.exclude_a = &rig.Object(owner);
        const LN_FindAnswer scan = ScanEveryObject(
            pickups, std::vector<bool>(pickups.size(), true), request);
        ASSERT_TRUE(scan.found);
        EXPECT_EQ(got[owner * 4 + 0], scan.best.distance) << "owner " << owner;
        EXPECT_EQ(got[owner * 4 + 1], 30.0f) << "owner " << owner;
        EXPECT_EQ(got[owner * 4 + 2], scan.best.position.x()) << "owner " << owner;
        EXPECT_EQ(got[owner * 4 + 3], scan.best.position.y()) << "owner " << owner;
      }
      if (vm.empty()) {
        vm = got;
      }
      EXPECT_EQ(got, vm);
    }
    SemRig wide(program, kProduction, owners);
    build(wide);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(values(wide), vm) << "a wide run's finds";
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
          << "the NPCs ran wide: " << wide.LastColumnRefusal();
    }
  }
}

/* Find Objects' answers inside stateful nodes' inputs (2026-11-17, the game check's NPC brain):
 * Smooth Values of the nearest pickup's distance, position and the count within the radius, a
 * Boolean Edge of Found. The warmed answer is the same at every read of the tick, so the NPCs
 * run wide (they ran tree by tree: "a bridged node inside another stateful node's input") and end
 * as their column passes, in every mode, while the pickups move. */
TEST(LN_NodeValues, FindObjectsInsideStatefulInputsRunWide)
{
  constexpr uint32_t owners = 16;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &find = b.Add("LogicNativeFindObjects", 2, 0);
  b.String(find, "Property", "pickup");
  b.Float(find, "Radius", 9.0f);
  GenNode &smooth_distance = b.Add("LogicNativeSmoothValue", 0, 0);
  b.Link(find, "Distance", smooth_distance, "Target");
  b.Float(smooth_distance, "Speed", 4.0f);
  GenNode &smooth_place = b.Add("LogicNativeSmoothValue", 1, 0);
  b.Link(find, "Position", smooth_place, "Target_Vector");
  b.Float(smooth_place, "Speed", 3.0f);
  GenNode &smooth_count = b.Add("LogicNativeSmoothValue", 0, 0);
  b.Link(find, "Count", smooth_count, "Target");
  b.Float(smooth_count, "Speed", 2.0f);
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(smooth_place, "Value_Vector", split, "Vector");
  GenNode &edge = b.Add("LogicNativeBooleanEdge");
  b.Link(find, "Found", edge, "Condition");
  GenNode &rose = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(rose, "Property", "rose");
  b.Link(find, "Distance", rose, "Value");
  b.Link(edge, "Rising", rose, "Flow");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const auto &[name, source, pin] :
       std::vector<std::tuple<const char *, GenNode *, const char *>>{
           {"sd", &smooth_distance, "Value"},
           {"sx", &split, "X"},
           {"sy", &split, "Y"},
           {"sc", &smooth_count, "Value"}})
  {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", name);
    b.Link(*source, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  constexpr int steps = 12;
  const auto run = [&](SemRig &rig) {
    for (uint32_t owner = 0; owner < owners; owner++) {
      const MT_Vector3 place(float(owner % 4) * 9.0f, float(owner / 4) * 7.0f, 0.0f);
      rig.Object(owner).NodeSetLocalPosition(place);
      rig.Object(owner).NodeUpdateGS(0.0);
      rig.SetFloat(rig.Object(owner), "rose", -1.0f);
    }
    std::vector<KX_GameObject *> pickups;
    for (uint64_t k = 0; k < 12; k++) {
      KX_GameObject &pickup = rig.AddObject("Pickup" + std::to_string(k), MT_Vector3());
      rig.SetFloat(pickup, "pickup", 1.0f);
      pickups.push_back(&pickup);
    }
    std::vector<float> out;
    for (int s = 0; s < steps; s++) {
      /* The pickups wander (in and out of the NPCs' radius). */
      for (size_t k = 0; k < pickups.size(); k++) {
        const MT_Vector3 place = HashedPlace(k + 31, 30.0f, 0.0f) +
                                 MT_Vector3(float(s) * 1.7f * (k % 2 ? 1.0f : -1.0f),
                                            float(s) * 0.9f, 0.0f);
        pickups[k]->NodeSetLocalPosition(place);
        pickups[k]->NodeUpdateGS(0.0);
      }
      rig.Step();
      for (uint32_t owner = 0; owner < owners; owner++) {
        for (const char *name : {"sd", "sx", "sy", "sc", "rose"}) {
          out.push_back(rig.Float(rig.Object(owner), name, kNaN));
        }
      }
    }
    return out;
  };
  std::vector<float> vm;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::vector<float> got = run(rig);
    if (vm.empty()) {
      vm = got;
    }
    ASSERT_EQ(got.size(), vm.size());
    for (size_t k = 0; k < got.size(); k++) {
      ASSERT_TRUE(SameFloat(got[k], vm[k])) << "value " << k;
    }
  }
  /* The answers moved the smoothed values and the edge rose somewhere. */
  size_t rises = 0, moved = 0;
  for (size_t k = 0; k + 5 * owners < vm.size(); k++) {
    moved += vm[k] != vm[k + 5 * owners];
  }
  for (size_t k = 4; k < vm.size(); k += 5) {
    rises += vm[k] != -1.0f;
  }
  EXPECT_GT(moved, size_t(100));
  EXPECT_GT(rises, size_t(0));
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  const std::vector<float> columns = run(wide);
  ASSERT_EQ(columns.size(), vm.size());
  for (size_t k = 0; k < vm.size(); k++) {
    ASSERT_TRUE(SameFloat(columns[k], vm[k])) << "a wide run's value " << k;
  }
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the NPCs ran wide: " << wide.LastColumnRefusal();
  }
}

/** \} */

/* Find Objects' query is warmed before the trees record, on the job pool's helpers (its search
 * reads only the tick's poses): a crowd's searches run in parallel (LN_QUERY_WARM_ON_HELPERS). */
TEST(LN_NodeValues, FindObjectsIsWarmedOnTheHelpers)
{
  const std::shared_ptr<LN_Program> program = FindNearestProgram("pickup", 0);
  ASSERT_NE(program, nullptr);
  const std::vector<uint8_t> events = LN_QueryWarmEvents(*program);
  ASSERT_EQ(events.size(), program->GetQueryExpressions().size());
  bool warmed = false;
  for (size_t query = 0; query < events.size(); query++) {
    if (program->GetQueryExpressions()[query].kind == LN_QueryExpressionKind::FindObjects) {
      EXPECT_NE(events[query] & (1u << uint8_t(LN_Event::OnFixedUpdate)), 0u);
      EXPECT_NE(events[query] & LN_QUERY_WARM_ON_HELPERS, 0u);
      warmed = true;
    }
  }
  EXPECT_TRUE(warmed);
}

/* Timing (--gtest_also_run_disabled_tests): 1,000 NPCs each finding the nearest of 1,000 objects
 * with a property, per step: the column pass (one after another) and a wide run with 1 and 8
 * helpers; the median of 5 rounds of 20 steps. */
TEST(LN_NodeValues, DISABLED_FindObjectsCrowdCost)
{
  constexpr uint32_t owners = 1000;
  const std::shared_ptr<LN_Program> program = FindNearestProgram("enemy", 0);
  ASSERT_NE(program, nullptr);
  for (const int way : {0, 1, 8}) {
    std::vector<double> rounds;
    for (int round = 0; round < 5; round++) {
      SemRig rig(program, kProduction, owners);
      for (uint32_t owner = 0; owner < owners; owner++) {
        const MT_Vector3 place = HashedPlace(owner, 400.0f, 0.0f);
        rig.Object(owner).NodeSetLocalPosition(place);
        rig.Object(owner).NodeUpdateGS(0.0);
      }
      for (uint64_t k = 0; k < 1000; k++) {
        const MT_Vector3 place = HashedPlace(k + 50000, 400.0f, 0.0f);
        KX_GameObject &enemy = rig.AddObject("Enemy" + std::to_string(k), place);
        enemy.NodeSetLocalPosition(place);
        enemy.NodeUpdateGS(0.0);
        rig.SetFloat(enemy, "enemy", 1.0f);
      }
      if (way > 0) {
        rig.SetColumns(true, uint32_t(way - 1), 1);
      }
      rig.Step();
      rig.Step();
      const auto start = std::chrono::steady_clock::now();
      for (int s = 0; s < 20; s++) {
        rig.Step();
      }
      rounds.push_back(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count() /
                       20.0);
    }
    std::sort(rounds.begin(), rounds.end());
    if (way == 0) {
      /* The search alone (its index), and the same tree's writes without a search. */
      SemRig rig(program, kProduction, owners);
      std::vector<KX_GameObject *> objects;
      for (uint64_t k = 0; k < 1000; k++) {
        const MT_Vector3 place = HashedPlace(k + 50000, 400.0f, 0.0f);
        KX_GameObject &enemy = rig.AddObject("Enemy" + std::to_string(k), place);
        enemy.NodeSetLocalPosition(place);
        enemy.NodeUpdateGS(0.0);
        objects.push_back(&enemy);
      }
      LN_FindIndex index;
      index.Begin(1, objects);
      LN_FindSourceKey all;
      index.AddSource(all, [](KX_GameObject *) { return true; });
      for (const bool count : {true, false}) {
        const auto start = std::chrono::steady_clock::now();
        float sum = 0.0f;
        for (uint32_t owner = 0; owner < owners; owner++) {
          LN_FindRequest request;
          request.center = HashedPlace(owner, 400.0f, 0.0f);
          request.need_count = count;
          LN_FindAnswer answer;
          index.Search(0, request, answer);
          sum += answer.best.distance;
        }
        std::printf("  the searches alone (%s): %.3f ms (%g)\n",
                    count ? "with Count: every object" : "without Count: rings",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              start)
                        .count(),
                    double(sum));
      }
    }
    std::printf("Find Objects, 1,000 NPCs x 1,000 objects, %s: %.3f ms a step\n",
                way == 0 ? "column pass" : (way == 1 ? "wide, no helper" : "wide, 7 helpers"),
                rounds[2]);
  }
}

/* Find Objects of every object leaves the center (the tree's own object) out with Exclude Self:
 * two NPCs find each other; without it each finds itself (distance 0). */
TEST(LN_NodeValues, FindObjectsLeavesItsOwnObjectOut)
{
  for (const bool exclude : {true, false}) {
    SCOPED_TRACE(exclude ? "Exclude Self" : "itself allowed");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &find = b.Add("LogicNativeFindObjects", 0, 0);
    b.Bool(find, "Exclude Self", exclude);
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "dist");
    b.Link(find, "Distance", set, "Value");
    b.Link(step, "Out", set, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    SemRig rig(program, kProduction, 2);
    rig.Object(0).NodeSetLocalPosition(MT_Vector3(1.0f, 2.0f, 0.0f));
    rig.Object(1).NodeSetLocalPosition(MT_Vector3(4.0f, 6.0f, 0.0f));
    rig.Object(0).NodeUpdateGS(0.0);
    rig.Object(1).NodeUpdateGS(0.0);
    rig.Step();
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "dist", kNaN), exclude ? 5.0f : 0.0f);
    EXPECT_EQ(rig.Float(rig.Object(1), "dist", kNaN), exclude ? 5.0f : 0.0f);
  }
}

/* -------------------------------------------------------------------- */
/** \name The "build first" list, phase E (2026-11-17): State Machine.
 * \{ */

namespace {

std::shared_ptr<LN_Program> StateMachineProgram()
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &machine = b.Add("LogicNativeStateMachine", 0, 3, 3); /* three State outputs */
  b.Int(machine, "Start State", 1);
  b.Link(step, "Out", machine, "Update");
  GenNode &branch = b.Add("LogicNativeBranch");
  GenNode &go = b.Add("LogicNativeGetGamePropertyBool");
  b.String(go, "Property", "go");
  b.Link(go, "Value", branch, "Condition");
  b.Link(step, "Out", branch, "Flow");
  b.Link(branch, "True", machine, "Go To");
  GenNode &target = b.Add("LogicNativeGetGamePropertyInt");
  b.String(target, "Property", "target");
  b.Link(target, "Value", machine, "State");
  const auto mark = [&](const char *output, const char *property) {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", property);
    b.Float(set, "Value", 1.0f);
    b.Link(machine, output, set, "Flow");
  };
  mark("State 0", "in0");
  mark("State 1", "in1");
  mark("State 2", "in2");
  mark("Entered", "entered");
  /* The outputs, written after the routes of the step (a second step event). */
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const auto &[name, pin] : std::vector<std::pair<const char *, const char *>>{
           {"cur", "Current State"}, {"prev", "Previous State"}, {"time", "Time in State"}})
  {
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", name);
    b.Link(machine, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

}  // namespace

/* State Machine: it starts in the Start State (1 here); Go To changes the state (Entered pulses,
 * not when it already is in it), Update continues on the output of the state it is in, Previous
 * State is the one before and Time in State the game time since it entered (a step's time a
 * step, frames between them or not); sixteen NPCs on their own schedules end a wide run as their
 * column passes. */
TEST(LN_NodeValues, StateMachineChangesStatesAndRoutesUpdate)
{
  constexpr uint32_t owners = 16;
  const std::shared_ptr<LN_Program> program = StateMachineProgram();
  ASSERT_NE(program, nullptr);
  /* Step s of owner o: Go To (target) on the steps of its schedule. */
  const auto schedule = [](const uint32_t owner, const int step, bool &go, int &target) {
    const int s = step - int(owner % 3);
    go = s == 2 || s == 4 || s == 5 || s == 9;
    target = s == 2 ? 2 : (s == 4 ? 2 : (s == 5 ? 1 : 0));
  };
  constexpr int steps = 14;
  const auto run = [&](SemRig &rig) {
    std::vector<float> values;
    for (int step = 0; step < steps; step++) {
      for (uint32_t owner = 0; owner < owners; owner++) {
        bool go;
        int target;
        schedule(owner, step, go, target);
        KX_GameObject &object = rig.Object(owner);
        rig.SetBool(object, "go", go);
        rig.SetInt(object, "target", target);
        for (const char *name : {"in0", "in1", "in2", "entered"}) {
          rig.SetFloat(object, name, 0.0f);
        }
      }
      /* (A Frame Update before each step: the rig's frame clock moves on frames too, so Time in
       * State shows it counts the steps' time, Float::GameTime.) */
      rig.Frame();
      for (uint32_t owner = 0; owner < owners; owner++) {
        for (const char *name : {"in0", "in1", "in2", "entered", "cur", "prev", "time"}) {
          values.push_back(rig.Float(rig.Object(owner), name, kNaN));
        }
      }
    }
    return values;
  };
  SemRig rig(program, kProduction, owners);
  const std::vector<float> vm = run(rig);
  /* The model: the state after each step. */
  for (uint32_t owner = 0; owner < owners; owner++) {
    int state = 1, previous = 1;
    int entered_step = -1;
    for (int step = 0; step < steps; step++) {
      bool go;
      int target;
      schedule(owner, step, go, target);
      const float *v = &vm[(size_t(step) * owners + owner) * 7];
      const bool changes = go && target != state;
      EXPECT_EQ(v[3], changes ? 1.0f : 0.0f) << "Entered, owner " << owner << " step " << step;
      if (changes) {
        previous = state;
        state = target;
        entered_step = step;
      }
      if (!go) {
        /* (A step without Go To: Update in the state it is in.) */
        EXPECT_EQ(v[0], state == 0 ? 1.0f : 0.0f) << "owner " << owner << " step " << step;
        EXPECT_EQ(v[1], state == 1 ? 1.0f : 0.0f) << "owner " << owner << " step " << step;
        EXPECT_EQ(v[2], state == 2 ? 1.0f : 0.0f) << "owner " << owner << " step " << step;
      }
      if (!go) {
        /* (A step with Go To writes them before or after it: the step's order of routes.) */
        EXPECT_EQ(v[4], float(state)) << "Current State, owner " << owner << " step " << step;
        EXPECT_EQ(v[5], float(previous)) << "Previous State, owner " << owner << " step " << step;
        if (entered_step >= 0) {
          /* (The tick's elapsed time minus the one where it entered.) */
          EXPECT_NEAR(v[6], float(step - entered_step) * float(SemRig::kStep), 1.0e-4f)
              << "Time in State, owner " << owner << " step " << step;
        }
      }
      EXPECT_GE(v[6], 0.0f);
    }
  }
  /* Time in State grows a step's time a step. */
  {
    const size_t a = (size_t(10) * owners + 0) * 7 + 6, b = (size_t(11) * owners + 0) * 7 + 6;
    EXPECT_NEAR(vm[b] - vm[a], float(SemRig::kStep), 1.0e-5f);
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  const std::vector<float> columns = run(wide);
  ASSERT_EQ(columns.size(), vm.size());
  for (size_t k = 0; k < vm.size(); k++) {
    ASSERT_TRUE(SameFloat(columns[k], vm[k])) << "a wide run's value " << k;
  }
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the NPCs ran wide: " << wide.LastColumnRefusal();
  }
}

/** \} */

