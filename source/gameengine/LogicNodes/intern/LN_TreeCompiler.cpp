/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler.cpp
 *  \ingroup logicnodes
 */

#include "LN_TreeCompiler.h"
#include "LN_TreeCompiler_internal.hh"
#include "LN_FormulaEval.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"
#include "SCA_JoystickSensor.h"

#include "BL_Action.h"

#include "BKE_colortools.hh"
#include "DNA_color_types.h"

namespace ln_compiler {

LoopFrameCache *g_active_loop_frame_cache = nullptr;

struct TopologyResult {
  std::vector<const blender::bNode *> order;
  const blender::bNode *cycle_node = nullptr;
};

struct FlowPath {
  LN_Event event = LN_Event::OnFixedUpdate;
  uint32_t bool_guard_expr_index = LN_INVALID_INDEX;
  uint32_t loop_frame_index = LN_INVALID_INDEX;
};

struct FlowEventsResult {
  std::vector<FlowPath> paths;
  bool valid = true;
};

using FlowEventsCache = std::unordered_map<const blender::bNodeSocket *, FlowEventsResult>;

uint32_t AppendFlowInstruction(LN_Program &program,
                               const FlowPath &path,
                               LN_Instruction instruction)
{
  const LN_Event event = path.event;
  instruction.bool_guard_expr_index = path.bool_guard_expr_index;
  instruction.loop_frame_index = path.loop_frame_index;
  return program.AddInstruction(event, instruction);
}

uint32_t CombineBoolExpressionsWithOr(LN_Program &program, const std::vector<uint32_t> &expressions)
{
  if (expressions.empty()) {
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::Constant;
    expression.bool_value = false;
    return program.AddBoolExpression(expression);
  }
  uint32_t result = expressions.front();
  for (size_t index = 1; index < expressions.size(); index++) {
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::Or;
    expression.input0 = result;
    expression.input1 = expressions[index];
    result = program.AddBoolExpression(expression);
  }
  return result;
}

static const char *ValueTypeDiagnosticName(const LN_ValueType type)
{
  switch (type) {
    case LN_ValueType::None:
      return "none";
    case LN_ValueType::Bool:
      return "boolean";
    case LN_ValueType::Int:
      return "integer";
    case LN_ValueType::Float:
      return "float";
    case LN_ValueType::Vector:
      return "vector";
    case LN_ValueType::Vector4:
      return "vector4";
    case LN_ValueType::Matrix:
      return "matrix";
    case LN_ValueType::Color:
      return "color";
    case LN_ValueType::Rotation:
      return "rotation";
    case LN_ValueType::String:
      return "string";
    case LN_ValueType::ObjectRef:
      return "object";
    case LN_ValueType::SceneRef:
      return "scene";
    case LN_ValueType::CollectionRef:
      return "collection";
    case LN_ValueType::DatablockRef:
      return "datablock";
    case LN_ValueType::List:
      return "list";
    case LN_ValueType::Dict:
      return "dictionary";
    case LN_ValueType::Generic:
      return "generic";
  }
  return "unknown";
}

static std::string LinkTypeMismatchDiagnostic(const LN_PinDefinition &from_pin,
                                              const LN_PinDefinition &to_pin)
{
  if (to_pin.kind == LN_PinKind::Execution && from_pin.kind != LN_PinKind::Execution) {
    return "Execution input '" + to_pin.name +
           "' requires an execution output; boolean data should drive Branch, Gate, or an "
           "explicit pulse conversion node";
  }

  if (from_pin.kind == LN_PinKind::Execution && to_pin.kind != LN_PinKind::Execution) {
    if (to_pin.kind == LN_PinKind::Condition || to_pin.value_type == LN_ValueType::Bool) {
      return "Execution output cannot connect to boolean data input '" + to_pin.name +
             "'; connect execution to 'Flow' and boolean data to 'Condition'";
    }
    return "Execution output cannot connect to data input '" + to_pin.name + "'";
  }

  return "Data link type mismatch: " + std::string(ValueTypeDiagnosticName(from_pin.value_type)) +
         " output cannot connect to " + ValueTypeDiagnosticName(to_pin.value_type) + " input '" +
         to_pin.name + "'";
}

static std::string RequiredInputSocketMissingDiagnostic(const LN_PinDefinition &input)
{
  if (input.kind == LN_PinKind::Execution) {
    return "Required execution input '" + input.name +
           "' socket is missing; execution inputs should be named 'Flow'";
  }
  if (input.kind == LN_PinKind::Condition || input.value_type == LN_ValueType::Bool) {
    return "Required input socket is missing: boolean data input '" + input.name +
           "' should be a Condition-style data socket";
  }
  return "Required input socket is missing: " +
         std::string(ValueTypeDiagnosticName(input.value_type)) + " data input '" + input.name +
         "'";
}

static std::string RequiredInputLinkMissingDiagnostic(const LN_PinDefinition &input)
{
  if (input.kind == LN_PinKind::Execution) {
    return "Required execution input '" + input.name +
           "' is not linked; connect an execution output to 'Flow'";
  }
  if (input.kind == LN_PinKind::Condition || input.value_type == LN_ValueType::Bool) {
    return "Required boolean data input '" + input.name +
           "' is not linked; connect boolean data to 'Condition'";
  }
  return "Required " + std::string(ValueTypeDiagnosticName(input.value_type)) + " data input '" +
         input.name + "' is not linked";
}

static std::string SocketTypeMismatchDiagnostic(const LN_PinDefinition &input,
                                                const blender::bNodeSocket &socket)
{
  const std::string actual = socket.idname[0] == '\0' ? "<untyped>" : socket.idname;
  const std::string expected = input.socket_idname.empty() ? "<untyped>" : input.socket_idname;
  return "Socket '" + input.name + "' has type '" + actual +
         "' but native definition requires '" + expected + "'";
}

bool VisitNode(
    const blender::bNode &node,
    const EdgeMap &edges,
    std::unordered_map<const blender::bNode *, uint8_t> &visit_state,
    TopologyResult &result)
{
  const auto state_iter = visit_state.find(&node);
  if (state_iter != visit_state.end()) {
    if (state_iter->second == 1) {
      result.cycle_node = &node;
      return false;
    }
    if (state_iter->second == 2) {
      return true;
    }
  }

  visit_state[&node] = 1;
  const auto edges_iter = edges.find(&node);
  if (edges_iter != edges.end()) {
    for (const blender::bNode *target : edges_iter->second) {
      if (target != nullptr && !VisitNode(*target, edges, visit_state, result)) {
        return false;
      }
    }
  }
  visit_state[&node] = 2;
  result.order.push_back(&node);
  return true;
}

std::optional<uint32_t> EnsureLoopFrame(
    LN_Program &program,
    const blender::bNode &loop_node,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    IntExpressionCache &int_expression_cache,
    ValueExpressionCache &value_expression_cache,
    LoopFrameCache &cache)
{
  if (const auto existing = cache.indices.find(&loop_node); existing != cache.indices.end()) {
    return existing->second;
  }

  LN_LoopFrame frame;
  const std::optional<uint32_t> condition =
      BuildPrimaryExecutionExpression(program,
                                                        loop_node,
                                                        definition,
                                                        node_definitions,
                                                        input_links,
                                                        value_cache,
                                                        float_expression_cache,
                                                        bool_expression_cache);
  if (condition) {
    frame.trigger_bool_expr_index = *condition;
  }

  if (definition.kind == LN_NodeKind::LoopFromList) {
    frame.kind = LN_LoopKind::FromList;
    StringExpressionCache string_expression_cache;
    VectorExpressionCache vector_expression_cache;
    ColorExpressionCache color_expression_cache;
    const std::optional<uint32_t> list_expr = BuildInputValueExpression(program,
                                                                          loop_node,
                                                                          "List",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          bool_expression_cache,
                                                                          int_expression_cache,
                                                                          float_expression_cache,
                                                                          string_expression_cache,
                                                                          vector_expression_cache,
                                                                          color_expression_cache,
                                                                          value_expression_cache);
    if (list_expr) {
      frame.list_value_expr_index = *list_expr;
    }
  }
  else {
    frame.kind = LN_LoopKind::Count;
    const std::optional<uint32_t> count_expr = BuildInputIntExpression(program,
                                                                       loop_node,
                                                                       "Count",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       int_expression_cache,
                                                                       &bool_expression_cache,
                                                                       &float_expression_cache,
                                                                       nullptr,
                                                                       nullptr,
                                                                       nullptr,
                                                                       nullptr);
    if (count_expr) {
      frame.count_int_expr_index = *count_expr;
    }
  }

  const uint32_t frame_index = program.AddLoopFrame(frame);

  LN_BoolExpression loop_active;
  loop_active.kind = LN_BoolExpressionKind::LoopActive;
  loop_active.int_value = int32_t(frame_index);
  frame.loop_active_bool_expr_index = program.AddBoolExpression(loop_active);

  LN_IntExpression loop_index;
  loop_index.kind = LN_IntExpressionKind::LoopIndex;
  loop_index.int_value = int32_t(frame_index);
  frame.loop_index_int_expr_index = program.AddIntExpression(loop_index);

  LN_ValueExpression current_value;
  current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
  current_value.property_ref_index = frame_index;
  frame.loop_current_value_expr_index = program.AddValueExpression(current_value);

  program.UpdateLoopFrame(frame_index, frame);
  cache.indices[&loop_node] = frame_index;

  if (const blender::bNodeSocket *loop_out = FindOutputSocket(loop_node, "Loop")) {
    bool_expression_cache[loop_out] = frame.loop_active_bool_expr_index;
  }
  cache.index_expressions[&loop_node] = frame.loop_index_int_expr_index;
  cache.value_expressions[&loop_node] = frame.loop_current_value_expr_index;

  return frame_index;
}

std::optional<uint32_t> GetLoopFrameIndexForNode(
    LN_Program &program,
    const blender::bNode &loop_node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    IntExpressionCache &int_expression_cache,
    ValueExpressionCache &value_expression_cache,
    LoopFrameCache &cache)
{
  const auto definition_iter = node_definitions.find(&loop_node);
  if (definition_iter == node_definitions.end()) {
    return std::nullopt;
  }
  return EnsureLoopFrame(program,
                         loop_node,
                         *definition_iter->second,
                         node_definitions,
                         input_links,
                         value_cache,
                         float_expression_cache,
                         bool_expression_cache,
                         int_expression_cache,
                         value_expression_cache,
                         cache);
}

FlowEventsResult ResolveFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache);

static FlowEventsResult ResolvePrimaryExecutionInputFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &node,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache)
{
  FlowEventsResult result;
  result.valid = false;

  const LN_PinDefinition *flow_pin = FindFirstExecutionInputPin(definition);
  if (flow_pin == nullptr) {
    return result;
  }

  result = ResolveFlowEvents(program,
                             tree,
                             node,
                             flow_pin->name,
                             node_definitions,
                             input_links,
                             value_cache,
                             float_expression_cache,
                             bool_expression_cache,
                             flow_events_cache,
                             loop_frame_cache);
  return result;
}

static FlowEventsResult BuildConditionRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &source_node,
    const blender::bNodeSocket &route_socket,
    const FlowEventsResult &source_flow,
    const uint32_t condition_expr,
    const bool condition_value,
    BoolExpressionCache *route_bool_cache,
    FlowEventsCache &flow_events_cache)
{
  FlowEventsResult result;
  result.valid = false;

  if (!source_flow.valid || source_flow.paths.empty()) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  result.valid = true;
  std::vector<uint32_t> route_expressions;
  const uint32_t source_ref_index = AddSourceRef(program, tree, source_node, route_socket.name);
  for (const FlowPath &source_path : source_flow.paths) {
    LN_Instruction route_instruction;
    route_instruction.opcode = LN_OpCode::BranchRoute;
    route_instruction.source_ref_index = source_ref_index;
    route_instruction.bool_expr_index = condition_expr;
    route_instruction.bool_value = condition_value;
    const uint32_t instruction_index = AppendFlowInstruction(program, source_path, route_instruction);

    LN_BoolExpression route_pulse;
    route_pulse.kind = LN_BoolExpressionKind::InstructionExecuted;
    route_pulse.input0 = instruction_index;
    const uint32_t route_expr_index = program.AddBoolExpression(route_pulse);
    route_expressions.push_back(route_expr_index);

    FlowPath route_path;
    route_path.event = source_path.event;
    route_path.bool_guard_expr_index = route_expr_index;
    route_path.loop_frame_index = source_path.loop_frame_index;
    result.paths.push_back(route_path);
  }

  if (route_bool_cache != nullptr) {
    (*route_bool_cache)[&route_socket] = CombineBoolExpressionsWithOr(program, route_expressions);
  }
  flow_events_cache[&route_socket] = result;
  return result;
}

static FlowEventsResult BuildBranchRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &branch_node,
    const blender::bNodeSocket &route_socket,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache)
{
  if (const auto cached = flow_events_cache.find(&route_socket); cached != flow_events_cache.end())
  {
    return cached->second;
  }

  FlowEventsResult result;
  result.valid = false;

  const bool wants_true = NamesMatch(route_socket.name, route_socket.identifier, "True");
  const bool wants_false = NamesMatch(route_socket.name, route_socket.identifier, "False");
  if (!wants_true && !wants_false) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  const std::optional<uint32_t> condition = BuildInputBoolExpression(program,
                                                                    branch_node,
                                                                    "Condition",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    float_expression_cache,
                                                                    bool_expression_cache);
  if (!condition) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  FlowEventsResult input_flow = ResolvePrimaryExecutionInputFlowEvents(program,
                                                                       tree,
                                                                       branch_node,
                                                                       definition,
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       bool_expression_cache,
                                                                       flow_events_cache,
                                                                       loop_frame_cache);
  if (!input_flow.valid) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  return BuildConditionRouteFlowEvents(program,
                                       tree,
                                       branch_node,
                                       route_socket,
                                       input_flow,
                                       *condition,
                                       wants_true,
                                       &bool_expression_cache,
                                       flow_events_cache);
}

static FlowEventsResult BuildFixedUpdateRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &source_node,
    const blender::bNodeSocket &route_socket,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache)
{
  if (const auto cached = flow_events_cache.find(&route_socket); cached != flow_events_cache.end())
  {
    return cached->second;
  }

  FlowEventsResult result;
  result.valid = false;

  if (!IsExecutionOutputSocket(definition, route_socket)) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  const std::optional<uint32_t> condition = BuildOutputBoolExpression(program,
                                                                      source_node,
                                                                      route_socket,
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
  if (!condition) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  FlowPath source_path;
  source_path.event = LN_Event::OnFixedUpdate;
  FlowEventsResult source_flow;
  source_flow.paths.push_back(source_path);

  return BuildConditionRouteFlowEvents(program,
                                       tree,
                                       source_node,
                                       route_socket,
                                       source_flow,
                                       *condition,
                                       true,
                                       nullptr,
                                       flow_events_cache);
}

static FlowEventsResult BuildFlowConditionRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &source_node,
    const blender::bNodeSocket &route_socket,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache)
{
  if (const auto cached = flow_events_cache.find(&route_socket); cached != flow_events_cache.end())
  {
    return cached->second;
  }

  FlowEventsResult result;
  result.valid = false;

  if (!IsExecutionOutputSocket(definition, route_socket)) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  const std::optional<uint32_t> condition = BuildOutputBoolExpression(program,
                                                                      source_node,
                                                                      route_socket,
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
  if (!condition) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  FlowEventsResult source_flow = ResolvePrimaryExecutionInputFlowEvents(program,
                                                                        tree,
                                                                        source_node,
                                                                        definition,
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        float_expression_cache,
                                                                        bool_expression_cache,
                                                                        flow_events_cache,
                                                                        loop_frame_cache);
  if (!source_flow.valid) {
    source_flow.valid = true;
    source_flow.paths.clear();
    FlowPath fallback_path;
    fallback_path.event = LN_Event::OnFixedUpdate;
    source_flow.paths.push_back(fallback_path);
  }

  return BuildConditionRouteFlowEvents(program,
                                       tree,
                                       source_node,
                                       route_socket,
                                       source_flow,
                                       *condition,
                                       true,
                                       nullptr,
                                       flow_events_cache);
}

static FlowEventsResult BuildLatentCompletionRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &source_node,
    const blender::bNodeSocket &route_socket,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache)
{
  if (const auto cached = flow_events_cache.find(&route_socket); cached != flow_events_cache.end())
  {
    return cached->second;
  }

  FlowEventsResult result;
  result.valid = false;

  if (!IsExecutionOutputSocket(definition, route_socket)) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  const std::optional<uint32_t> condition = BuildOutputBoolExpression(program,
                                                                      source_node,
                                                                      route_socket,
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
  if (!condition) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  FlowPath source_path;
  source_path.event = LN_Event::OnFixedUpdate;
  FlowEventsResult source_flow;
  source_flow.paths.push_back(source_path);

  return BuildConditionRouteFlowEvents(program,
                                       tree,
                                       source_node,
                                       route_socket,
                                       source_flow,
                                       *condition,
                                       true,
                                       nullptr,
                                       flow_events_cache);
}

static FlowEventsResult BuildCommandContinuationRouteFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &source_node,
    const blender::bNodeSocket &route_socket,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache)
{
  if (const auto cached = flow_events_cache.find(&route_socket); cached != flow_events_cache.end())
  {
    return cached->second;
  }

  FlowEventsResult result;
  result.valid = false;

  const bool is_continuation =
      NamesMatch(route_socket.name, route_socket.identifier, "Done") ||
      NamesMatch(route_socket.name, route_socket.identifier, "On Start") ||
      NamesMatch(route_socket.name, route_socket.identifier, "When Done") ||
      NamesMatch(route_socket.name, route_socket.identifier, "When Reached");
  if (!IsExecutionOutputSocket(definition, route_socket) || !is_continuation) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  FlowEventsResult source_flow = ResolvePrimaryExecutionInputFlowEvents(program,
                                                                        tree,
                                                                        source_node,
                                                                        definition,
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        float_expression_cache,
                                                                        bool_expression_cache,
                                                                        flow_events_cache,
                                                                        loop_frame_cache);
  if (!source_flow.valid) {
    flow_events_cache[&route_socket] = result;
    return result;
  }

  std::optional<uint32_t> condition = BuildOutputBoolExpression(program,
                                                                source_node,
                                                                route_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache,
                                                                float_expression_cache,
                                                                bool_expression_cache);
  if (!condition) {
    if (NamesMatch(route_socket.name, route_socket.identifier, "When Reached")) {
      flow_events_cache[&route_socket] = result;
      return result;
    }
    condition = AddConstantBoolExpression(program, true);
  }

  return BuildConditionRouteFlowEvents(program,
                                       tree,
                                       source_node,
                                       route_socket,
                                       source_flow,
                                       *condition,
                                       true,
                                       nullptr,
                                       flow_events_cache);
}

FlowEventsResult ResolveFlowEvents(
    LN_Program &program,
    const blender::bNodeTree &tree,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    FlowEventsCache &flow_events_cache,
    LoopFrameCache &loop_frame_cache)
{
  FlowEventsResult result;
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    result.valid = false;
    return result;
  }

  const auto target_definition_iter = node_definitions.find(&node);
  if (target_definition_iter != node_definitions.end()) {
    const LN_PinDefinition *input_pin = FindPinDefinition(target_definition_iter->second->inputs,
                                                          *socket);
    if (input_pin == nullptr || input_pin->kind != LN_PinKind::Execution) {
      result.valid = false;
      return result;
    }
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter == input_links.end() || links_iter->second.empty()) {
    result.valid = false;
    return result;
  }

  const ResolvedLink &link = links_iter->second.front();
  const auto definition_iter = node_definitions.find(link.fromnode);
  if (definition_iter == node_definitions.end()) {
    result.valid = false;
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
  const InternalCompileHandler *handler = FindInternalCompileHandler(definition.kind);
  if (handler == nullptr) {
    result.valid = false;
    return result;
  }

  switch (handler->flow_kind) {
    case FlowResolverKind::EventSource: {
      if (!IsExecutionOutputSocket(definition, *link.fromsock)) {
        result.valid = false;
        break;
      }

      FlowPath path;
      path.event = handler->source_event;
      result.paths.push_back(path);
      break;
    }
    case FlowResolverKind::Branch: {
      if (!IsExecutionOutputSocket(definition, *link.fromsock)) {
        result.valid = false;
        break;
      }

      result = BuildBranchRouteFlowEvents(program,
                                          tree,
                                          *link.fromnode,
                                          *link.fromsock,
                                          definition,
                                          node_definitions,
                                          input_links,
                                          value_cache,
                                          float_expression_cache,
                                          bool_expression_cache,
                                          flow_events_cache,
                                          loop_frame_cache);
      break;
    }
    case FlowResolverKind::FlowConditionRoute: {
      result = BuildFlowConditionRouteFlowEvents(program,
                                                 tree,
                                                 *link.fromnode,
                                                 *link.fromsock,
                                                 definition,
                                                 node_definitions,
                                                 input_links,
                                                 value_cache,
                                                 float_expression_cache,
                                                 bool_expression_cache,
                                                 flow_events_cache,
                                                 loop_frame_cache);
      break;
    }
    case FlowResolverKind::LatentCompletionRoute: {
      result = BuildLatentCompletionRouteFlowEvents(program,
                                                    tree,
                                                    *link.fromnode,
                                                    *link.fromsock,
                                                    definition,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache,
                                                    float_expression_cache,
                                                    bool_expression_cache,
                                                    flow_events_cache);
      break;
    }
    case FlowResolverKind::PollingEventRoute: {
      result = BuildFixedUpdateRouteFlowEvents(program,
                                               tree,
                                               *link.fromnode,
                                               *link.fromsock,
                                               definition,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               float_expression_cache,
                                               bool_expression_cache,
                                               flow_events_cache);
      break;
    }
    case FlowResolverKind::PrecompiledRoute: {
      const auto cached = flow_events_cache.find(link.fromsock);
      if (cached != flow_events_cache.end()) {
        result = cached->second;
      }
      else {
        result.valid = false;
      }
      break;
    }
    case FlowResolverKind::Loop: {
      if (!IsExecutionOutputSocket(definition, *link.fromsock)) {
        result.valid = false;
        break;
      }
      if (!NamesMatch(link.fromsock->name, link.fromsock->identifier, "Loop")) {
        result.valid = false;
        break;
      }

      IntExpressionCache int_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> loop_frame_index = GetLoopFrameIndexForNode(
          program,
          *link.fromnode,
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          bool_expression_cache,
          int_expression_cache,
          value_expression_cache,
          loop_frame_cache);
      if (!loop_frame_index) {
        result.valid = false;
        break;
      }

      const std::optional<uint32_t> loop_guard = BuildOutputBoolExpression(program,
                                                                           *link.fromnode,
                                                                           *link.fromsock,
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           float_expression_cache,
                                                                           bool_expression_cache);
      if (!loop_guard) {
        result.valid = false;
        break;
      }

      result = ResolvePrimaryExecutionInputFlowEvents(program,
                                                      tree,
                                                      *link.fromnode,
                                                      definition,
                                                      node_definitions,
                                                      input_links,
                                                      value_cache,
                                                      float_expression_cache,
                                                      bool_expression_cache,
                                                      flow_events_cache,
                                                      loop_frame_cache);
      if (!result.valid) {
        break;
      }

      for (FlowPath &path : result.paths) {
        /* The incoming flow is consumed once by LN_LoopFrame::trigger_bool_expr_index in the
         * outer event scope.  Rechecking a scoped route pulse inside an iteration would always
         * fail because loop iterations intentionally use distinct execution scopes. */
        path.bool_guard_expr_index = *loop_guard;
        path.loop_frame_index = *loop_frame_index;
      }
      break;
    }
    case FlowResolverKind::Invalid: {
      const blender::bNodeSocket *fromsock = link.fromsock;
      if (handler->descriptor.info.emits_commands && fromsock != nullptr) {
        result = BuildCommandContinuationRouteFlowEvents(program,
                                                         tree,
                                                         *link.fromnode,
                                                         *fromsock,
                                                         definition,
                                                         node_definitions,
                                                         input_links,
                                                         value_cache,
                                                         float_expression_cache,
                                                         bool_expression_cache,
                                                         flow_events_cache,
                                                         loop_frame_cache);
      }
      else {
        result.valid = false;
      }
      break;
    }
  }

  return result;
}

struct CompilerContext {
  const blender::bNodeTree &tree;
  const blender::bNode &node;
  const LN_NodeDefinition &definition;
  LN_Program &program;
  const NodeDefinitionMap &node_definitions;
  const ActiveNodeSet &active_nodes;
  const InputLinkMap &input_links;
  ValueCache &value_cache;
  BoolExpressionCache &bool_expression_cache;
  FlowEventsCache &flow_events_cache;
  IntExpressionCache &int_expression_cache;
  FloatExpressionCache &float_expression_cache;
  StringExpressionCache &string_expression_cache;
  VectorExpressionCache &vector_expression_cache;
  ColorExpressionCache &color_expression_cache;
  ValueExpressionCache &value_expression_cache;
  bool &has_event_node;
  LoopFrameCache &loop_frame_cache;

  bool OutputIsLinked(const char *pin_name) const
  {
    const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name);
    if (socket == nullptr) {
      return false;
    }
    for (const auto &item : input_links) {
      for (const ResolvedLink &link : item.second) {
        if (link.fromnode == &node && link.fromsock == socket) {
          return true;
        }
      }
    }
    return false;
  }

  bool OutputFeedsActiveNode(const char *pin_name) const
  {
    const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name);
    if (socket == nullptr) {
      return false;
    }
    for (const auto &item : input_links) {
      for (const ResolvedLink &link : item.second) {
        if (link.fromnode == &node && link.fromsock == socket &&
            IsNodeActive(active_nodes, link.tonode))
        {
          return true;
        }
      }
    }
    return false;
  }

  bool InputIsLinked(const char *pin_name) const
  {
    const blender::bNodeSocket *socket = FindInputSocket(node, pin_name);
    if (socket == nullptr) {
      return false;
    }
    const auto links_iter = input_links.find(socket);
    return links_iter != input_links.end() && !links_iter->second.empty();
  }

  std::optional<LN_Value> ReadValue(const char *socket_name, LN_ValueType expected_type)
  {
    return ReadInputValue(
        node, socket_name, expected_type, node_definitions, input_links, value_cache);
  }

  std::optional<uint32_t> BuildBool(const char *socket_name)
  {
    return BuildInputBoolExpression(program,
                                    node,
                                    socket_name,
                                    node_definitions,
                                    input_links,
                                    value_cache,
                                    float_expression_cache,
                                    bool_expression_cache);
  }

  std::optional<uint32_t> BuildExecution(const char *socket_name)
  {
    return BuildInputExecutionExpression(program,
                                         node,
                                         socket_name,
                                         node_definitions,
                                         input_links,
                                         value_cache,
                                         float_expression_cache,
                                         bool_expression_cache);
  }

  const LN_PinDefinition *PrimaryExecutionInputPin() const
  {
    return FindFirstExecutionInputPin(definition);
  }

  std::optional<uint32_t> BuildPrimaryExecution()
  {
    return BuildPrimaryExecutionExpression(program,
                                           node,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_expression_cache);
  }

  std::optional<uint32_t> BuildInt(const char *socket_name)
  {
    return BuildInputIntExpression(program,
                                   node,
                                   socket_name,
                                   node_definitions,
                                   input_links,
                                   value_cache,
                                   int_expression_cache,
                                   &bool_expression_cache,
                                   &float_expression_cache,
                                   &string_expression_cache,
                                   &vector_expression_cache,
                                   &color_expression_cache,
                                   &value_expression_cache);
  }

  std::optional<uint32_t> BuildFloat(const char *socket_name)
  {
    return BuildInputFloatExpression(program,
                                     node,
                                     socket_name,
                                     node_definitions,
                                     input_links,
                                     value_cache,
                                     float_expression_cache,
                                     &bool_expression_cache,
                                     &vector_expression_cache);
  }

  std::optional<uint32_t> BuildString(const char *socket_name)
  {
    return BuildInputStringExpression(program,
                                      node,
                                      socket_name,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      string_expression_cache);
  }

  std::optional<uint32_t> BuildVector(const char *socket_name)
  {
    return BuildInputVectorExpression(program,
                                      node,
                                      socket_name,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      vector_expression_cache);
  }

  std::optional<uint32_t> BuildRotation(const char *socket_name)
  {
    return BuildInputRotationVectorExpression(program,
                                              node,
                                              socket_name,
                                              node_definitions,
                                              input_links,
                                              value_cache,
                                              float_expression_cache,
                                              vector_expression_cache);
  }

  std::optional<uint32_t> BuildColor(const char *socket_name)
  {
    return BuildInputColorExpression(program,
                                     node,
                                     socket_name,
                                     node_definitions,
                                     input_links,
                                     value_cache,
                                     float_expression_cache,
                                     color_expression_cache);
  }

  std::optional<uint32_t> BuildValueExpression(const char *socket_name)
  {
    return BuildInputValueExpression(program,
                                     node,
                                     socket_name,
                                     node_definitions,
                                     input_links,
                                     value_cache,
                                     bool_expression_cache,
                                     int_expression_cache,
                                     float_expression_cache,
                                     string_expression_cache,
                                     vector_expression_cache,
                                     color_expression_cache,
                                     value_expression_cache);
  }

  void CacheBoolOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      bool_expression_cache[socket] = expr_index;
    }
  }

  void CacheIntOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      int_expression_cache[socket] = expr_index;
    }
  }

  void CacheFloatOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      float_expression_cache[socket] = expr_index;
    }
  }

  void CacheStringOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      string_expression_cache[socket] = expr_index;
    }
  }

  void CacheValueOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      value_expression_cache[socket] = expr_index;
    }
  }

  void CacheVectorOutputPin(const char *pin_name, uint32_t expr_index)
  {
    if (const blender::bNodeSocket *socket = FindOutputSocket(node, pin_name)) {
      vector_expression_cache[socket] = expr_index;
    }
  }

  std::optional<uint32_t> BuildOptionalObject(const char *socket_name)
  {
    return BuildOptionalObjectTargetExpression(program,
                                               node,
                                               socket_name,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               bool_expression_cache,
                                               int_expression_cache,
                                               float_expression_cache,
                                               string_expression_cache,
                                               vector_expression_cache,
                                               color_expression_cache,
                                               value_expression_cache);
  }

  std::optional<uint32_t> BuildCameraOrActive(const char *socket_name)
  {
    return BuildInputOrActiveCameraExpression(program,
                                              node,
                                              socket_name,
                                              node_definitions,
                                              input_links,
                                              value_cache,
                                              bool_expression_cache,
                                              int_expression_cache,
                                              float_expression_cache,
                                              string_expression_cache,
                                              vector_expression_cache,
                                              color_expression_cache,
                                              value_expression_cache);
  }

  std::optional<FlowEventsResult> ResolveRequiredFlow(const char *socket_name,
                                                      const std::string &error_message)
  {
    FlowEventsResult flow_events = ResolveFlowEvents(program,
                                                     tree,
                                                     node,
                                                     socket_name,
                                                     node_definitions,
                                                     input_links,
                                                     value_cache,
                                                     float_expression_cache,
                                                     bool_expression_cache,
                                                     flow_events_cache,
                                                     loop_frame_cache);
    if (!flow_events.valid) {
      AddNodeIssue(program, tree, node, socket_name, LN_CompileSeverity::Error, error_message);
      return std::nullopt;
    }
    return flow_events;
  }

  std::optional<FlowEventsResult> ResolveOptionalFlow(const char *socket_name)
  {
    FlowEventsResult flow_events = ResolveFlowEvents(program,
                                                     tree,
                                                     node,
                                                     socket_name,
                                                     node_definitions,
                                                     input_links,
                                                     value_cache,
                                                     float_expression_cache,
                                                     bool_expression_cache,
                                                     flow_events_cache,
                                                     loop_frame_cache);
    if (!flow_events.valid) {
      return std::nullopt;
    }
    return flow_events;
  }

  std::optional<FlowEventsResult> ResolveRequiredPrimaryFlow(const std::string &error_message)
  {
    const LN_PinDefinition *flow_pin = PrimaryExecutionInputPin();
    if (flow_pin == nullptr) {
      AddNodeIssue(program, tree, node, nullptr, LN_CompileSeverity::Error, error_message);
      return std::nullopt;
    }

    FlowEventsResult flow_events = ResolveFlowEvents(program,
                                                     tree,
                                                     node,
                                                     flow_pin->name,
                                                     node_definitions,
                                                     input_links,
                                                     value_cache,
                                                     float_expression_cache,
                                                     bool_expression_cache,
                                                     flow_events_cache,
                                                     loop_frame_cache);
    if (flow_events.valid) {
      return flow_events;
    }

    AddNodeIssue(program,
                 tree,
                 node,
                 flow_pin->name.c_str(),
                 LN_CompileSeverity::Error,
                 error_message);
    return std::nullopt;
  }

  uint32_t MakeSourceRef(const char *socket_name) const
  {
    return AddSourceRef(program, tree, node, socket_name);
  }

  void AddError(const char *socket_name, const std::string &message) const
  {
    AddNodeIssue(program, tree, node, socket_name, LN_CompileSeverity::Error, message);
  }

  void AddWarning(const char *socket_name, const std::string &message) const
  {
    AddNodeIssue(program, tree, node, socket_name, LN_CompileSeverity::Warning, message);
  }
};

std::optional<std::string> ReadRequiredConstantString(CompilerContext &context,
                                                      const char *socket_name,
                                                      const std::string &error_message)
{
  const std::optional<LN_Value> value = context.ReadValue(socket_name, LN_ValueType::String);
  if (!value || value->type != LN_ValueType::String || value->string_value.empty()) {
    context.AddError(socket_name, error_message);
    return std::nullopt;
  }
  return value->string_value;
}

std::optional<std::string> ReadRequiredConstantStringWithFallback(CompilerContext &context,
                                                                  const char *primary_socket,
                                                                  const char *fallback_socket,
                                                                  const std::string &error_message)
{
  const std::optional<LN_Value> primary = context.ReadValue(primary_socket, LN_ValueType::String);
  if (primary && primary->type == LN_ValueType::String && !primary->string_value.empty()) {
    return primary->string_value;
  }
  return ReadRequiredConstantString(context, fallback_socket, error_message);
}

static bool InputHasLink(const blender::bNode &node,
                         const char *socket_name,
                         const InputLinkMap &input_links)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return false;
  }
  const auto links_iter = input_links.find(socket);
  return links_iter != input_links.end() && !links_iter->second.empty();
}

static bool OutputFeedsActiveNode(const blender::bNode &node,
                                  const InputLinkMap &input_links,
                                  const ActiveNodeSet &active_nodes)
{
  for (const auto &item : input_links) {
    for (const ResolvedLink &link : item.second) {
      if (link.fromnode == &node && IsNodeActive(active_nodes, link.tonode)) {
        return true;
      }
    }
  }
  return false;
}

static void ValidateEventInputs(LN_Program &program,
                                const blender::bNodeTree &tree,
                                const std::vector<const blender::bNode *> &nodes,
                                const NodeDefinitionMap &node_definitions,
                                const InputLinkMap &input_links,
                                const ActiveNodeSet &active_nodes)
{
  ValueCache value_cache;
  for (const blender::bNode *node : nodes) {
    if (!IsNodeActive(active_nodes, node)) {
      continue;
    }

    const auto definition_iter = node_definitions.find(node);
    if (definition_iter == node_definitions.end()) {
      continue;
    }

    const LN_NodeKind kind = definition_iter->second->kind;
    if (kind != LN_NodeKind::SendEvent && kind != LN_NodeKind::ReceiveEvent) {
      continue;
    }

    if (kind == LN_NodeKind::ReceiveEvent &&
        !OutputFeedsActiveNode(*node, input_links, active_nodes))
    {
      continue;
    }

    const char *node_name = kind == LN_NodeKind::SendEvent ? "Send Event" : "Receive Event";
    const std::optional<LN_Value> subject = ReadInputValue(
        *node, "Subject", LN_ValueType::String, node_definitions, input_links, value_cache);
    if (subject && subject->type == LN_ValueType::String && subject->string_value.empty()) {
      AddNodeIssue(program,
                   tree,
                   *node,
                   "Subject",
                   LN_CompileSeverity::Warning,
                   std::string(node_name) +
                       " has an empty Subject and will not send or receive events. Empty Subject "
                       "is not a wildcard; use a matching non-empty Subject for event delivery.");
    }

    const bool use_target = kind == LN_NodeKind::SendEvent ? node->custom2 != 0 :
                                                            node->custom1 != 0;
    if (!use_target) {
      continue;
    }

    const std::optional<LN_Value> target = ReadInputValue(
        *node, "Target", LN_ValueType::ObjectRef, node_definitions, input_links, value_cache);
    const bool has_static_target =
        target && target->type == LN_ValueType::ObjectRef && target->exists;
    const bool has_dynamic_target = !target && InputHasLink(*node, "Target", input_links);
    if (!has_static_target && !has_dynamic_target) {
      AddNodeIssue(program,
                   tree,
                   *node,
                   "Target",
                   LN_CompileSeverity::Warning,
                   std::string(node_name) +
                       " target mode has no Target object and will not send or receive events; "
                       "turn Use Target off for broadcast.");
    }
  }
}

const char *EditorNodeValueSocketIdentifier(int value_type);
constexpr int set_material_node_value_per_object_only = 1 << 0;
constexpr int set_material_node_value_string_type = 5;
constexpr int set_material_node_value_material_type = 6;
constexpr int set_object_attribute_use_x = 1 << 0;
constexpr int set_object_attribute_use_y = 1 << 1;
constexpr int set_object_attribute_use_z = 1 << 2;
constexpr int modify_property_clamp = 1 << 0;
constexpr int modify_property_mode_attribute = 1 << 1;

static const blender::bNodeSocket *FindInputSocketByIdentifier(const blender::bNode &node,
                                                               const char *identifier)
{
  for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
           node.inputs.first);
       socket;
       socket = socket->next)
  {
    if (std::strcmp(socket->identifier, identifier) == 0) {
      return socket;
    }
  }
  return nullptr;
}

static bool InputSocketIdentifierIsLinked(const blender::bNode &node,
                                          const char *identifier,
                                          const InputLinkMap &input_links)
{
  const blender::bNodeSocket *socket = FindInputSocketByIdentifier(node, identifier);
  if (socket == nullptr) {
    return false;
  }
  const auto links_iter = input_links.find(socket);
  return links_iter != input_links.end() && !links_iter->second.empty();
}

static const char *SetObjectAttributeName(const int attribute_type)
{
  switch (attribute_type) {
    case 0:
      return "worldPosition";
    case 1:
      return "worldOrientation";
    case 2:
      return "worldLinearVelocity";
    case 3:
      return "worldAngularVelocity";
    case 5:
      return "worldScale";
    case 6:
      return "localPosition";
    case 7:
      return "localOrientation";
    case 8:
      return "localLinearVelocity";
    case 9:
      return "localAngularVelocity";
    default:
      return nullptr;
  }
}

static bool SetObjectAttributeUsesRotationInput(const int attribute_type)
{
  return attribute_type == 1 || attribute_type == 7;
}

static bool SetObjectAttributeUsesFullDefaultMask(const CompilerContext &context)
{
  constexpr int full_mask = set_object_attribute_use_x | set_object_attribute_use_y |
                            set_object_attribute_use_z;
  return !context.InputIsLinked("XYZ") && (context.node.custom2 & full_mask) == full_mask;
}

static bool SetObjectAttributeUsesTransformInput(const int attribute_type)
{
  return attribute_type == 4 || attribute_type == 10;
}

static std::optional<LN_OpCode> SetObjectAttributeOpcode(const int attribute_type)
{
  switch (attribute_type) {
    case 0:
      return LN_OpCode::SetWorldPosition;
    case 1:
      return LN_OpCode::SetWorldOrientation;
    case 2:
      return LN_OpCode::SetLinearVelocity;
    case 3:
      return LN_OpCode::SetAngularVelocity;
    case 5:
      return LN_OpCode::SetWorldScale;
    case 6:
      return LN_OpCode::SetLocalPosition;
    case 7:
      return LN_OpCode::SetLocalOrientation;
    case 8:
      return LN_OpCode::SetLocalLinearVelocity;
    case 9:
      return LN_OpCode::SetLocalAngularVelocity;
    case 11:
      return LN_OpCode::SetObjectColor;
    case 12:
      return LN_OpCode::SetVisibility;
    default:
      return std::nullopt;
  }
}

static void ConfigureVectorCommandInstruction(LN_Instruction &instruction,
                                              const LN_OpCode opcode)
{
  instruction.opcode = opcode;
  switch (opcode) {
    case LN_OpCode::SetWorldPosition:
    case LN_OpCode::SetLocalPosition:
      instruction.opcode = LN_OpCode::SetTransformVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
      instruction.vector_operation_mode = uint8_t(opcode == LN_OpCode::SetLocalPosition ?
                                                      LN_VectorOperationMode::Local :
                                                      LN_VectorOperationMode::World);
      break;
    case LN_OpCode::SetWorldOrientation:
    case LN_OpCode::SetLocalOrientation:
      instruction.opcode = LN_OpCode::SetTransformVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Orientation);
      instruction.vector_operation_mode = uint8_t(opcode == LN_OpCode::SetLocalOrientation ?
                                                      LN_VectorOperationMode::Local :
                                                      LN_VectorOperationMode::World);
      break;
    case LN_OpCode::SetWorldScale:
    case LN_OpCode::SetLocalScale:
      instruction.opcode = LN_OpCode::SetTransformVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Scale);
      instruction.vector_operation_mode = uint8_t(opcode == LN_OpCode::SetLocalScale ?
                                                      LN_VectorOperationMode::Local :
                                                      LN_VectorOperationMode::World);
      break;
    case LN_OpCode::SetLinearVelocity:
    case LN_OpCode::SetLocalLinearVelocity:
      instruction.opcode = LN_OpCode::SetVelocityVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::LinearVelocity);
      instruction.vector_operation_mode = uint8_t(opcode == LN_OpCode::SetLocalLinearVelocity ?
                                                      LN_VectorOperationMode::Local :
                                                      LN_VectorOperationMode::World);
      break;
    case LN_OpCode::SetAngularVelocity:
    case LN_OpCode::SetLocalAngularVelocity:
      instruction.opcode = LN_OpCode::SetVelocityVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::AngularVelocity);
      instruction.vector_operation_mode = uint8_t(opcode == LN_OpCode::SetLocalAngularVelocity ?
                                                      LN_VectorOperationMode::Local :
                                                      LN_VectorOperationMode::World);
      break;
    case LN_OpCode::ApplyMovement:
      instruction.opcode = LN_OpCode::ApplyTransformVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Movement);
      instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
      break;
    case LN_OpCode::ApplyRotation:
      instruction.opcode = LN_OpCode::ApplyTransformVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Rotation);
      instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
      break;
    case LN_OpCode::ApplyForce:
      instruction.opcode = LN_OpCode::ApplyPhysicsVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Force);
      instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
      break;
    case LN_OpCode::ApplyTorque:
      instruction.opcode = LN_OpCode::ApplyPhysicsVector;
      instruction.vector_operation_channel = uint8_t(LN_VectorOperationChannel::Torque);
      instruction.vector_operation_mode = uint8_t(LN_VectorOperationMode::LocalFromBool);
      break;
    default:
      break;
  }
}

static uint32_t AddBinaryFloatExpression(LN_Program &program,
                                         const LN_FloatExpressionKind kind,
                                         const uint32_t input0,
                                         const uint32_t input1)
{
  LN_FloatExpression expression;
  expression.kind = kind;
  expression.input0 = input0;
  expression.input1 = input1;
  return program.AddFloatExpression(expression);
}

static uint32_t AddColorComponentFloatExpression(LN_Program &program,
                                                 const uint32_t color_expression_index,
                                                 const uint8_t component_index)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::ColorComponent;
  expression.input0 = color_expression_index;
  expression.component_index = component_index;
  return program.AddFloatExpression(expression);
}

static uint32_t AddMaskedFloatExpression(LN_Program &program,
                                         const uint32_t current_expr,
                                         const uint32_t value_expr,
                                         const uint32_t mask_expr)
{
  const uint32_t delta_expr = AddBinaryFloatExpression(
      program, LN_FloatExpressionKind::Subtract, value_expr, current_expr);
  const uint32_t scaled_delta_expr = AddBinaryFloatExpression(
      program, LN_FloatExpressionKind::Multiply, delta_expr, mask_expr);
  return AddBinaryFloatExpression(program,
                                  LN_FloatExpressionKind::Add,
                                  current_expr,
                                  scaled_delta_expr);
}

static uint32_t AddBinaryVectorExpression(LN_Program &program,
                                          const LN_VectorExpressionKind kind,
                                          const uint32_t input0,
                                          const uint32_t input1)
{
  LN_VectorExpression expression;
  expression.kind = kind;
  expression.input0 = input0;
  expression.input1 = input1;
  return program.AddVectorExpression(expression);
}

static uint32_t AddMaskedVectorExpression(LN_Program &program,
                                          const uint32_t current_expr,
                                          const uint32_t value_expr,
                                          const uint32_t mask_expr)
{
  const uint32_t delta_expr = AddBinaryVectorExpression(
      program, LN_VectorExpressionKind::Subtract, value_expr, current_expr);
  const uint32_t scaled_delta_expr = AddBinaryVectorExpression(
      program, LN_VectorExpressionKind::Multiply, delta_expr, mask_expr);
  return AddBinaryVectorExpression(program,
                                   LN_VectorExpressionKind::Add,
                                   current_expr,
                                   scaled_delta_expr);
}

static uint32_t AddObjectAttributeValueExpression(LN_Program &program,
                                                  const std::optional<uint32_t> &object_expr,
                                                  const char *attribute_name)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ObjectAttribute;
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.input1 = AddConstantStringExpression(program, attribute_name);
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildSetObjectAttributeVectorExpression(
    CompilerContext &context, const std::optional<uint32_t> &object_expr)
{
  const char *attribute_name = SetObjectAttributeName(context.node.custom1);
  if (attribute_name == nullptr) {
    return std::nullopt;
  }

  const bool use_rotation_input = SetObjectAttributeUsesRotationInput(context.node.custom1);
  const char *value_socket_name = use_rotation_input ? "Rotation" : "Value";
  const std::optional<uint32_t> value_expr = use_rotation_input ?
                                                 context.BuildRotation(value_socket_name) :
                                                 context.BuildVector(value_socket_name);
  if (!value_expr) {
    context.AddError(value_socket_name,
                     use_rotation_input ? "Set Attribute requires a rotation value input" :
                                          "Set Attribute requires a vector value input");
    return std::nullopt;
  }

  if (SetObjectAttributeUsesFullDefaultMask(context)) {
    return value_expr;
  }

  const std::optional<uint32_t> mask_expr = context.BuildVector("XYZ");
  if (!mask_expr) {
    context.AddError("XYZ", "Set Attribute requires an XYZ mask input");
    return std::nullopt;
  }

  LN_VectorExpression current_value_expression;
  current_value_expression.kind = LN_VectorExpressionKind::FromGenericValue;
  current_value_expression.input0 = AddObjectAttributeValueExpression(
      context.program, object_expr, attribute_name);
  const uint32_t current_expr = context.program.AddVectorExpression(current_value_expression);
  return AddMaskedVectorExpression(context.program, current_expr, *value_expr, *mask_expr);
}

static std::optional<uint32_t> BuildMaskedSetObjectAttributeColorExpression(
    CompilerContext &context, const std::optional<uint32_t> &object_expr)
{
  const std::optional<uint32_t> color_expr = context.BuildColor("Color");
  if (!color_expr) {
    context.AddError("Color", "Set Attribute requires a color value input");
    return std::nullopt;
  }

  const std::optional<uint32_t> mask_expr = context.BuildVector("XYZ");
  if (!mask_expr) {
    context.AddError("XYZ", "Set Attribute requires an XYZ mask input");
    return std::nullopt;
  }

  LN_ColorExpression current_color_expression;
  current_color_expression.kind = LN_ColorExpressionKind::SnapshotObjectColor;
  if (object_expr) {
    current_color_expression.input0 = *object_expr;
  }
  const uint32_t current_color_expr = context.program.AddColorExpression(current_color_expression);

  const uint32_t current_r = AddColorComponentFloatExpression(context.program, current_color_expr, 0);
  const uint32_t current_g = AddColorComponentFloatExpression(context.program, current_color_expr, 1);
  const uint32_t current_b = AddColorComponentFloatExpression(context.program, current_color_expr, 2);
  const uint32_t current_a = AddColorComponentFloatExpression(context.program, current_color_expr, 3);
  const uint32_t input_r = AddColorComponentFloatExpression(context.program, *color_expr, 0);
  const uint32_t input_g = AddColorComponentFloatExpression(context.program, *color_expr, 1);
  const uint32_t input_b = AddColorComponentFloatExpression(context.program, *color_expr, 2);
  const uint32_t mask_x = AddVectorComponentFloatExpression(context.program, *mask_expr, 0);
  const uint32_t mask_y = AddVectorComponentFloatExpression(context.program, *mask_expr, 1);
  const uint32_t mask_z = AddVectorComponentFloatExpression(context.program, *mask_expr, 2);

  LN_ColorExpression merged_color_expression;
  merged_color_expression.kind = LN_ColorExpressionKind::Combine;
  merged_color_expression.input0 = AddMaskedFloatExpression(context.program, current_r, input_r, mask_x);
  merged_color_expression.input1 = AddMaskedFloatExpression(context.program, current_g, input_g, mask_y);
  merged_color_expression.input2 = AddMaskedFloatExpression(context.program, current_b, input_b, mask_z);
  merged_color_expression.input3 = current_a;
  return context.program.AddColorExpression(merged_color_expression);
}

static void AppendSetObjectAttributeVectorInstruction(CompilerContext &context,
                                                      const FlowPath &path,
                                                      const std::optional<uint32_t> &object_expr,
                                                      const LN_OpCode opcode,
                                                      const char *socket_name,
                                                      const uint32_t vector_expr,
                                                      const MT_Vector3 &fallback)
{
  LN_Instruction instruction;
  ConfigureVectorCommandInstruction(instruction, opcode);
  instruction.source_ref_index = context.MakeSourceRef(socket_name);
  if (object_expr) {
    instruction.value_expr_index = *object_expr;
  }
  instruction.vector_expr_index = vector_expr;
  instruction.vector_value = fallback;
  AppendFlowInstruction(context.program, path, instruction);
}

std::optional<FlowEventsResult> ResolveCommandFlow(CompilerContext &context,
                                                   const char *flow_socket,
                                                   const char *flow_error)
{
  if (std::strcmp(flow_socket, "Flow") == 0) {
    return context.ResolveRequiredPrimaryFlow(flow_error);
  }
  return context.ResolveRequiredFlow(flow_socket, flow_error);
}

void CompileVectorCommand(CompilerContext &context,
                          LN_OpCode opcode,
                          const char *value_socket,
                          const char *object_socket,
                          const char *flow_socket,
                          const char *input_error,
                          const char *flow_error,
                          bool use_rotation_input)
{
  const std::optional<uint32_t> value_expr = use_rotation_input ?
                                                 context.BuildRotation(value_socket) :
                                                 context.BuildVector(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  const MT_Vector3 fallback = VectorExpressionConstantFallback(context.program, *value_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    ConfigureVectorCommandInstruction(instruction, opcode);
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *value_expr;
    instruction.vector_value = fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileVectorBoolCommand(CompilerContext &context,
                              LN_OpCode opcode,
                              const char *value_socket,
                              const char *bool_socket,
                              const char *object_socket,
                              const char *flow_socket,
                              const char *input_error,
                              const char *bool_error,
                              const char *flow_error,
                              bool use_rotation_input)
{
  const std::optional<uint32_t> value_expr = use_rotation_input ?
                                                 context.BuildRotation(value_socket) :
                                                 context.BuildVector(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> bool_expr = context.BuildBool(bool_socket);
  if (!bool_expr) {
    context.AddError(bool_socket, bool_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  const MT_Vector3 fallback = VectorExpressionConstantFallback(context.program, *value_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    ConfigureVectorCommandInstruction(instruction, opcode);
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *value_expr;
    instruction.vector_value = fallback;
    instruction.bool_expr_index = *bool_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileBoolCommand(CompilerContext &context,
                        LN_OpCode opcode,
                        const char *value_socket,
                        const char *object_socket,
                        const char *flow_socket,
                        const char *input_error,
                        const char *flow_error)
{
  const std::optional<uint32_t> value_expr = context.BuildBool(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.bool_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileBoolPairCommand(CompilerContext &context,
                            LN_OpCode opcode,
                            const char *value_socket,
                            const char *secondary_socket,
                            const char *object_socket,
                            const char *flow_socket,
                            const char *input_error,
                            const char *secondary_error,
                            const char *flow_error)
{
  const std::optional<uint32_t> value_expr = context.BuildBool(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> secondary_expr = context.BuildBool(secondary_socket);
  if (!secondary_expr) {
    context.AddError(secondary_socket, secondary_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.bool_expr_index = *value_expr;
    instruction.secondary_bool_expr_index = *secondary_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileIntCommand(CompilerContext &context,
                       LN_OpCode opcode,
                       const char *value_socket,
                       const char *object_socket,
                       const char *flow_socket,
                       const char *input_error,
                       const char *flow_error)
{
  const std::optional<uint32_t> value_expr = context.BuildInt(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.int_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileColorCommand(CompilerContext &context,
                         LN_OpCode opcode,
                         const char *value_socket,
                         const char *object_socket,
                         const char *flow_socket,
                         const char *input_error,
                         const char *flow_error)
{
  const std::optional<uint32_t> value_expr = context.BuildColor(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, input_error);
    return;
  }

  const std::optional<uint32_t> object_expr =
      object_socket ? context.BuildOptionalObject(object_socket) : std::optional<uint32_t>();
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  const MT_Vector4 fallback = ColorExpressionConstantFallback(context.program, *value_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.color_expr_index = *value_expr;
    instruction.color_value = fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileOptionalObjectCommand(CompilerContext &context,
                                  LN_OpCode opcode,
                                  const char *source_socket,
                                  const char *object_socket,
                                  const char *flow_socket,
                                  const char *flow_error)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject(object_socket);
  if (!object_expr && IsInputSocketLinked(context.node, object_socket, context.input_links)) {
    context.AddError(object_socket, std::string(object_socket) + " input link did not compile");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, flow_socket, flow_error);
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(source_socket);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileCameraFloatCommand(CompilerContext &context,
                               LN_OpCode opcode,
                               const char *value_socket,
                               const char *camera_error,
                               const char *value_error,
                               const char *flow_error)
{
  const std::optional<uint32_t> camera_expr = context.BuildCameraOrActive("Camera");
  if (!camera_expr) {
    context.AddError("Camera", camera_error);
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildFloat(value_socket);
  if (!value_expr) {
    context.AddError(value_socket, value_error);
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(flow_error);
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    instruction.value_expr_index = *camera_expr;
    instruction.float_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

#define DEFINE_VECTOR_COMMAND_HANDLER(name, opcode, value_socket, object_socket, flow_socket, \
                                      input_error, flow_error, use_rotation)                 \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileVectorCommand(context,                                                             \
                         LN_OpCode::opcode,                                                   \
                         value_socket,                                                        \
                         object_socket,                                                       \
                         flow_socket,                                                         \
                         input_error,                                                         \
                         flow_error,                                                          \
                         use_rotation);                                                       \
  }

#define DEFINE_VECTOR_BOOL_COMMAND_HANDLER(name, opcode, value_socket, bool_socket,           \
                                           object_socket, flow_socket, input_error,           \
                                           bool_error, flow_error, use_rotation)              \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileVectorBoolCommand(context,                                                         \
                             LN_OpCode::opcode,                                               \
                             value_socket,                                                    \
                             bool_socket,                                                     \
                             object_socket,                                                   \
                             flow_socket,                                                     \
                             input_error,                                                     \
                             bool_error,                                                      \
                             flow_error,                                                      \
                             use_rotation);                                                   \
  }

#define DEFINE_BOOL_COMMAND_HANDLER(name, opcode, value_socket, object_socket, flow_socket,   \
                                    input_error, flow_error)                                  \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileBoolCommand(context,                                                               \
                       LN_OpCode::opcode,                                                     \
                       value_socket,                                                          \
                       object_socket,                                                         \
                       flow_socket,                                                           \
                       input_error,                                                           \
                       flow_error);                                                           \
  }

#define DEFINE_BOOL_PAIR_COMMAND_HANDLER(name, opcode, value_socket, secondary_socket,         \
                                         object_socket, flow_socket, input_error,             \
                                         secondary_error, flow_error)                         \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileBoolPairCommand(context,                                                           \
                           LN_OpCode::opcode,                                                 \
                           value_socket,                                                      \
                           secondary_socket,                                                  \
                           object_socket,                                                     \
                           flow_socket,                                                       \
                           input_error,                                                       \
                           secondary_error,                                                   \
                           flow_error);                                                       \
  }

#define DEFINE_INT_COMMAND_HANDLER(name, opcode, value_socket, object_socket, flow_socket,    \
                                   input_error, flow_error)                                   \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileIntCommand(context,                                                                \
                      LN_OpCode::opcode,                                                      \
                      value_socket,                                                           \
                      object_socket,                                                          \
                      flow_socket,                                                            \
                      input_error,                                                            \
                      flow_error);                                                            \
  }

#define DEFINE_COLOR_COMMAND_HANDLER(name, opcode, value_socket, object_socket, flow_socket,  \
                                     input_error, flow_error)                                 \
  void name(CompilerContext &context)                                                         \
  {                                                                                           \
    CompileColorCommand(context,                                                              \
                        LN_OpCode::opcode,                                                    \
                        value_socket,                                                         \
                        object_socket,                                                        \
                        flow_socket,                                                          \
                        input_error,                                                          \
                        flow_error);                                                          \
  }

DEFINE_VECTOR_COMMAND_HANDLER(CompileSetGravityNode,
                              SetGravity,
                              "Gravity",
                              nullptr,
                              "Flow",
                              "Set Gravity requires a vector gravity input",
                              "Set Gravity flow must be driven by an event or branch",
                              false)

DEFINE_VECTOR_BOOL_COMMAND_HANDLER(CompileApplyMovementNode,
                                   ApplyMovement,
                                   "Vector",
                                   "Local",
                                   "Object",
                                   "Flow",
                                   "Apply Movement requires a vector movement input",
                                   "Apply Movement requires a boolean local input",
                                   "Apply Movement flow must be driven by an event or branch",
                                   false)
DEFINE_VECTOR_BOOL_COMMAND_HANDLER(CompileApplyRotationNode,
                                   ApplyRotation,
                                   "Rotation",
                                   "Local",
                                   "Object",
                                   "Flow",
                                   "Apply Rotation requires a rotation input",
                                   "Apply Rotation requires a boolean local input",
                                   "Apply Rotation flow must be driven by an event or branch",
                                   true)
DEFINE_VECTOR_BOOL_COMMAND_HANDLER(CompileApplyForceNode,
                                   ApplyForce,
                                   "Force",
                                   "Local",
                                   "Object",
                                   "Flow",
                                   "Apply Force requires a vector force input",
                                   "Apply Force requires a boolean local input",
                                   "Apply Force flow must be driven by an event or branch",
                                   false)
DEFINE_VECTOR_BOOL_COMMAND_HANDLER(CompileApplyTorqueNode,
                                   ApplyTorque,
                                   "Torque",
                                   "Local",
                                   "Object",
                                   "Flow",
                                   "Apply Torque requires a vector torque input",
                                   "Apply Torque requires a boolean local input",
                                   "Apply Torque flow must be driven by an event or branch",
                                   false)

DEFINE_BOOL_COMMAND_HANDLER(CompileSetFullscreenNode,
                            SetFullscreen,
                            "Fullscreen",
                            nullptr,
                            "Flow",
                            "Set Fullscreen requires a boolean fullscreen input",
                            "Set Fullscreen flow must be driven by an event or branch")
DEFINE_BOOL_COMMAND_HANDLER(CompileShowFramerateNode,
                            SetShowFramerate,
                            "Show",
                            nullptr,
                            "Flow",
                            "Show Framerate requires a boolean show input",
                            "Show Framerate flow must be driven by an event or branch")
DEFINE_BOOL_COMMAND_HANDLER(CompileShowProfileNode,
                            SetShowProfile,
                            "Show",
                            nullptr,
                            "Flow",
                            "Show Profile requires a boolean show input",
                            "Show Profile flow must be driven by an event or branch")
DEFINE_BOOL_COMMAND_HANDLER(CompileSetLightShadowNode,
                            SetLightShadow,
                            "Use Shadow",
                            "Object",
                            "Flow",
                            "Set Light Shadow requires a boolean shadow input",
                            "Set Light Shadow flow must be driven by an event or branch")
DEFINE_BOOL_COMMAND_HANDLER(CompileSetCursorVisibilityNode,
                            SetCursorVisibility,
                            "Visible",
                            nullptr,
                            "Flow",
                            "Set Cursor Visibility requires a boolean visible input",
                            "Set Cursor Visibility flow must be driven by an event or branch")

DEFINE_BOOL_COMMAND_HANDLER(CompileSetPhysicsNode,
                            SetPhysics,
                            "Active",
                            "Object",
                            "Flow",
                            "Enable Physics Body requires a boolean enabled input",
                            "Enable Physics Body flow must be driven by an event or branch")

void CompileSetDynamicsNode(CompilerContext &context)
{
  if (context.node.custom1 < 0 || context.node.custom1 > 3) {
    context.AddError("Mode", "Set Dynamics has an unsupported dynamics mode");
    return;
  }

  constexpr int ghost_mode = 2;
  uint32_t enabled_expr = LN_INVALID_INDEX;
  bool enabled = true;
  if (context.node.custom1 == ghost_mode) {
    const std::optional<uint32_t> expr = context.BuildBool("Enabled");
    if (!expr) {
      context.AddError("Enabled", "Set Dynamics Ghost mode requires a boolean enabled input");
      return;
    }
    enabled_expr = *expr;
    enabled = BoolExpressionConstantFallback(context.program, *expr);
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Dynamics flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetDynamics;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.bool_guard_expr_index = path.bool_guard_expr_index;
    instruction.int_value = context.node.custom1;
    instruction.bool_expr_index = enabled_expr;
    instruction.bool_value = enabled;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution()) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRebuildCollisionShapeNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Rebuild Collision Shape flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RebuildCollisionShape;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.bool_guard_expr_index = path.bool_guard_expr_index;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution()) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

DEFINE_INT_COMMAND_HANDLER(CompileSetCollisionGroupNode,
                           SetCollisionGroup,
                           "Group",
                           "Object",
                           "Flow",
                           "Set Collision Layers requires a collision layers input",
                           "Set Collision Layers flow must be driven by an event or branch")

DEFINE_COLOR_COMMAND_HANDLER(CompileSetLightColorNode,
                             SetLightColor,
                             "Color",
                             "Object",
                             "Flow",
                             "Set Light Color requires a color input",
                             "Set Light Color flow must be driven by an event or branch")

static std::optional<uint32_t> WrapTypedInputAsValue(
    CompilerContext &context,
    const LN_ValueExpressionKind kind,
    const std::optional<uint32_t> typed_expr)
{
  if (!typed_expr) {
    return std::nullopt;
  }
  LN_ValueExpression value_expr;
  value_expr.kind = kind;
  value_expr.input0 = *typed_expr;
  return context.program.AddValueExpression(value_expr);
}

static std::optional<uint32_t> BuildEditorNodeValueExpression(CompilerContext &context)
{
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  if (InputSocketIdentifierIsLinked(context.node, value_socket, context.input_links)) {
    return context.BuildValueExpression(value_socket);
  }
  if (context.node.custom1 != 7 &&
      InputSocketIdentifierIsLinked(context.node, "Value", context.input_links)) {
    return context.BuildValueExpression("Value");
  }

  switch (context.node.custom1) {
    case 1:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromInt,
          context.BuildInt(EditorNodeValueSocketIdentifier(context.node.custom1)));
    case 2:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromBool,
          context.BuildBool(EditorNodeValueSocketIdentifier(context.node.custom1)));
    case 3:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromVector,
          context.BuildVector(EditorNodeValueSocketIdentifier(context.node.custom1)));
    case 4:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromColor,
          context.BuildColor(EditorNodeValueSocketIdentifier(context.node.custom1)));
    case 5:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromString,
          context.BuildString(EditorNodeValueSocketIdentifier(context.node.custom1)));
    case 6:
    case 7:
      return context.BuildValueExpression(EditorNodeValueSocketIdentifier(context.node.custom1));
    default:
      return WrapTypedInputAsValue(
          context,
          LN_ValueExpressionKind::FromFloat,
          context.BuildFloat(EditorNodeValueSocketIdentifier(context.node.custom1)));
  }
}

void CompileMaterialEditorNodeValue(CompilerContext &context,
                                    const char *node_label,
                                    const LN_OpCode opcode)
{
  using blender::ID_Type;
  const bool per_object_only =
      (context.node.custom2 & set_material_node_value_per_object_only) != 0;
  std::optional<uint32_t> object_expr;
  std::optional<uint32_t> material_expr;
  std::optional<uint32_t> slot_expr;

  if (per_object_only) {
    object_expr = context.BuildOptionalObject("Object");
    slot_expr = context.BuildInt("Slot");
    if (!slot_expr) {
      context.AddError("Slot", std::string(node_label) + " requires a slot in per-object mode");
      return;
    }
  }
  else {
    const bool material_linked = context.InputIsLinked("Material");
    const std::optional<LN_Value> material_default = context.ReadValue("Material",
                                                                       LN_ValueType::DatablockRef);
    if (material_linked || (material_default && material_default->exists)) {
      material_expr = context.BuildValueExpression("Material");
    }
    if (!material_expr && context.node.id != nullptr &&
        GS(context.node.id->name) == blender::ID_MA)
    {
      LN_Value material_value;
      material_value.type = LN_ValueType::DatablockRef;
      material_value.exists = true;
      material_value.reference_name = context.node.id->name + 2;
      material_expr = AddConstantValueExpression(context.program, material_value);
    }
    if (!material_expr) {
      context.AddWarning("Material",
                         std::string(node_label) +
                             " skipped: no Material is selected or connected");
      return;
    }
  }

  const std::optional<uint32_t> node_name_expr = context.BuildString("Node Name");
  const std::optional<uint32_t> socket_name_expr = context.BuildString("Socket");
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  const std::optional<uint32_t> value_expr = BuildEditorNodeValueExpression(context);
  if (!node_name_expr || !socket_name_expr || !value_expr) {
    context.AddError(value_socket,
                     std::string(node_label) + " requires node name, socket name, and value");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(std::string(node_label) +
                                         " flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (slot_expr) {
      instruction.int_expr_index = *slot_expr;
    }
    if (per_object_only && opcode == LN_OpCode::SetMaterialNodeSocketValue) {
      /* The universal node leaves ownership explicit: Make Node Tree Unique is the only
       * operation that copies and reassigns the slot material. */
      instruction.secondary_int_value = 1;
    }
    instruction.string_expr_index = *node_name_expr;
    instruction.secondary_string_expr_index = *socket_name_expr;
    instruction.secondary_value_expr_index = *value_expr;
    if (material_expr) {
      instruction.tertiary_value_expr_index = *material_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetMaterialParameterNode(CompilerContext &context)
{
  if (context.node.custom1 == set_material_node_value_string_type ||
      context.node.custom1 == set_material_node_value_material_type)
  {
    context.AddError("Value",
                     "Set Material Parameter supports only boolean, integer, float, vector, "
                     "rotation, and color sockets; use Set Editor Node Value for string or "
                     "material socket defaults");
    return;
  }
  CompileMaterialEditorNodeValue(
      context, "Set Material Parameter", LN_OpCode::SetMaterialParameter);
}

void CompileMakeLightUniqueNode(CompilerContext &context)
{
  CompileOptionalObjectCommand(context,
                               LN_OpCode::MakeLightUnique,
                               "Object",
                               "Object",
                               "Flow",
                               "Make Light Unique flow must be driven by an event or branch");
}

void CompileRemoveParentNode(CompilerContext &context)
{
  CompileOptionalObjectCommand(context,
                               LN_OpCode::RemoveParent,
                               "Child Object",
                               "Child Object",
                               "Flow",
                               "Remove Parent flow must be driven by an event or branch");
}

void CompileRemoveObjectNode(CompilerContext &context)
{
  CompileOptionalObjectCommand(context,
                               LN_OpCode::RemoveObject,
                               "Object",
                               "Object",
                               "Flow",
                               "Remove Object flow must be driven by an event or branch");
}

void CompileSetTimescaleNode(CompilerContext &context);
void CompileSetCameraNode(CompilerContext &context);
void CompileSetCameraFovNode(CompilerContext &context);
void CompileSetCameraOrthoScaleNode(CompilerContext &context);
void CompileSetLightPowerNode(CompilerContext &context);
void CompileSetResolutionNode(CompilerContext &context);
void CompileSetVSyncNode(CompilerContext &context);
void CompileTogglePropertyNode(CompilerContext &context);
void CompileModifyPropertyNode(CompilerContext &context);
void CompileModifyPropertyClampedNode(CompilerContext &context);
void CompileAddObjectNode(CompilerContext &context);
void CompileSetParentNode(CompilerContext &context);
void CompileApplyImpulseNode(CompilerContext &context);
void CompileApplyForceToTargetNode(CompilerContext &context);
void CompileSetGamePropertyIntNode(CompilerContext &context);
void CompileSetGamePropertyFloatNode(CompilerContext &context);
void CompileSetGamePropertyBoolNode(CompilerContext &context);
void CompileSetGamePropertyStringNode(CompilerContext &context);
void CompileSetTreePropertyNode(CompilerContext &context);
void CompileSetCursorPositionNode(CompilerContext &context);
void CompileGamepadVibrationNode(CompilerContext &context);
void CompileGamepadLookNode(CompilerContext &context);
void CompileMouseLookNode(CompilerContext &context);
void CompileCharacterJumpNode(CompilerContext &context);
void CompileSetCharacterGravityNode(CompilerContext &context);
void CompileSetCharacterJumpSpeedNode(CompilerContext &context);
void CompileSetCharacterMaxJumpsNode(CompilerContext &context);
void CompileSetCharacterWalkDirectionNode(CompilerContext &context);
void CompileSetCharacterVelocityNode(CompilerContext &context);
void CompileVehicleControlNode(CompilerContext &context);
void CompileVehicleAccelerateNode(CompilerContext &context);
void CompileVehicleBrakeNode(CompilerContext &context);
void CompileVehicleSteerNode(CompilerContext &context);
void CompileVehicleSetAttributesNode(CompilerContext &context);
void CompilePrintNode(CompilerContext &context);
void CompileDataContainerNode(CompilerContext &context);
void CompileSnapshotTransformNode(CompilerContext &context);
void CompileExpressionOutputsNode(CompilerContext &context);
void CompileRaycastNode(CompilerContext &context);
void CompileLoopNode(CompilerContext &context);
void CompileOnceNode(CompilerContext &context);
void CompileBooleanEdgeNode(CompilerContext &context);
void CompileCooldownNode(CompilerContext &context);
void CompileDelayNode(CompilerContext &context);
void CompileTimerNode(CompilerContext &context);
void CompilePulsifyNode(CompilerContext &context);
void CompileBarrierNode(CompilerContext &context);

void CompileQuitGameNode(CompilerContext &context);
void CompileRestartGameNode(CompilerContext &context);
void CompileLoadBlendFileNode(CompilerContext &context);
void CompilePlayActionNode(CompilerContext &context);
void CompileStopActionNode(CompilerContext &context);
void CompileSetActionFrameNode(CompilerContext &context);
void CompileStopAllSoundsNode(CompilerContext &context);
void CompilePlaySoundNode(CompilerContext &context);
void CompilePlaySound3DNode(CompilerContext &context);
void CompileStartSpeakerNode(CompilerContext &context);
void CompilePauseSoundNode(CompilerContext &context);
void CompileResumeSoundNode(CompilerContext &context);
void CompileStopSoundNode(CompilerContext &context);
void CompileStartLogicTreeNode(CompilerContext &context);
void CompileStopLogicTreeNode(CompilerContext &context);
void CompileRunLogicTreeNode(CompilerContext &context);
void CompileInstallLogicTreeNode(CompilerContext &context);
void CompileMoveTowardNode(CompilerContext &context);
void CompileSlowFollowNode(CompilerContext &context);
void CompileLoadSceneNode(CompilerContext &context);
void CompileSetSceneNode(CompilerContext &context);
void CompileSaveGameNode(CompilerContext &context);
void CompileLoadGameNode(CompilerContext &context);
void CompileAlignAxisToVectorNode(CompilerContext &context);
void CompileRotateTowardNode(CompilerContext &context);
void CompileSetObjectAttributeNode(CompilerContext &context);
void CompileSetRigidBodyAttributeNode(CompilerContext &context);
void CompileRebuildCollisionShapeNode(CompilerContext &context);
void CompileReplaceMeshNode(CompilerContext &context);
void CompileCopyPropertyNode(CompilerContext &context);
void CompileSetBonePoseLocationNode(CompilerContext &context);
void CompileSetBonePoseRotationNode(CompilerContext &context);
void CompileSetBonePoseScaleNode(CompilerContext &context);
void CompileSetBonePoseTransformNode(CompilerContext &context);
void CompileSetBoneConstraintInfluenceNode(CompilerContext &context);
void CompileSetBoneAttributeNode(CompilerContext &context);
void CompileSetBoneConstraintTargetNode(CompilerContext &context);
void CompileSetBoneConstraintAttributeNode(CompilerContext &context);
void CompileSetGeometryNodesInputNode(CompilerContext &context);
void CompileSetEditorNodeValueNode(CompilerContext &context);
void CompileMakeNodeTreeUniqueNode(CompilerContext &context);
void CompileSetNodeMuteNode(CompilerContext &context);
void CompileEnableDisableModifierNode(CompilerContext &context);
void CompileAssignGeometryNodesModifierNode(CompilerContext &context);
void CompileDrawLineNode(CompilerContext &context);
void CompileDrawCubeNode(CompilerContext &context);
void CompileDrawBoxNode(CompilerContext &context);
void CompileDrawNode(CompilerContext &context);
void CompileUnsupportedCommandNode(CompilerContext &context);
void CompileSetMaterialSlotNode(CompilerContext &context);
void CompileGetObjectAttributeNode(CompilerContext &context);
void CompileToggleTreePropertyNode(CompilerContext &context);
void CompileEvaluatePropertyNode(CompilerContext &context);
void CompileTypecastNode(CompilerContext &context);
void CompileValueValidNode(CompilerContext &context);
void CompileValueSwitchListNode(CompilerContext &context);
void CompileValueSwitchListCompareNode(CompilerContext &context);
void CompileListRandomItemNode(CompilerContext &context);
void CompileGetChildByNameNode(CompilerContext &context);
void CompileListSavedVariablesNode(CompilerContext &context);
void CompileClearVariablesNode(CompilerContext &context);
void CompileRemoveVariableNode(CompilerContext &context);
void CompileSetOverlayCollectionNode(CompilerContext &context);
void CompileRemoveOverlayCollectionNode(CompilerContext &context);
void CompileRandomFloatNode(CompilerContext &context);
void CompileRandomIntNode(CompilerContext &context);
void CompileRandomVectorNode(CompilerContext &context);
void CompileFormulaNode(CompilerContext &context);
void CompileTweenValueNode(CompilerContext &context);
void CompileAddPhysicsConstraintNode(CompilerContext &context);
void CompileGetRigidBodyConstraintsNode(CompilerContext &context);
void CompileRemovePhysicsConstraintNode(CompilerContext &context);
void CompileProjectileRayNode(CompilerContext &context);
void CompileSpawnPoolNode(CompilerContext &context);
FlowPath GuardFlowPath(LN_Program &program, const FlowPath &path, uint32_t guard_expr);

static uint32_t BakeTweenCurveTable(CompilerContext &context)
{
  std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT> samples{};
  if (blender::CurveMapping *cumap = static_cast<blender::CurveMapping *>(context.node.storage)) {
    blender::BKE_curvemapping_init(cumap);
    for (int sample_index = 0; sample_index < LN_TWEEN_CURVE_SAMPLE_COUNT; sample_index++) {
      const float factor = float(sample_index) / float(LN_TWEEN_CURVE_SAMPLE_COUNT - 1);
      const float mapped_factor = blender::BKE_curvemapping_evaluateF(cumap, 0, factor);
      samples[size_t(sample_index)] = std::isfinite(mapped_factor) ? mapped_factor : factor;
    }
  }
  else {
    for (int sample_index = 0; sample_index < LN_TWEEN_CURVE_SAMPLE_COUNT; sample_index++) {
      samples[size_t(sample_index)] = float(sample_index) /
                                     float(LN_TWEEN_CURVE_SAMPLE_COUNT - 1);
    }
  }
  return context.program.AddTweenCurveTable(samples);
}

static std::string FormulaTextFromNode(const blender::bNode &node)
{
  static const char *predefined[] = {
      "a + b",
      "abs(a)",
      "acos(a)",
      "acosh(a)",
      "asin(a)",
      "asinh(a)",
      "atan(a)",
      "atan2(a,b)",
      "atanh(a)",
      "ceil(a)",
      "cos(a)",
      "cosh(a)",
      "curt(a)",
      "degrees(a)",
      "e",
      "exp(a)",
      "floor(a)",
      "hypot(a,b)",
      "log(a)",
      "log10(a)",
      "mod(a,b)",
      "pi",
      "pow(a,b)",
      "radians(a)",
      "sign(a)",
      "sin(a)",
      "sinh(a)",
      "sqrt(a)",
      "tan(a)",
      "tanh(a)",
  };
  if (node.custom1 == 0) {
    if (const char *formula = reinterpret_cast<const char *>(node.storage)) {
      return formula;
    }
    return "a + b";
  }
  const int index = node.custom1 - 1;
  if (index >= 0 && index < int(std::size(predefined))) {
    return predefined[index];
  }
  return "a + b";
}

}  // namespace ln_compiler

LN_TreeCompiler::LN_TreeCompiler(const LN_NodeRegistry &registry) : m_registry(registry) {}

namespace ln_compiler {

void CompileSetTimescaleNode(CompilerContext &context)
{
  const std::optional<uint32_t> timescale_expr = context.BuildFloat("Timescale");
  if (!timescale_expr) {
    context.AddError("Timescale", "Set Timescale requires a float timescale input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Timescale flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetTimeScale;
    instruction.source_ref_index = context.MakeSourceRef("Timescale");
    instruction.float_expr_index = *timescale_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileDataContainerNode(CompilerContext &context)
{
  switch (context.definition.kind) {
    case LN_NodeKind::FindObject: {
      if (const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object")) {
        context.CacheValueOutputPin("Object", *object_expr);
      }
      break;
    }
    case LN_NodeKind::ObjectByName: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ObjectByName;
      if (const std::optional<uint32_t> name_expr = context.BuildString("Name")) {
        expression.input0 = *name_expr;
      }
      context.CacheValueOutputPin("Object", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetOwner: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::OwnerObject;
      context.CacheValueOutputPin("Owner Object", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetGlobalProperty: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::GetGlobalProperty;
      if (const std::optional<uint32_t> category_expr = context.BuildString("Category")) {
        expression.input0 = *category_expr;
      }
      if (const std::optional<uint32_t> property_expr = context.BuildString("Property")) {
        expression.input1 = *property_expr;
      }
      if (const std::optional<uint32_t> default_expr = context.BuildValueExpression("Default Value")) {
        expression.input2 = *default_expr;
      }
      context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListGlobalProperties: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListGlobalProperties;
      if (const std::optional<uint32_t> category_expr = context.BuildString("Category")) {
        expression.input0 = *category_expr;
      }
      if (const std::optional<uint32_t> print_expr = context.BuildBool("Print")) {
        expression.bool_expr_index = *print_expr;
      }
      context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::LoadVariable: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::LoadVariable;
      if (const std::optional<uint32_t> path_expr = context.BuildString("Path")) {
        expression.input0 = *path_expr;
      }
      if (const std::optional<uint32_t> file_expr = context.BuildString("File")) {
        expression.input1 = *file_expr;
      }
      if (const std::optional<uint32_t> name_expr = context.BuildString("Name")) {
        expression.input2 = *name_expr;
      }
      if (const std::optional<uint32_t> default_expr = context.BuildValueExpression("Default Value")) {
        expression.input_indices.push_back(*default_expr);
      }
      context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::LoadVariableDict: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::LoadVariableDict;
      if (const std::optional<uint32_t> path_expr = context.BuildString("Path")) {
        expression.input0 = *path_expr;
      }
      if (const std::optional<uint32_t> file_expr = context.BuildString("File")) {
        expression.input1 = *file_expr;
      }
      context.CacheValueOutputPin("Variables", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetScene: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::CurrentScene;
      context.CacheValueOutputPin("Scene", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetCollection: {
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection")) {
        context.CacheValueOutputPin("Collection", *collection_expr);
      }
      break;
    }
    case LN_NodeKind::GetCollectionObjects: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::CollectionObjects;
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection")) {
        expression.input0 = *collection_expr;
      }
      context.CacheValueOutputPin("Objects", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetCollectionObjectNames: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::CollectionObjectNames;
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection")) {
        expression.input0 = *collection_expr;
      }
      context.CacheValueOutputPin("Object Names", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListAppend: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListAppend;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value")) {
        expression.input1 = *value_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListRemoveIndex: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListRemoveIndex;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> index_expr = context.BuildInt("Index")) {
        expression.input1 = *index_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListRemoveValue: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListRemoveValue;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value")) {
        expression.input1 = *value_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListSetIndex: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListSetIndex;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> index_expr = context.BuildInt("Index")) {
        expression.input1 = *index_expr;
      }
      if (const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value")) {
        expression.input2 = *value_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictSetKey: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::DictSetKey;
      if (const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary")) {
        expression.input0 = *dict_expr;
      }
      if (const std::optional<uint32_t> key_expr = context.BuildString("Key")) {
        expression.input1 = *key_expr;
      }
      if (const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value")) {
        expression.input2 = *value_expr;
      }
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictRemoveKey: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression dict_expression;
      dict_expression.kind = LN_ValueExpressionKind::DictRemoveKey;
      if (const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary")) {
        dict_expression.input0 = *dict_expr;
      }
      if (const std::optional<uint32_t> key_expr = context.BuildString("Key")) {
        dict_expression.input1 = *key_expr;
      }
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(dict_expression));

      LN_ValueExpression value_expression = dict_expression;
      value_expression.kind = LN_ValueExpressionKind::DictRemoveKeyValue;
      context.CacheValueOutputPin("Value", context.program.AddValueExpression(value_expression));
      break;
    }
    case LN_NodeKind::ListFromItems: {
      static const char *socket_names[] = {"Values",
                                           "Floats",
                                           "Integers",
                                           "Strings",
                                           "Booleans",
                                           "Vectors",
                                           "Colors",
                                           "Lists",
                                           "Dictionaries",
                                           "Datablocks",
                                           "Objects",
                                           "Collections",
                                           "Conditions",
                                           "Instances",
                                           "Widgets"};
      const int active_socket_index = std::clamp<int>(context.node.custom1,
                                                      0,
                                                      int(std::size(socket_names)) - 1);
      for (int socket_index = 0; socket_index < int(std::size(socket_names)); socket_index++) {
        const char *socket_name = socket_names[socket_index];
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::ListFromItems;
        if (socket_index == active_socket_index) {
          const blender::bNodeSocket *input_socket = FindInputSocket(context.node, socket_name);
          const auto links_iter = input_socket ? context.input_links.find(input_socket) :
                                                 context.input_links.end();
          if (links_iter != context.input_links.end() && !links_iter->second.empty()) {
            for (const ResolvedLink &link : links_iter->second) {
              if (link.fromnode == nullptr || link.fromsock == nullptr) {
                continue;
              }
              const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
                  context.program,
                  *link.fromnode,
                  *link.fromsock,
                  context.node_definitions,
                  context.input_links,
                  context.value_cache,
                  context.bool_expression_cache,
                  context.int_expression_cache,
                  context.float_expression_cache,
                  context.string_expression_cache,
                  context.vector_expression_cache,
                  context.color_expression_cache,
                  context.value_expression_cache);
              if (value_expr) {
                expression.input_indices.push_back(*value_expr);
              }
            }
          }
          else if (const std::optional<uint32_t> value_expr = context.BuildValueExpression(
                       socket_name))
          {
            expression.input_indices.push_back(*value_expr);
          }
        }
        context.CacheValueOutputPin(socket_name, context.program.AddValueExpression(expression));
      }
      break;
    }
    case LN_NodeKind::ListContains: {
      const std::optional<uint32_t> list_expr = context.BuildValueExpression("List");
      const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
      if (list_expr && value_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ListContains;
        expression.input0 = *list_expr;
        expression.input1 = *value_expr;
        context.CacheBoolOutputPin("Contains", context.program.AddBoolExpression(expression));
      }
      break;
    }
    case LN_NodeKind::DictHasKey: {
      const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary");
      const std::optional<uint32_t> key_expr = context.BuildString("Key");
      if (dict_expr && key_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::DictHasKey;
        expression.input0 = *dict_expr;
        expression.input1 = *key_expr;
        context.CacheBoolOutputPin("Has Key", context.program.AddBoolExpression(expression));
      }
      break;
    }
    case LN_NodeKind::DictLength: {
      const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary");
      if (dict_expr) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::DictLength;
        expression.input0 = *dict_expr;
        context.CacheIntOutputPin("Length", context.program.AddIntExpression(expression));
      }
      break;
    }
    case LN_NodeKind::ListLength: {
      const std::optional<uint32_t> list_expr = context.BuildValueExpression("List");
      if (list_expr) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::ListLength;
        expression.input0 = *list_expr;
        context.CacheIntOutputPin("Length", context.program.AddIntExpression(expression));
      }
      break;
    }
    case LN_NodeKind::ListGetItem: {
      const std::optional<uint32_t> list_expr = context.BuildValueExpression("List");
      const std::optional<uint32_t> index_expr = context.BuildInt("Index");
      if (list_expr && index_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::ListElement;
        expression.input0 = *list_expr;
        expression.input1 = *index_expr;
        context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
      }
      break;
    }
    case LN_NodeKind::ListDuplicate: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListDuplicate;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictMerge: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::DictMerge;
      if (const std::optional<uint32_t> dict_a = context.BuildValueExpression("Dictionary A")) {
        expression.input0 = *dict_a;
      }
      if (const std::optional<uint32_t> dict_b = context.BuildValueExpression("Dictionary B")) {
        expression.input1 = *dict_b;
      }
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListExtend: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ListExtend;
      if (const std::optional<uint32_t> list_a = context.BuildValueExpression("List A")) {
        expression.input0 = *list_a;
      }
      if (const std::optional<uint32_t> list_b = context.BuildValueExpression("List B")) {
        expression.input1 = *list_b;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictGetKey: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::DictGetKey;
      if (const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary")) {
        expression.input0 = *dict_expr;
      }
      if (const std::optional<uint32_t> key_expr = context.BuildString("Key")) {
        expression.input1 = *key_expr;
      }
      if (const std::optional<uint32_t> default_expr = context.BuildValueExpression(
              "Default Value"))
      {
        expression.input2 = *default_expr;
      }
      context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::EmptyList: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::EmptyList;
      if (const std::optional<uint32_t> length_expr = context.BuildInt("Length")) {
        expression.input0 = *length_expr;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::EmptyDict: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::EmptyDict;
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictGetKeys: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::DictGetKeys;
      if (const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary")) {
        expression.input0 = *dict_expr;
      }
      context.CacheValueOutputPin("Keys", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::MakeDict: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::MakeDict;
      if (const std::optional<uint32_t> key_expr = context.BuildString("Key")) {
        expression.input0 = *key_expr;
      }
      if (const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value")) {
        expression.input1 = *value_expr;
      }
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::MakeList: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::MakeList;
      if (const std::optional<uint32_t> item_a = context.BuildValueExpression("Item A")) {
        expression.input0 = *item_a;
      }
      if (const std::optional<uint32_t> item_b = context.BuildValueExpression("Item B")) {
        expression.input1 = *item_b;
      }
      if (const std::optional<uint32_t> item_c = context.BuildValueExpression("Item C")) {
        expression.input2 = *item_c;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    default:
      break;
  }
}

static LN_FloatCompareOperation CompareOperationFromNode(const blender::bNode &node)
{
  switch (std::clamp<int>(node.custom1, 0, 5)) {
    case 1:
      return LN_FloatCompareOperation::NotEqual;
    case 2:
      return LN_FloatCompareOperation::GreaterThan;
    case 3:
      return LN_FloatCompareOperation::LessThan;
    case 4:
      return LN_FloatCompareOperation::GreaterEqual;
    case 5:
      return LN_FloatCompareOperation::LessEqual;
    case 0:
    default:
      return LN_FloatCompareOperation::Equal;
  }
}

void CompileToggleTreePropertyNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Toggle Tree Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Toggle Tree Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t property_ref_index = AddTreePropertyRef(
      context.program, *name, LN_ValueType::Bool, MakeDefaultValue(LN_ValueType::Bool));
  LN_BoolExpression property_expr;
  property_expr.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
  property_expr.property_ref_index = property_ref_index;
  const uint32_t current = context.program.AddBoolExpression(property_expr);

  LN_BoolExpression not_expr;
  not_expr.kind = LN_BoolExpressionKind::Not;
  not_expr.input0 = current;
  const uint32_t toggled = context.program.AddBoolExpression(not_expr);

  LN_ValueExpression value_expr;
  value_expr.kind = LN_ValueExpressionKind::FromBool;
  value_expr.input0 = toggled;
  const uint32_t value_index = context.program.AddValueExpression(value_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetTreeProperty;
    instruction.source_ref_index = context.MakeSourceRef("Property");
    instruction.property_ref_index = property_ref_index;
    instruction.value_expr_index = value_index;
    AppendFlowInstruction(context.program, path, instruction);
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileEvaluatePropertyNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<std::string> property_name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Evaluate Object Property requires a constant property name");
  const std::optional<uint32_t> compare_expr = context.BuildValueExpression("Value");
  if (!property_name || !compare_expr) {
    return;
  }

  LN_ValueExpression value_expr;
  value_expr.kind = context.node.custom2 == 0 ? LN_ValueExpressionKind::ObjectGameProperty :
                                                LN_ValueExpressionKind::ObjectAttribute;
  if (object_expr) {
    value_expr.input0 = *object_expr;
  }
  value_expr.input1 = AddConstantStringExpression(context.program, *property_name);
  const uint32_t value_index = context.program.AddValueExpression(value_expr);
  context.CacheValueOutputPin("Value", value_index);

  LN_BoolExpression bool_expr;
  bool_expr.kind = LN_BoolExpressionKind::ValueCompare;
  bool_expr.input0 = value_index;
  bool_expr.input1 = *compare_expr;
  bool_expr.float_compare_operation = CompareOperationFromNode(context.node);
  context.CacheBoolOutputPin("If True", context.program.AddBoolExpression(bool_expr));
}

void CompileTypecastNode(CompilerContext &context)
{
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!value_expr) {
    return;
  }

  LN_IntExpression int_expr;
  int_expr.kind = LN_IntExpressionKind::FromGenericValue;
  int_expr.input0 = *value_expr;
  context.CacheIntOutputPin("Integer", context.program.AddIntExpression(int_expr));

  LN_BoolExpression bool_expr;
  bool_expr.kind = LN_BoolExpressionKind::FromGenericValue;
  bool_expr.input0 = *value_expr;
  context.CacheBoolOutputPin("Boolean", context.program.AddBoolExpression(bool_expr));

  LN_StringExpression string_expr;
  string_expr.kind = LN_StringExpressionKind::FromGenericValue;
  string_expr.value_expr_index = *value_expr;
  context.CacheStringOutputPin("String", context.program.AddStringExpression(string_expr));

  LN_FloatExpression float_expr;
  float_expr.kind = LN_FloatExpressionKind::FromGenericValue;
  float_expr.input0 = *value_expr;
  context.CacheFloatOutputPin("Float", context.program.AddFloatExpression(float_expr));
}

void CompileValueValidNode(CompilerContext &context)
{
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!value_expr) {
    return;
  }
  LN_BoolExpression none_expr;
  none_expr.kind = LN_BoolExpressionKind::ValueIsNone;
  none_expr.input0 = *value_expr;
  context.CacheBoolOutputPin("If Valid",
                             AddNotBoolExpression(context.program,
                                                  context.program.AddBoolExpression(none_expr)));
}

void CompileValueSwitchListNode(CompilerContext &context)
{
  static const char *conditions[] = {"if A", "elif B", "elif C", "elif D", "elif E", "elif F"};
  static const char *values[] = {"Value A", "Value B", "Value C", "Value D", "Value E", "Value F"};

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ValueSwitchList;
  for (size_t index = 0; index < std::size(conditions); index++) {
    expression.input_indices.push_back(context.BuildBool(conditions[index]).value_or(LN_INVALID_INDEX));
    expression.input_indices.push_back(context.BuildValueExpression(values[index]).value_or(LN_INVALID_INDEX));
  }
  context.CacheValueOutputPin("Result", context.program.AddValueExpression(expression));
}

void CompileValueSwitchListCompareNode(CompilerContext &context)
{
  static const char *cases[] = {"Case A", "Case B", "Case C", "Case D", "Case E", "Case F"};
  static const char *values[] = {"Value A", "Value B", "Value C", "Value D", "Value E", "Value F"};

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ValueSwitchListCompare;
  expression.input0 = context.BuildValueExpression("Switch").value_or(LN_INVALID_INDEX);
  expression.input1 = context.BuildValueExpression("Default").value_or(LN_INVALID_INDEX);
  expression.value.type = LN_ValueType::Int;
  expression.value.exists = true;
  expression.value.int_value = static_cast<int32_t>(CompareOperationFromNode(context.node));
  for (size_t index = 0; index < std::size(cases); index++) {
    expression.input_indices.push_back(context.BuildValueExpression(cases[index]).value_or(LN_INVALID_INDEX));
    expression.input_indices.push_back(context.BuildValueExpression(values[index]).value_or(LN_INVALID_INDEX));
  }
  context.CacheValueOutputPin("Result", context.program.AddValueExpression(expression));
}

void CompileListRandomItemNode(CompilerContext &context)
{
  if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::ListRandomItem;
    expression.input0 = *list_expr;
    context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
  }
}

void CompileGetChildByNameNode(CompilerContext &context)
{
  const std::optional<uint32_t> parent_expr = context.BuildOptionalObject("Parent");
  const std::optional<uint32_t> child_expr = context.BuildOptionalObject("Child");
  if (parent_expr && child_expr) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::ObjectChildByName;
    expression.input0 = *parent_expr;
    expression.input1 = *child_expr;
    context.CacheValueOutputPin("Child", context.program.AddValueExpression(expression));
  }
}

void CompileListSavedVariablesNode(CompilerContext &context)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ListSavedVariables;
  expression.input0 = context.BuildString("Path").value_or(LN_INVALID_INDEX);
  expression.input1 = context.BuildString("File").value_or(LN_INVALID_INDEX);
  expression.bool_expr_index = context.BuildBool("Print").value_or(LN_INVALID_INDEX);
  context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileClearVariablesNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Clear Variables flow must be driven by an event or branch");
  if (!path_expr || !file_expr || !flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ClearVariables;
    instruction.source_ref_index = context.MakeSourceRef("File");
    instruction.string_expr_index = *path_expr;
    instruction.secondary_string_expr_index = *file_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRemoveVariableNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  const std::optional<uint32_t> name_expr = context.BuildString("Name");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Remove Variable flow must be driven by an event or branch");
  if (!path_expr || !file_expr || !name_expr || !flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RemoveVariable;
    instruction.source_ref_index = context.MakeSourceRef("Name");
    instruction.string_expr_index = *path_expr;
    instruction.secondary_string_expr_index = *file_expr;
    instruction.tertiary_string_expr_index = *name_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileSetOverlayCollectionNode(CompilerContext &context)
{
  const std::optional<uint32_t> camera_expr = context.BuildCameraOrActive("Camera");
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Overlay Collection flow must be driven by an event or branch");
  if (!camera_expr || !collection_expr || !flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetOverlayCollection;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *camera_expr;
    instruction.secondary_value_expr_index = *collection_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRemoveOverlayCollectionNode(CompilerContext &context)
{
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Remove Overlay Collection flow must be driven by an event or branch");
  if (!collection_expr || !flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RemoveOverlayCollection;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *collection_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRandomFloatNode(CompilerContext &context)
{
  const std::optional<uint32_t> min_expr = context.BuildFloat("Min");
  const std::optional<uint32_t> max_expr = context.BuildFloat("Max");
  if (min_expr && max_expr) {
    LN_FloatExpression expression;
    expression.kind = LN_FloatExpressionKind::Random;
    expression.input0 = *min_expr;
    expression.input1 = *max_expr;
    context.CacheFloatOutputPin("Value", context.program.AddFloatExpression(expression));
  }
}

void CompileRandomIntNode(CompilerContext &context)
{
  const std::optional<uint32_t> min_expr = context.BuildInt("Min");
  const std::optional<uint32_t> max_expr = context.BuildInt("Max");
  if (min_expr && max_expr) {
    LN_IntExpression expression;
    expression.kind = LN_IntExpressionKind::Random;
    expression.input0 = *min_expr;
    expression.input1 = *max_expr;
    context.CacheIntOutputPin("Value", context.program.AddIntExpression(expression));
  }
}

void CompileRandomVectorNode(CompilerContext &context)
{
  if (const std::optional<uint32_t> axes_expr = context.BuildVector("Axes")) {
    LN_VectorExpression expression;
    expression.kind = LN_VectorExpressionKind::Random;
    expression.input0 = *axes_expr;
    context.CacheVectorOutputPin("Vector", context.program.AddVectorExpression(expression));
  }
}

void CompileFormulaNode(CompilerContext &context)
{
  const std::optional<uint32_t> a_expr = context.BuildFloat("a");
  const std::optional<uint32_t> b_expr = context.BuildFloat("b");
  if (!a_expr || !b_expr) {
    return;
  }
  LN_StringExpression formula_string;
  formula_string.kind = LN_StringExpressionKind::Constant;
  formula_string.string_value = FormulaTextFromNode(context.node);

  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::Formula;
  expression.input0 = *a_expr;
  expression.input1 = *b_expr;
  expression.string_expr_index = context.program.AddStringExpression(formula_string);
  context.CacheFloatOutputPin("Result", context.program.AddFloatExpression(expression));
}

void CompileTweenValueNode(CompilerContext &context)
{
  const int tween_type = std::clamp(int(context.node.custom1), 0, 3);

  LN_BoolExpression tween_expr;
  tween_expr.kind = LN_BoolExpressionKind::TweenValue;
  tween_expr.input0 = context.BuildExecution("Forward").value_or(LN_INVALID_INDEX);
  tween_expr.input1 = context.BuildExecution("Back").value_or(LN_INVALID_INDEX);
  tween_expr.float_expr_index = context.BuildFloat("Duration").value_or(LN_INVALID_INDEX);
  tween_expr.int_value = tween_type;
  tween_expr.int_expr_index = BakeTweenCurveTable(context);
  tween_expr.bool_value = (context.node.custom2 & 1) != 0;
  tween_expr.float_value = (context.node.custom2 & 2) != 0 ? 1.0f : 0.0f;
  const uint32_t tween_index = context.program.AddBoolExpression(tween_expr);
  context.CacheBoolOutputPin("Done", tween_index);

  LN_BoolExpression reached_expr;
  reached_expr.kind = LN_BoolExpressionKind::TweenReached;
  reached_expr.input0 = tween_index;
  context.CacheBoolOutputPin("Reached", context.program.AddBoolExpression(reached_expr));

  LN_FloatExpression factor_expr;
  factor_expr.kind = LN_FloatExpressionKind::TweenFactor;
  factor_expr.input0 = tween_index;
  context.CacheFloatOutputPin("Factor", context.program.AddFloatExpression(factor_expr));

  LN_FloatExpression float_result;
  float_result.kind = LN_FloatExpressionKind::TweenFloatResult;
  float_result.input0 = tween_index;
  float_result.input1 = context.BuildFloat("FromFloat").value_or(LN_INVALID_INDEX);
  float_result.input2 = context.BuildFloat("ToFloat").value_or(LN_INVALID_INDEX);
  context.CacheFloatOutputPin("ResultFloat", context.program.AddFloatExpression(float_result));

  LN_VectorExpression vector_result;
  vector_result.kind = LN_VectorExpressionKind::TweenVectorResult;
  vector_result.input0 = tween_index;
  vector_result.input1 = context.BuildVector("FromVector").value_or(LN_INVALID_INDEX);
  vector_result.input2 = context.BuildVector("ToVector").value_or(LN_INVALID_INDEX);
  context.CacheVectorOutputPin("ResultVector", context.program.AddVectorExpression(vector_result));

  LN_VectorExpression rotation_result;
  rotation_result.kind = LN_VectorExpressionKind::TweenRotationResult;
  rotation_result.input0 = tween_index;
  rotation_result.input1 = context.BuildRotation("FromRotation").value_or(LN_INVALID_INDEX);
  rotation_result.input2 = context.BuildRotation("ToRotation").value_or(LN_INVALID_INDEX);
  rotation_result.property_ref_index = tween_type == 3 ? 1u : 0u;
  context.CacheVectorOutputPin("ResultRotation",
                               context.program.AddVectorExpression(rotation_result));
}

void CompileAddPhysicsConstraintNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Add Rigid Body Constraints flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  auto constraint_type_from_node = [](const int type) {
    switch (type) {
      case 0:
        return PHY_RigidBodyConstraintType::Point;
      case 1:
        return PHY_RigidBodyConstraintType::Hinge;
      case 3:
        return PHY_RigidBodyConstraintType::Slider;
      case 5:
        return PHY_RigidBodyConstraintType::Generic;
      case 6:
        return PHY_RigidBodyConstraintType::GenericSpring;
      case 8:
        return PHY_RigidBodyConstraintType::Fixed;
      case 9:
        return PHY_RigidBodyConstraintType::Piston;
      case 11:
        return PHY_RigidBodyConstraintType::Motor;
      default:
        return PHY_RigidBodyConstraintType::Point;
    }
  };
  auto bool_slot = [](LN_RigidBodyConstraintBoolInput input) {
    return size_t(input);
  };
  auto vector_slot = [](LN_RigidBodyConstraintVectorInput input) {
    return size_t(input);
  };
  auto float_slot = [](LN_RigidBodyConstraintFloatInput input) {
    return size_t(input);
  };
  for (const FlowPath &path : flow_events->paths) {
    LN_RigidBodyConstraintCommandPayload payload;
    payload.constraint_object_value_expr_index = context.BuildOptionalObject("Constraint Object")
                                                     .value_or(LN_INVALID_INDEX);
    payload.object_value_expr_index = context.BuildOptionalObject("Object").value_or(
        LN_INVALID_INDEX);
    payload.target_value_expr_index = context.BuildOptionalObject("Target").value_or(
        LN_INVALID_INDEX);
    payload.name_string_expr_index = context.BuildString("Name").value_or(LN_INVALID_INDEX);
    payload.velocity_solver_iterations_int_expr_index =
        context.BuildInt("Velocity Solver Iterations").value_or(LN_INVALID_INDEX);
    payload.position_solver_iterations_int_expr_index =
        context.BuildInt("Position Solver Iterations").value_or(LN_INVALID_INDEX);
    payload.type = constraint_type_from_node(context.node.custom1);
    payload.spring_type = PHY_RigidBodyConstraintSpringType::Spring2;

    auto set_bool = [&](const LN_RigidBodyConstraintBoolInput input, const char *socket_name) {
      payload.bool_expr_indices[bool_slot(input)] = context.BuildBool(socket_name).value_or(
          LN_INVALID_INDEX);
    };
    auto set_vector = [&](const LN_RigidBodyConstraintVectorInput input, const char *socket_name) {
      payload.vector_expr_indices[vector_slot(input)] = context.BuildVector(socket_name).value_or(
          LN_INVALID_INDEX);
    };
    auto set_float = [&](const LN_RigidBodyConstraintFloatInput input, const char *socket_name) {
      payload.float_expr_indices[float_slot(input)] = context.BuildFloat(socket_name).value_or(
          LN_INVALID_INDEX);
    };

    set_bool(LN_RigidBodyConstraintBoolInput::UseWorldSpace, "Use World Space");
    set_bool(LN_RigidBodyConstraintBoolInput::Enabled, "Enabled");
    set_bool(LN_RigidBodyConstraintBoolInput::DisableCollisions, "Disable Collisions");
    set_bool(LN_RigidBodyConstraintBoolInput::Breakable, "Breakable");
    set_bool(LN_RigidBodyConstraintBoolInput::OverrideIterations, "Override Iterations");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitLinX, "Use Linear Limit X");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitLinY, "Use Linear Limit Y");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitLinZ, "Use Linear Limit Z");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitAngX, "Use Angular Limit X");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitAngY, "Use Angular Limit Y");
    set_bool(LN_RigidBodyConstraintBoolInput::UseLimitAngZ, "Use Angular Limit Z");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringX, "Use Spring X");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringY, "Use Spring Y");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringZ, "Use Spring Z");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringAngX, "Use Angular Spring X");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringAngY, "Use Angular Spring Y");
    set_bool(LN_RigidBodyConstraintBoolInput::UseSpringAngZ, "Use Angular Spring Z");
    set_bool(LN_RigidBodyConstraintBoolInput::UseMotorLin, "Use Linear Motor");
    set_bool(LN_RigidBodyConstraintBoolInput::UseMotorAng, "Use Angular Motor");

    set_vector(LN_RigidBodyConstraintVectorInput::Pivot, "Pivot");
    set_vector(LN_RigidBodyConstraintVectorInput::Rotation, "Rotation");
    set_vector(LN_RigidBodyConstraintVectorInput::LinearLower, "Linear Lower");
    set_vector(LN_RigidBodyConstraintVectorInput::LinearUpper, "Linear Upper");
    set_vector(LN_RigidBodyConstraintVectorInput::AngularLower, "Angular Lower");
    set_vector(LN_RigidBodyConstraintVectorInput::AngularUpper, "Angular Upper");
    set_vector(LN_RigidBodyConstraintVectorInput::SpringStiffness, "Spring Stiffness");
    set_vector(LN_RigidBodyConstraintVectorInput::SpringDamping, "Spring Damping");
    set_vector(LN_RigidBodyConstraintVectorInput::AngularSpringStiffness,
               "Angular Spring Stiffness");
    set_vector(LN_RigidBodyConstraintVectorInput::AngularSpringDamping,
               "Angular Spring Damping");

    set_float(LN_RigidBodyConstraintFloatInput::BreakingThreshold, "Breaking Threshold");
    set_float(LN_RigidBodyConstraintFloatInput::MotorLinTargetVelocity,
              "Linear Motor Target Velocity");
    set_float(LN_RigidBodyConstraintFloatInput::MotorLinMaxImpulse, "Linear Motor Max Impulse");
    set_float(LN_RigidBodyConstraintFloatInput::MotorAngTargetVelocity,
              "Angular Motor Target Velocity");
    set_float(LN_RigidBodyConstraintFloatInput::MotorAngMaxImpulse, "Angular Motor Max Impulse");

    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::AddPhysicsConstraint;
    instruction.source_ref_index = context.MakeSourceRef("Name");
    instruction.bool_guard_expr_index = path.bool_guard_expr_index;
    instruction.command_payload_index = context.program.AddRigidBodyConstraintCommandPayload(
        payload);
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution())
  {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRemovePhysicsConstraintNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Remove Rigid Body Constraints flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RemovePhysicsConstraint;
    instruction.source_ref_index = context.MakeSourceRef("Name");
    instruction.bool_guard_expr_index = path.bool_guard_expr_index;
    instruction.value_expr_index = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
    instruction.bool_expr_index = context.BuildBool("Remove All").value_or(LN_INVALID_INDEX);
    instruction.string_expr_index = context.BuildString("Name").value_or(LN_INVALID_INDEX);
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution())
  {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileGetRigidBodyConstraintsNode(CompilerContext &context)
{
  const LN_RigidBodyConstraintMatchMode match_mode = [&]() {
    switch (context.node.custom1) {
      case 1:
        return LN_RigidBodyConstraintMatchMode::Contains;
      case 2:
        return LN_RigidBodyConstraintMatchMode::All;
      default:
        return LN_RigidBodyConstraintMatchMode::Exact;
    }
  }();
  const uint32_t object_expr = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  const uint32_t name_expr = match_mode == LN_RigidBodyConstraintMatchMode::All ?
                                 LN_INVALID_INDEX :
                                 context.BuildString("Name").value_or(LN_INVALID_INDEX);

  if (context.OutputFeedsActiveNode("Found")) {
    LN_BoolExpression found_expression;
    found_expression.kind = LN_BoolExpressionKind::RigidBodyConstraintFound;
    found_expression.input0 = object_expr;
    found_expression.property_ref_index = name_expr;
    found_expression.rigid_body_constraint_match_mode = match_mode;
    context.CacheBoolOutputPin("Found", context.program.AddBoolExpression(found_expression));
  }

  if (context.OutputFeedsActiveNode("Constraint")) {
    LN_StringExpression constraint_expression;
    constraint_expression.kind = LN_StringExpressionKind::RigidBodyConstraintName;
    constraint_expression.value_expr_index = object_expr;
    constraint_expression.input0 = name_expr;
    constraint_expression.rigid_body_constraint_match_mode = match_mode;
    context.CacheStringOutputPin("Constraint",
                                 context.program.AddStringExpression(constraint_expression));
  }

  if (context.OutputFeedsActiveNode("Constraints")) {
    LN_ValueExpression constraints_expression;
    constraints_expression.kind = LN_ValueExpressionKind::RigidBodyConstraintNames;
    constraints_expression.input0 = object_expr;
    constraints_expression.string_expr_index = name_expr;
    constraints_expression.rigid_body_constraint_match_mode = match_mode;
    context.CacheValueOutputPin("Constraints",
                                context.program.AddValueExpression(constraints_expression));
  }
}

void CompileProjectileRayNode(CompilerContext &context)
{
  const std::optional<uint32_t> condition_expr = context.BuildPrimaryExecution();
  const std::optional<uint32_t> caster_expr = context.BuildOptionalObject("Caster");
  const std::optional<uint32_t> origin_expr = context.BuildVector("Origin");
  const std::optional<uint32_t> aim_expr = context.BuildVector("Aim");
  if (!condition_expr || !caster_expr || !origin_expr || !aim_expr) {
    return;
  }

  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::ProjectileRay;
  query.condition_bool_expr_index = *condition_expr;
  query.input0 = *caster_expr;
  query.input1 = *origin_expr;
  query.input2 = *aim_expr;
  query.bool_expr_index = context.BuildBool("Local").value_or(LN_INVALID_INDEX);
  query.float_expr_index = context.BuildFloat("Power").value_or(LN_INVALID_INDEX);
  query.secondary_float_expr_index = context.BuildFloat("Distance").value_or(LN_INVALID_INDEX);
  query.tertiary_float_expr_index = context.BuildFloat("Resolution").value_or(LN_INVALID_INDEX);
  query.int_expr_index = context.BuildInt("Mask").value_or(LN_INVALID_INDEX);
  query.string_expr_index = context.BuildString("Property").value_or(LN_INVALID_INDEX);
  query.secondary_bool_expr_index = context.BuildBool("X-Ray").value_or(LN_INVALID_INDEX);
  query.tertiary_bool_expr_index = context.BuildBool("Visualize").value_or(LN_INVALID_INDEX);
  query.float_value = 0.0f;
  query.int_value = 0;
  query.cache_key = context.node.identifier;
  const uint32_t query_index = context.program.AddQueryExpression(query);

  LN_BoolExpression hit_expr;
  hit_expr.kind = LN_BoolExpressionKind::PhysicsQueryHit;
  hit_expr.input0 = query_index;
  context.CacheBoolOutputPin("Has Result", context.program.AddBoolExpression(hit_expr));

  LN_ValueExpression object_expr;
  object_expr.kind = LN_ValueExpressionKind::PhysicsQueryObject;
  object_expr.input0 = query_index;
  context.CacheValueOutputPin("Picked Object", context.program.AddValueExpression(object_expr));

  LN_VectorExpression point_expr;
  point_expr.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
  point_expr.input0 = query_index;
  context.CacheVectorOutputPin("Picked Point", context.program.AddVectorExpression(point_expr));

  LN_VectorExpression normal_expr;
  normal_expr.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
  normal_expr.input0 = query_index;
  context.CacheVectorOutputPin("Picked Normal", context.program.AddVectorExpression(normal_expr));

  LN_ValueExpression parabola_expr;
  parabola_expr.kind = LN_ValueExpressionKind::ProjectileParabola;
  parabola_expr.input0 = query_index;
  context.CacheValueOutputPin("Parabola", context.program.AddValueExpression(parabola_expr));
}

void CompileSpawnPoolNode(CompilerContext &context)
{
  const uint32_t pool_index = context.program.AddSpawnPoolState();

  auto emit_create = [&](const FlowPath *path) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SpawnPoolCreate;
    instruction.int_value = int32_t(pool_index);
    instruction.secondary_int_value = context.node.custom1;
    if (path) {
      instruction.bool_guard_expr_index = path->bool_guard_expr_index;
    }
    instruction.value_expr_index = context.BuildOptionalObject("Object Instance").value_or(
        LN_INVALID_INDEX);
    instruction.int_expr_index = context.BuildInt("Amount").value_or(LN_INVALID_INDEX);
    instruction.secondary_int_expr_index = context.BuildInt("Life").value_or(LN_INVALID_INDEX);
    instruction.tertiary_int_expr_index = context.BuildInt("Bitmask").value_or(LN_INVALID_INDEX);
    instruction.secondary_value_expr_index = context.BuildOptionalObject("Spawner").value_or(
        LN_INVALID_INDEX);
    instruction.bool_expr_index = context.BuildBool("Visualize").value_or(LN_INVALID_INDEX);
    if (path) {
      AppendFlowInstruction(context.program, *path, instruction);
    }
    else {
      context.program.AddInstruction(LN_Event::OnInit, instruction);
    }
  };

  if ((context.node.custom2 & 1) != 0) {
    emit_create(nullptr);
  }

  if (const std::optional<FlowEventsResult> create_flow = context.ResolveOptionalFlow("Create Pool"))
  {
    for (const FlowPath &path : create_flow->paths) {
      emit_create(&path);
    }
    if (const std::optional<uint32_t> done_expr = context.BuildBool("Create Pool")) {
      context.CacheBoolOutputPin("OUT", *done_expr);
    }
  }

  if (const std::optional<FlowEventsResult> spawn_flow = context.ResolveOptionalFlow("Spawn"))
  {
    for (const FlowPath &path : spawn_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::SpawnPoolSpawn;
      instruction.int_value = int32_t(pool_index);
      instruction.bool_guard_expr_index = path.bool_guard_expr_index;
      instruction.float_expr_index = context.BuildFloat("Speed").value_or(LN_INVALID_INDEX);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  LN_BoolExpression spawned_pulse;
  spawned_pulse.kind = LN_BoolExpressionKind::SpawnPoolSpawnedPulse;
  spawned_pulse.int_value = int32_t(pool_index);
  context.CacheBoolOutputPin("SPAWNED", context.program.AddBoolExpression(spawned_pulse));

  LN_BoolExpression hit_pulse;
  hit_pulse.kind = LN_BoolExpressionKind::SpawnPoolHitPulse;
  hit_pulse.int_value = int32_t(pool_index);
  context.CacheBoolOutputPin("ONHIT", context.program.AddBoolExpression(hit_pulse));

  LN_ValueExpression hit_object;
  hit_object.kind = LN_ValueExpressionKind::SpawnPoolHitObject;
  hit_object.input0 = pool_index;
  context.CacheValueOutputPin("HITOBJECT", context.program.AddValueExpression(hit_object));

  LN_VectorExpression hit_point;
  hit_point.kind = LN_VectorExpressionKind::SpawnPoolHitPoint;
  hit_point.input0 = pool_index;
  context.CacheVectorOutputPin("HITPOINT", context.program.AddVectorExpression(hit_point));

  LN_VectorExpression hit_normal;
  hit_normal.kind = LN_VectorExpressionKind::SpawnPoolHitNormal;
  hit_normal.input0 = pool_index;
  context.CacheVectorOutputPin("HITNORMAL", context.program.AddVectorExpression(hit_normal));

  LN_VectorExpression hit_direction;
  hit_direction.kind = LN_VectorExpressionKind::SpawnPoolHitDirection;
  hit_direction.input0 = pool_index;
  context.CacheVectorOutputPin("HITDIR", context.program.AddVectorExpression(hit_direction));
}

void CompileSnapshotTransformNode(CompilerContext &context)
{
  if (context.definition.kind == LN_NodeKind::GetGravity) {
    LN_VectorExpression expression;
    expression.kind = LN_VectorExpressionKind::SnapshotGravity;
    context.CacheVectorOutputPin("Gravity", context.program.AddVectorExpression(expression));
  }
}

static void CompileExpressionOutputsNodeImpl(CompilerContext &context, const bool linked_only)
{
  for (const LN_PinDefinition &pin : context.definition.outputs) {
    if (linked_only && !context.OutputFeedsActiveNode(pin.name.c_str())) {
      continue;
    }
    const blender::bNodeSocket *socket = FindOutputSocket(context.node, pin.name);
    if (socket == nullptr) {
      continue;
    }

    switch (pin.value_type) {
      case LN_ValueType::Bool:
        (void)BuildOutputBoolExpression(context.program,
                                          context.node,
                                          *socket,
                                          context.node_definitions,
                                          context.input_links,
                                          context.value_cache,
                                          context.float_expression_cache,
                                          context.bool_expression_cache);
        break;
      case LN_ValueType::Int:
        (void)BuildOutputIntExpression(context.program,
                                         context.node,
                                         *socket,
                                         context.node_definitions,
                                         context.input_links,
                                         context.value_cache,
                                         context.int_expression_cache,
                                         &context.bool_expression_cache,
                                         &context.float_expression_cache,
                                         &context.string_expression_cache,
                                         &context.vector_expression_cache,
                                         &context.color_expression_cache,
                                         &context.value_expression_cache);
        break;
      case LN_ValueType::Float:
        (void)BuildOutputFloatExpression(context.program,
                                          context.node,
                                          *socket,
                                          context.node_definitions,
                                          context.input_links,
                                          context.value_cache,
                                          context.float_expression_cache,
                                          &context.bool_expression_cache,
                                          &context.vector_expression_cache);
        break;
      case LN_ValueType::String:
        (void)BuildOutputStringExpression(context.program,
                                            context.node,
                                            *socket,
                                            context.node_definitions,
                                            context.input_links,
                                            context.value_cache,
                                            context.string_expression_cache);
        break;
      case LN_ValueType::Vector:
      case LN_ValueType::Rotation:
        (void)BuildOutputVectorExpression(context.program,
                                          context.node,
                                          *socket,
                                          context.node_definitions,
                                          context.input_links,
                                          context.value_cache,
                                          context.float_expression_cache,
                                          context.vector_expression_cache);
        break;
      case LN_ValueType::Color:
        (void)BuildOutputColorExpression(context.program,
                                           context.node,
                                           *socket,
                                           context.node_definitions,
                                           context.input_links,
                                           context.value_cache,
                                           context.float_expression_cache,
                                           context.color_expression_cache);
        break;
      case LN_ValueType::ObjectRef:
      case LN_ValueType::SceneRef:
      case LN_ValueType::CollectionRef:
      case LN_ValueType::DatablockRef:
      case LN_ValueType::Vector4:
      case LN_ValueType::Matrix:
      case LN_ValueType::List:
      case LN_ValueType::Dict:
      case LN_ValueType::Generic:
        (void)BuildOutputValueExpression(context.program,
                                           context.node,
                                           *socket,
                                           context.node_definitions,
                                           context.input_links,
                                           context.value_cache,
                                           context.bool_expression_cache,
                                           context.int_expression_cache,
                                           context.float_expression_cache,
                                           context.string_expression_cache,
                                           context.vector_expression_cache,
                                           context.color_expression_cache,
                                           context.value_expression_cache);
        break;
      case LN_ValueType::None:
        break;
    }
  }
}

void CompileExpressionOutputsNode(CompilerContext &context)
{
  CompileExpressionOutputsNodeImpl(context, false);
}

static bool BoolExpressionIsKnownFalse(const LN_Program &program, const uint32_t expr_index)
{
  const std::vector<LN_BoolExpression> &expressions = program.GetBoolExpressions();
  if (expr_index == LN_INVALID_INDEX || expr_index >= expressions.size()) {
    return false;
  }
  const LN_BoolExpression &expression = expressions[expr_index];
  return expression.kind == LN_BoolExpressionKind::Constant && !expression.bool_value;
}

void CompileRaycastNode(CompilerContext &context)
{
  CompileExpressionOutputsNodeImpl(context, true);

  if (!context.InputIsLinked("Flow")) {
    return;
  }

  const std::optional<uint32_t> visualize_expr = context.BuildBool("Visualize");
  if (!visualize_expr) {
    context.AddError("Visualize", "Raycast requires a Visualize input");
    return;
  }
  if (BoolExpressionIsKnownFalse(context.program, *visualize_expr)) {
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Raycast flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const blender::bNodeSocket *done_socket = FindOutputSocket(context.node, "Done");
  if (done_socket == nullptr) {
    context.AddError("Done", "Raycast requires a Done output");
    return;
  }

  const std::optional<uint32_t> done_expr = BuildOutputBoolExpression(context.program,
                                                                      context.node,
                                                                      *done_socket,
                                                                      context.node_definitions,
                                                                      context.input_links,
                                                                      context.value_cache,
                                                                      context.float_expression_cache,
                                                                      context.bool_expression_cache);
  if (!done_expr) {
    context.AddError("Done", "Raycast query output did not compile");
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Nop;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    const FlowPath guarded_path = GuardFlowPath(context.program, path, *done_expr);
    AppendFlowInstruction(context.program, guarded_path, instruction);
  }
}

void CompileLoopNode(CompilerContext &context)
{
  IntExpressionCache int_expression_cache;
  ValueExpressionCache value_expression_cache;
  EnsureLoopFrame(context.program,
                  context.node,
                  context.definition,
                  context.node_definitions,
                  context.input_links,
                  context.value_cache,
                  context.float_expression_cache,
                  context.bool_expression_cache,
                  int_expression_cache,
                  value_expression_cache,
                  context.loop_frame_cache);
}

static FlowPath AppendPrecompiledRoute(CompilerContext &context,
                                       const FlowPath &source_path,
                                       const uint32_t condition_expr,
                                       const char *output_name)
{
  LN_Instruction route;
  route.opcode = LN_OpCode::BranchRoute;
  route.source_ref_index = context.MakeSourceRef(output_name);
  route.bool_expr_index = condition_expr;
  route.bool_value = true;
  const uint32_t route_index = AppendFlowInstruction(context.program, source_path, route);

  LN_BoolExpression route_pulse;
  route_pulse.kind = LN_BoolExpressionKind::InstructionExecuted;
  route_pulse.input0 = route_index;

  FlowPath output_path;
  output_path.event = source_path.event;
  output_path.bool_guard_expr_index = context.program.AddBoolExpression(route_pulse);
  output_path.loop_frame_index = source_path.loop_frame_index;
  return output_path;
}

static void CachePrecompiledRoutes(CompilerContext &context,
                                   const char *output_name,
                                   const std::vector<FlowPath> &paths)
{
  const blender::bNodeSocket *socket = FindOutputSocket(context.node, output_name);
  if (socket == nullptr) {
    context.AddError(output_name, std::string("Missing execution output '") + output_name + "'");
    return;
  }

  FlowEventsResult result;
  result.paths = paths;
  result.valid = true;
  context.flow_events_cache[socket] = result;

  std::vector<uint32_t> route_pulses;
  route_pulses.reserve(paths.size());
  for (const FlowPath &path : paths) {
    route_pulses.push_back(path.bool_guard_expr_index);
  }
  context.bool_expression_cache[socket] = CombineBoolExpressionsWithOr(context.program,
                                                                       route_pulses);
}

void CompileOnceNode(CompilerContext &context)
{
  LN_BoolExpression once;
  once.kind = LN_BoolExpressionKind::Once;
  const uint32_t once_expr = context.program.AddBoolExpression(once);

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Do Once Reset must be driven by an event or branch");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::ResetOnce;
      instruction.source_ref_index = context.MakeSourceRef("Reset");
      instruction.int_value = int32_t(once_expr);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Do Once Flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  std::vector<FlowPath> output_paths;
  const bool route_output = context.OutputFeedsActiveNode("Out");
  output_paths.reserve(flow_events->paths.size());
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::TryOnce;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = int32_t(once_expr);
    AppendFlowInstruction(context.program, path, instruction);
    if (route_output) {
      output_paths.push_back(AppendPrecompiledRoute(context, path, once_expr, "Out"));
    }
  }
  if (!output_paths.empty()) {
    CachePrecompiledRoutes(context, "Out", output_paths);
  }
}

void CompileBooleanEdgeNode(CompilerContext &context)
{
  const std::optional<uint32_t> condition = context.BuildBool("Condition");
  if (!condition) {
    context.AddError("Condition", "Boolean Edge requires a Condition input");
    return;
  }

  LN_BoolExpression rising;
  rising.kind = LN_BoolExpressionKind::BooleanEdge;
  rising.input0 = *condition;
  const uint32_t rising_expr = context.program.AddBoolExpression(rising);
  context.CacheBoolOutputPin("Rising", rising_expr);

  LN_BoolExpression falling;
  falling.kind = LN_BoolExpressionKind::BooleanEdgeFalling;
  falling.input0 = rising_expr;
  context.CacheBoolOutputPin("Falling", context.program.AddBoolExpression(falling));
}

void CompileCooldownNode(CompilerContext &context)
{
  const std::optional<uint32_t> duration = context.BuildFloat("Duration");
  if (!duration) {
    context.AddError("Duration", "Cooldown requires a Duration input");
    return;
  }
  const std::optional<uint32_t> ignore_timescale = context.BuildBool("Ignore Timescale");

  const uint32_t state_index = context.program.AddTimeFlowState();
  auto add_state_bool = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(state_index);
    return context.program.AddBoolExpression(expression);
  };
  const uint32_t accepted_expr = add_state_bool(LN_BoolExpressionKind::CooldownAccepted);
  const uint32_t blocked_expr = add_state_bool(LN_BoolExpressionKind::CooldownBlocked);
  const uint32_t completed_expr = add_state_bool(LN_BoolExpressionKind::CooldownCompleted);
  context.CacheBoolOutputPin("Is Ready", add_state_bool(LN_BoolExpressionKind::CooldownReady));

  LN_FloatExpression remaining;
  remaining.kind = LN_FloatExpressionKind::CooldownRemaining;
  remaining.int_value = int32_t(state_index);
  context.CacheFloatOutputPin("Remaining", context.program.AddFloatExpression(remaining));

  LN_FloatExpression progress;
  progress.kind = LN_FloatExpressionKind::CooldownProgress;
  progress.int_value = int32_t(state_index);
  context.CacheFloatOutputPin("Progress", context.program.AddFloatExpression(progress));

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Cooldown Reset must be driven by an event or branch");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::ResetCooldown;
      instruction.source_ref_index = context.MakeSourceRef("Reset");
      instruction.int_value = int32_t(state_index);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  /* State outputs remain readable while the cooldown is idle. */
  if (!context.InputIsLinked("Flow")) {
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Cooldown Flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (std::any_of(flow_events->paths.begin(),
                  flow_events->paths.end(),
                  [](const FlowPath &path) { return path.loop_frame_index != LN_INVALID_INDEX; }))
  {
    context.AddError(
        "Flow",
        "Cooldown cannot run inside a Loop body; connect it before the Loop or trigger it from "
        "a separate non-loop event path");
    return;
  }

  std::vector<FlowPath> accepted_paths;
  std::vector<FlowPath> blocked_paths;
  const bool route_accepted = context.OutputFeedsActiveNode("Accepted");
  const bool route_blocked = context.OutputFeedsActiveNode("Blocked");
  accepted_paths.reserve(flow_events->paths.size());
  blocked_paths.reserve(flow_events->paths.size());
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::TryCooldown;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.float_expr_index = *duration;
    instruction.secondary_bool_expr_index = ignore_timescale.value_or(LN_INVALID_INDEX);
    instruction.int_value = int32_t(state_index);
    AppendFlowInstruction(context.program, path, instruction);
    if (route_accepted) {
      accepted_paths.push_back(
          AppendPrecompiledRoute(context, path, accepted_expr, "Accepted"));
    }
    if (route_blocked) {
      blocked_paths.push_back(AppendPrecompiledRoute(context, path, blocked_expr, "Blocked"));
    }
  }

  if (!accepted_paths.empty()) {
    CachePrecompiledRoutes(context, "Accepted", accepted_paths);
  }
  if (!blocked_paths.empty()) {
    CachePrecompiledRoutes(context, "Blocked", blocked_paths);
  }
  if (context.OutputFeedsActiveNode("Completed")) {
    FlowPath completed_path;
    completed_path.event = LN_Event::OnFixedUpdate;
    completed_path.bool_guard_expr_index = completed_expr;
    CachePrecompiledRoutes(context, "Completed", {completed_path});
  }
}

void CompileDelayNode(CompilerContext &context)
{
  const std::optional<uint32_t> duration_expr = context.BuildFloat("Delay");
  if (!duration_expr) {
    context.AddError("Delay", "Delay requires a duration input");
    return;
  }
  const std::optional<uint32_t> ignore_timescale_expr = context.BuildBool("Ignore Timescale");

  const uint32_t state_index = context.program.AddTimeFlowState();
  LN_BoolExpression done_expr;
  done_expr.kind = LN_BoolExpressionKind::DelayDone;
  done_expr.int_value = int32_t(state_index);
  context.CacheBoolOutputPin("Out", context.program.AddBoolExpression(done_expr));

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Delay flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ArmDelay;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.float_expr_index = *duration_expr;
    instruction.secondary_bool_expr_index = ignore_timescale_expr.value_or(LN_INVALID_INDEX);
    instruction.int_value = int32_t(state_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileTimerNode(CompilerContext &context)
{
  const std::optional<uint32_t> duration_expr = context.BuildFloat("Seconds");
  if (!duration_expr) {
    context.AddError("Seconds", "Timer requires a duration input");
    return;
  }
  const std::optional<uint32_t> ignore_timescale_expr = context.BuildBool("Ignore Timescale");

  const uint32_t timer_state_index = context.program.AddTimeFlowState();
  LN_BoolExpression elapsed_expr;
  elapsed_expr.kind = LN_BoolExpressionKind::TimerElapsed;
  elapsed_expr.int_value = int32_t(timer_state_index);
  const uint32_t elapsed_expr_index = context.program.AddBoolExpression(elapsed_expr);
  context.CacheBoolOutputPin("Out", elapsed_expr_index);

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Timer Set Timer must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ArmTimer;
    instruction.source_ref_index = context.MakeSourceRef("Set Timer");
    instruction.float_expr_index = *duration_expr;
    instruction.secondary_bool_expr_index = ignore_timescale_expr.value_or(LN_INVALID_INDEX);
    instruction.int_value = int32_t(timer_state_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompilePulsifyNode(CompilerContext &context)
{
  const std::optional<uint32_t> interval_expr = context.BuildFloat("Gap");
  if (!interval_expr) {
    context.AddError("Gap", "Pulsify requires a gap input");
    return;
  }
  const std::optional<uint32_t> ignore_timescale_expr = context.BuildBool("Ignore Timescale");

  const uint32_t state_index = context.program.AddTimeFlowState();
  LN_BoolExpression pulse_expr;
  pulse_expr.kind = LN_BoolExpressionKind::PulsifyPulse;
  pulse_expr.int_value = int32_t(state_index);
  context.CacheBoolOutputPin("Out", context.program.AddBoolExpression(pulse_expr));

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Pulsify flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::UpdatePulsify;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.float_expr_index = *interval_expr;
    instruction.secondary_bool_expr_index = ignore_timescale_expr.value_or(LN_INVALID_INDEX);
    instruction.int_value = int32_t(state_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileBarrierNode(CompilerContext &context)
{
  const std::optional<uint32_t> condition_expr = context.BuildBool("Condition");
  if (!condition_expr) {
    context.AddError("Condition", "Barrier requires a boolean Condition input");
    return;
  }
  const std::optional<uint32_t> duration_expr = context.BuildFloat("Time");
  if (!duration_expr) {
    context.AddError("Time", "Barrier requires a time input");
    return;
  }
  const std::optional<uint32_t> ignore_timescale_expr = context.BuildBool("Ignore Timescale");

  const uint32_t state_index = context.program.AddTimeFlowState();
  LN_BoolExpression passed_expr;
  passed_expr.kind = LN_BoolExpressionKind::BarrierPassed;
  passed_expr.int_value = int32_t(state_index);
  context.CacheBoolOutputPin("Out", context.program.AddBoolExpression(passed_expr));

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Barrier flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::UpdateBarrier;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.bool_expr_index = *condition_expr;
    instruction.float_expr_index = *duration_expr;
    instruction.secondary_bool_expr_index = ignore_timescale_expr.value_or(LN_INVALID_INDEX);
    instruction.int_value = int32_t(state_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompilePrintNode(CompilerContext &context)
{
  std::optional<uint32_t> message_expr = context.BuildValueExpression("Message");
  std::string source_socket = "Message";
  if (!message_expr) {
    message_expr = context.BuildValueExpression("Value");
    source_socket = "Value";
  }
  if (!message_expr) {
    for (const blender::bNodeSocket *socket =
             static_cast<const blender::bNodeSocket *>(context.node.inputs.first);
         socket != nullptr;
         socket = socket->next)
    {
      if (NamesMatch(socket->name, socket->identifier, "Flow") ||
          NamesMatch(socket->name, socket->identifier, "Condition"))
      {
        continue;
      }
      if (socket->identifier[0] != '\0') {
        message_expr = context.BuildValueExpression(socket->identifier);
        source_socket = socket->identifier;
      }
      if (!message_expr && socket->name[0] != '\0') {
        message_expr = context.BuildValueExpression(socket->name);
        source_socket = socket->name;
      }
      if (message_expr) {
        break;
      }
    }
  }
  if (!message_expr) {
    context.AddError(source_socket.c_str(), "Print requires a message input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Print flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Print;
    instruction.source_ref_index = context.MakeSourceRef(source_socket.c_str());
    instruction.value_expr_index = *message_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileQuitGameNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Quit Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::QuitGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileRestartGameNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Restart Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RestartGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileLoadBlendFileNode(CompilerContext &context)
{
  const std::optional<uint32_t> filepath_expr = context.BuildString("File Name");
  if (!filepath_expr) {
    context.AddError("File Name", "Load Blender File requires a file path input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Load Blender File flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::LoadBlendFile;
    instruction.source_ref_index = context.MakeSourceRef("File Name");
    instruction.string_expr_index = *filepath_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSendEventNode(CompilerContext &context)
{
  const std::optional<uint32_t> subject_expr = context.BuildString("Subject");
  if (!subject_expr) {
    context.AddError("Subject", "Send Event requires a subject input");
    return;
  }

  const bool use_content = (context.node.custom1 & 1) != 0;
  const bool use_target = context.node.custom2 != 0;
  const std::optional<uint32_t> content_expr = use_content ?
                                                   context.BuildValueExpression("Content") :
                                                   std::nullopt;
  const std::optional<uint32_t> messenger_expr = use_content ?
                                                     context.BuildOptionalObject("Messenger") :
                                                     std::nullopt;
  const std::optional<uint32_t> target_expr = use_target ?
                                                    context.BuildOptionalObject("Target") :
                                                    std::nullopt;
  if (use_target && !target_expr) {
    context.AddWarning("Target",
                       "Send Event target mode has no Target object and will not send events; "
                       "turn Use Target off for broadcast");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Send Event flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SendEvent;
    instruction.source_ref_index = context.MakeSourceRef("Subject");
    instruction.string_expr_index = *subject_expr;
    if (content_expr) {
      instruction.value_expr_index = *content_expr;
    }
    if (messenger_expr) {
      instruction.secondary_value_expr_index = *messenger_expr;
    }
    if (target_expr) {
      instruction.int_expr_index = *target_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGlobalPropertyNode(CompilerContext &context)
{
  const std::optional<uint32_t> category_expr = context.BuildString("Category");
  const std::optional<uint32_t> property_expr = context.BuildString("Property");
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!category_expr || !property_expr || !value_expr) {
    context.AddError("Property", "Set Global Property requires category, property, and value inputs");
    return;
  }

  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }

  const std::optional<uint32_t> persistent_expr = context.BuildBool("Persistent");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Global Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGlobalProperty;
    instruction.source_ref_index = context.MakeSourceRef("Property");
    instruction.string_expr_index = *category_expr;
    instruction.secondary_string_expr_index = *property_expr;
    instruction.value_expr_index = *value_expr;
    if (persistent_expr) {
      instruction.bool_expr_index = *persistent_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSaveVariableNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  const std::optional<uint32_t> name_expr = context.BuildString("Name");
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!path_expr || !file_expr || !name_expr || !value_expr) {
    context.AddError("Name", "Save Variable requires path, file, name, and value inputs");
    return;
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Save Variable flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SaveVariable;
    instruction.source_ref_index = context.MakeSourceRef("Name");
    instruction.string_expr_index = *path_expr;
    instruction.secondary_string_expr_index = *file_expr;
    instruction.tertiary_string_expr_index = *name_expr;
    instruction.value_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSaveVariableDictNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  const std::optional<uint32_t> variables_expr = context.BuildValueExpression("Variables");
  if (!path_expr || !file_expr || !variables_expr) {
    context.AddError("Variables", "Save Variable Dict requires path, file, and dictionary inputs");
    return;
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Save Variable Dict flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SaveVariableDict;
    instruction.source_ref_index = context.MakeSourceRef("Variables");
    instruction.string_expr_index = *path_expr;
    instruction.secondary_string_expr_index = *file_expr;
    instruction.value_expr_index = *variables_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileTranslateNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> vector_expr = context.BuildVector("Vector");
  const std::optional<uint32_t> speed_expr = context.BuildFloat("Speed");
  const std::optional<uint32_t> local_expr = context.BuildBool("Local");
  if (!vector_expr || !speed_expr || !local_expr) {
    context.AddError("Vector", "Translate requires vector, speed, and local inputs");
    return;
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("When Done", *done_expr);
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Translate flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Translate;
    instruction.source_ref_index = context.MakeSourceRef("Vector");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *vector_expr;
    instruction.float_expr_index = *speed_expr;
    instruction.bool_expr_index = *local_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileNavigateNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Moving Object");
  const std::optional<uint32_t> rotating_object_expr = context.BuildOptionalObject("Rotating Object");
  const std::optional<uint32_t> navmesh_expr = context.BuildOptionalObject("Navmesh Object");
  const std::optional<uint32_t> destination_expr = context.BuildVector("Destination");
  const std::optional<uint32_t> dynamic_expr = context.BuildBool("Move as Dynamic");
  const std::optional<uint32_t> speed_expr = context.BuildFloat("Lin Speed");
  const std::optional<uint32_t> threshold_expr = context.BuildFloat("Reach Threshold");
  const std::optional<uint32_t> look_at_expr = context.BuildBool("Look At");
  const std::optional<uint32_t> rot_axis_expr = context.BuildInt("Rot Axis");
  const std::optional<uint32_t> front_expr = context.BuildInt("Front");
  const std::optional<uint32_t> rot_speed_expr = context.BuildFloat("Rot Speed");
  const std::optional<uint32_t> visualize_expr = context.BuildBool("Visualize");
  if (!destination_expr || !dynamic_expr || !speed_expr || !threshold_expr || !look_at_expr ||
      !rot_axis_expr || !front_expr || !rot_speed_expr || !visualize_expr)
  {
    context.AddError("Destination", "Move To with Navmesh requires destination, speed, and threshold inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Move To with Navmesh flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  std::vector<uint32_t> reached_outputs;
  uint32_t first_instruction_index = LN_INVALID_INDEX;
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Navigate;
    instruction.source_ref_index = context.MakeSourceRef("Destination");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (navmesh_expr) {
      instruction.secondary_value_expr_index = *navmesh_expr;
    }
    if (rotating_object_expr) {
      instruction.tertiary_value_expr_index = *rotating_object_expr;
    }
    instruction.vector_expr_index = *destination_expr;
    instruction.bool_expr_index = *dynamic_expr;
    instruction.secondary_bool_expr_index = *look_at_expr;
    instruction.tertiary_bool_expr_index = *visualize_expr;
    instruction.int_expr_index = *rot_axis_expr;
    instruction.secondary_int_expr_index = *front_expr;
    instruction.float_expr_index = *speed_expr;
    instruction.secondary_float_expr_index = *threshold_expr;
    instruction.tertiary_float_expr_index = *rot_speed_expr;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    if (first_instruction_index == LN_INVALID_INDEX) {
      first_instruction_index = instruction_index;
    }
    LN_BoolExpression done_expression;
    done_expression.kind = LN_BoolExpressionKind::InstructionExecuted;
    done_expression.input0 = instruction_index;
    done_outputs.push_back(context.program.AddBoolExpression(done_expression));

    LN_BoolExpression reached_expression;
    reached_expression.kind = LN_BoolExpressionKind::InstructionReached;
    reached_expression.input0 = instruction_index;
    reached_outputs.push_back(context.program.AddBoolExpression(reached_expression));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
  context.CacheBoolOutputPin("When Reached",
                             CombineBoolExpressionsWithOr(context.program, reached_outputs));
  if (first_instruction_index != LN_INVALID_INDEX) {
    LN_VectorExpression next_point_expression;
    next_point_expression.kind = LN_VectorExpressionKind::InstructionNextPoint;
    next_point_expression.input0 = first_instruction_index;
    const uint32_t next_point_vector = context.program.AddVectorExpression(next_point_expression);
    context.CacheVectorOutputPin("Next Point", next_point_vector);

    LN_ValueExpression next_point_value;
    next_point_value.kind = LN_ValueExpressionKind::FromVector;
    next_point_value.input0 = next_point_vector;
    context.CacheValueOutputPin("Next Point", context.program.AddValueExpression(next_point_value));
  }
}

void CompileFollowPathNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Moving Object");
  const std::optional<uint32_t> rotating_object_expr = context.BuildOptionalObject("Rotating Object");
  const std::optional<uint32_t> path_expr = context.BuildValueExpression("Path Points");
  const std::optional<uint32_t> loop_expr = context.BuildBool("Loop");
  const std::optional<uint32_t> continue_expr = context.BuildBool("Continue");
  const std::optional<uint32_t> navmesh_expr = context.BuildOptionalObject("Optional Navmesh");
  const std::optional<uint32_t> dynamic_expr = context.BuildBool("Move as Dynamic");
  const std::optional<uint32_t> speed_expr = context.BuildFloat("Lin Speed");
  const std::optional<uint32_t> threshold_expr = context.BuildFloat("Reach Threshold");
  const std::optional<uint32_t> look_at_expr = context.BuildBool("Look At");
  const std::optional<uint32_t> rot_speed_expr = context.BuildFloat("Rot Speed");
  const std::optional<uint32_t> rot_axis_expr = context.BuildInt("Rot Axis");
  const std::optional<uint32_t> front_expr = context.BuildInt("Front");
  if (!path_expr || !loop_expr || !continue_expr || !dynamic_expr || !speed_expr ||
      !threshold_expr || !look_at_expr || !rot_speed_expr || !rot_axis_expr || !front_expr)
  {
    context.AddError("Path Points", "Follow Path requires path points, speed, and threshold inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Follow Path flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::FollowPath;
    instruction.source_ref_index = context.MakeSourceRef("Path Points");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.secondary_value_expr_index = *path_expr;
    instruction.bool_expr_index = *loop_expr;
    instruction.secondary_bool_expr_index = *continue_expr;
    instruction.tertiary_bool_expr_index = *dynamic_expr;
    instruction.quaternary_bool_expr_index = *look_at_expr;
    if (navmesh_expr) {
      instruction.tertiary_value_expr_index = *navmesh_expr;
    }
    if (rotating_object_expr) {
      instruction.quaternary_value_expr_index = *rotating_object_expr;
    }
    instruction.int_expr_index = *rot_axis_expr;
    instruction.secondary_int_expr_index = *front_expr;
    instruction.float_expr_index = *speed_expr;
    instruction.secondary_float_expr_index = *threshold_expr;
    instruction.tertiary_float_expr_index = *rot_speed_expr;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    LN_BoolExpression done_expression;
    done_expression.kind = LN_BoolExpressionKind::InstructionExecuted;
    done_expression.input0 = instruction_index;
    done_outputs.push_back(context.program.AddBoolExpression(done_expression));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileSetCollectionVisibilityNode(CompilerContext &context)
{
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<uint32_t> visible_expr = context.BuildBool("Visible");
  const std::optional<uint32_t> recursive_expr = context.BuildBool("Include Children");
  if (!collection_expr || !visible_expr || !recursive_expr) {
    context.AddError("Collection", "Set Collection Visibility requires collection and visibility inputs");
    return;
  }
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Collection Visibility flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCollectionVisibility;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *collection_expr;
    instruction.bool_expr_index = *visible_expr;
    instruction.secondary_bool_expr_index = *recursive_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompilePlayActionNode(CompilerContext &context)
{
  const blender::ID *action_id = context.node.id;
  if (action_id == nullptr) {
    context.AddWarning("action", "Play Action skipped: no Action is selected on the node");
    return;
  }
  /* ID codes live in the first two bytes of #ID.name (see #GS in DNA_ID.h). Do not read
   * `sizeof(ID_Type)` from `name`; the enum is wider than the on-disk/runtime prefix. */
  const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(action_id->name));
  if (id_code != int16_t(blender::ID_AC)) {
    context.AddWarning("action", "Play Action skipped: selected data-block is not an Action");
    return;
  }
  const std::string action_name(action_id->name + 2);
  const uint32_t name_expr = AddConstantStringExpression(context.program, action_name);

  const std::optional<uint32_t> start_expr = context.BuildFloat("Start Frame");
  const std::optional<uint32_t> end_expr = context.BuildFloat("End Frame");
  const std::optional<uint32_t> layer_expr = context.BuildInt("Layer");
  const std::optional<uint32_t> priority_expr = context.BuildInt("Priority");
  const std::optional<uint32_t> blendin_expr = context.BuildFloat("Blend In");
  const std::optional<uint32_t> speed_expr = context.BuildFloat("Speed");
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  if (!start_expr || !end_expr || !layer_expr || !priority_expr || !blendin_expr || !speed_expr) {
    context.AddError("Start Frame", "Play Action requires numeric inputs");
    return;
  }

  const int play_mode = int(std::clamp(int(context.node.custom1), 0, int(BL_Action::ACT_MODE_MAX) - 1));
  const int blend_mode = int(std::clamp(int(context.node.custom2), 0, int(BL_Action::ACT_BLEND_MAX) - 1));
  const uint32_t pack = uint32_t(play_mode & 0xFF) | (uint32_t(blend_mode & 0xFF) << 8u);

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Play Action flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::PlayAction;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    instruction.float_expr_index = *start_expr;
    instruction.secondary_float_expr_index = *end_expr;
    instruction.tertiary_float_expr_index = *blendin_expr;
    instruction.quaternary_float_expr_index = *speed_expr;
    instruction.int_expr_index = *layer_expr;
    instruction.secondary_int_expr_index = *priority_expr;
    instruction.int_value = int32_t(pack);
    instruction.secondary_vector_value = MT_Vector3(-1.0f, 1.0f, 0.0f);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStopActionNode(CompilerContext &context)
{
  const std::optional<uint32_t> layer_expr = context.BuildInt("Layer");
  if (!layer_expr) {
    context.AddError("Layer", "Stop Action requires a layer input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Stop Action flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::StopAction;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *layer_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetActionFrameNode(CompilerContext &context)
{
  const std::optional<uint32_t> layer_expr = context.BuildInt("Layer");
  const std::optional<uint32_t> frame_expr = context.BuildFloat("Frame");
  if (!layer_expr || !frame_expr) {
    context.AddError("Frame", "Set Action Frame requires layer and frame inputs");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Action Frame flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetActionFrame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *layer_expr;
    instruction.float_expr_index = *frame_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStopAllSoundsNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Stop All Sounds flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::StopAllSounds;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompilePlaySoundNode(CompilerContext &context)
{
  const blender::ID *sound_id = context.node.id;
  if (sound_id == nullptr) {
    context.AddWarning("sound", "Play Sound skipped: no Sound is selected on the node");
    return;
  }
  const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(sound_id->name));
  if (id_code != int16_t(blender::ID_SO)) {
    context.AddWarning("sound", "Play Sound skipped: selected data-block is not a Sound");
    return;
  }
  const std::string sound_name(sound_id->name + 2);
  const uint32_t name_expr = AddConstantStringExpression(context.program, sound_name);

  const std::optional<uint32_t> volume_expr = context.BuildFloat("Volume");
  const std::optional<uint32_t> pitch_expr = context.BuildFloat("Pitch");
  const std::optional<uint32_t> loop_expr = context.BuildBool("Loop");
  if (!volume_expr || !pitch_expr || !loop_expr) {
    context.AddError("Volume", "Play Sound requires volume, pitch, and loop inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Play Sound flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::PlaySound;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    instruction.float_expr_index = *volume_expr;
    instruction.secondary_float_expr_index = *pitch_expr;
    instruction.bool_expr_index = *loop_expr;
    instruction.vector_value = MT_Vector3(1.0f, 1.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStopSoundNode(CompilerContext &context)
{
  const blender::ID *sound_id = context.node.id;
  if (sound_id == nullptr) {
    context.AddWarning("sound", "Stop Sound skipped: no Sound is selected on the node");
    return;
  }
  const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(sound_id->name));
  if (id_code != int16_t(blender::ID_SO)) {
    context.AddWarning("sound", "Stop Sound skipped: selected data-block is not a Sound");
    return;
  }
  const std::string sound_name(sound_id->name + 2);
  const uint32_t name_expr = AddConstantStringExpression(context.program, sound_name);

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Stop Sound flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::StopSound;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

static bool CompileSoundNameFromNode(CompilerContext &context, std::string &r_sound_name)
{
  const blender::ID *sound_id = context.node.id;
  if (sound_id == nullptr) {
    context.AddWarning("sound", "Sound node skipped: no Sound is selected on the node");
    return false;
  }
  const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(sound_id->name));
  if (id_code != int16_t(blender::ID_SO)) {
    context.AddWarning("sound", "Sound node skipped: selected data-block is not a Sound");
    return false;
  }
  r_sound_name = std::string(sound_id->name + 2);
  return true;
}

void CompilePlaySound3DNode(CompilerContext &context)
{
  std::string sound_name;
  if (!CompileSoundNameFromNode(context, sound_name)) {
    return;
  }
  const uint32_t name_expr = AddConstantStringExpression(context.program, sound_name);
  const std::optional<uint32_t> speaker_expr = context.BuildOptionalObject("Speaker");
  const std::optional<uint32_t> volume_expr = context.BuildFloat("Volume");
  const std::optional<uint32_t> pitch_expr = context.BuildFloat("Pitch");
  const std::optional<uint32_t> loop_expr = context.BuildBool("Loop");
  if (!volume_expr || !pitch_expr || !loop_expr) {
    context.AddError("Volume", "Play Sound 3D requires volume, pitch, and loop inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Play Sound 3D flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::PlaySound3D;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    instruction.float_expr_index = *volume_expr;
    instruction.secondary_float_expr_index = *pitch_expr;
    instruction.bool_expr_index = *loop_expr;
    if (speaker_expr) {
      instruction.value_expr_index = *speaker_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStartSpeakerNode(CompilerContext &context)
{
  const std::optional<uint32_t> speaker_expr = context.BuildOptionalObject("Speaker");
  const std::optional<uint32_t> loop_count_expr = context.BuildInt("Mode");
  if (!speaker_expr || !loop_count_expr) {
    context.AddError("Speaker", "Start Speaker requires a speaker object and loop mode");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Start Speaker flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::PlaySound3D;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.value_expr_index = *speaker_expr;
    instruction.int_expr_index = *loop_count_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompilePauseSoundNode(CompilerContext &context)
{
  std::string sound_name;
  if (!CompileSoundNameFromNode(context, sound_name)) {
    return;
  }
  const uint32_t name_expr = AddConstantStringExpression(context.program, sound_name);
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Pause Sound flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::PauseSound;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileResumeSoundNode(CompilerContext &context)
{
  std::string sound_name;
  if (!CompileSoundNameFromNode(context, sound_name)) {
    return;
  }
  const uint32_t name_expr = AddConstantStringExpression(context.program, sound_name);
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Resume Sound flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ResumeSound;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = name_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBonePoseRotationNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  const std::optional<uint32_t> rotation_expr = context.BuildRotation("Rotation");
  if (!bone_name_expr || !rotation_expr) {
    context.AddError("Rotation", "Set Bone Pose Rotation requires bone name and rotation inputs");
    return;
  }
  if (context.node.custom1 < int(LN_BonePoseRotationSpace::PoseChannel) ||
      context.node.custom1 > int(LN_BonePoseRotationSpace::World))
  {
    context.AddError("Space", "Set Bone Pose Rotation has an unsupported space mode");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Pose Rotation flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBonePoseRotation;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.vector_expr_index = *rotation_expr;
    instruction.int_value = context.node.custom1;
    instruction.secondary_int_value = (context.node.custom2 != 0) ? 1 : 0;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBoneConstraintInfluenceNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  const std::optional<uint32_t> constraint_name_expr = context.BuildString("Constraint Name");
  const std::optional<uint32_t> influence_expr = context.BuildFloat("Influence");
  if (!bone_name_expr || !constraint_name_expr || !influence_expr) {
    context.AddError("Influence",
                     "Set Bone Constraint Influence requires bone, constraint, and influence");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Constraint Influence flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBoneConstraintInfluence;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.secondary_string_expr_index = *constraint_name_expr;
    instruction.float_expr_index = *influence_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetMaterialSlotNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  std::optional<uint32_t> material_expr;
  const bool material_linked = context.InputIsLinked("Material");
  const std::optional<LN_Value> material_default = context.ReadValue("Material",
                                                                     LN_ValueType::DatablockRef);
  if (material_linked || (material_default && material_default->exists)) {
    material_expr = context.BuildValueExpression("Material");
  }
  else if (const blender::ID *legacy_material_id = context.node.id) {
    const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(legacy_material_id->name));
    if (id_code == int16_t(blender::ID_MA)) {
      LN_Value material_value;
      material_value.type = LN_ValueType::DatablockRef;
      material_value.exists = true;
      material_value.reference_name = legacy_material_id->name + 2;
      material_expr = AddConstantValueExpression(context.program, material_value);
    }
  }
  if (!material_expr) {
    context.AddWarning("Material",
                       "Assign Material To Slot skipped: no Material is selected or connected");
    return;
  }

  const std::optional<uint32_t> slot_expr = context.BuildInt("Slot");
  if (!slot_expr) {
    context.AddError("Slot", "Assign Material To Slot requires a slot index");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Assign Material To Slot flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetMaterialSlot;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.secondary_value_expr_index = *material_expr;
    instruction.int_expr_index = *slot_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileMoveTowardNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> target_expr = context.BuildVector("Target Position");
  const std::optional<uint32_t> speed_expr = context.BuildFloat("Speed");
  const std::optional<uint32_t> distance_expr = context.BuildFloat("Stop Distance");
  if (!target_expr || !speed_expr || !distance_expr) {
    context.AddError("Target Position", "Move To requires target, speed, and stop distance inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Move To flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::MoveToward;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.vector_expr_index = *target_expr;
    instruction.float_expr_index = *speed_expr;
    instruction.secondary_float_expr_index = *distance_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSlowFollowNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> target_expr = context.BuildValueExpression("Target");
  const std::optional<uint32_t> factor_expr = context.BuildFloat("Factor");
  if (!target_expr || !factor_expr) {
    context.AddError("Target", "Slow Follow requires target and factor inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Slow Follow flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SlowFollow;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.float_expr_index = *factor_expr;
    instruction.secondary_value_expr_index = *target_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileLoadSceneNode(CompilerContext &context)
{
  const std::optional<uint32_t> scene_expr = context.BuildString("Scene");
  if (!scene_expr) {
    context.AddError("Scene", "Load Scene requires a scene name input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Load Scene flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::LoadScene;
    instruction.source_ref_index = context.MakeSourceRef("Scene");
    instruction.string_expr_index = *scene_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetSceneNode(CompilerContext &context)
{
  const std::optional<uint32_t> scene_expr = context.BuildString("Scene");
  if (!scene_expr) {
    context.AddError("Scene", "Set Scene requires a scene name input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Scene flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetScene;
    instruction.source_ref_index = context.MakeSourceRef("Scene");
    instruction.string_expr_index = *scene_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSaveGameNode(CompilerContext &context)
{
  const std::optional<uint32_t> slot_expr = context.BuildInt("Slot");
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  if (!slot_expr) {
    context.AddError("Slot", "Save Game requires a slot input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Save Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SaveGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *slot_expr;
    if (path_expr) {
      instruction.string_expr_index = *path_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileLoadGameNode(CompilerContext &context)
{
  const std::optional<uint32_t> slot_expr = context.BuildInt("Slot");
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  if (!slot_expr) {
    context.AddError("Slot", "Load Game requires a slot input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Load Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::LoadGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *slot_expr;
    if (path_expr) {
      instruction.string_expr_index = *path_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileAlignAxisToVectorNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> vector_expr = context.BuildVector("Vector");
  const std::optional<uint32_t> axis_expr = context.BuildInt("Axis");
  const std::optional<uint32_t> factor_expr = context.BuildFloat("Factor");
  if (!vector_expr || !axis_expr || !factor_expr) {
    context.AddError("Vector", "Align Axis to Vector requires vector, axis, and factor inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Align Axis to Vector flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::AlignAxisToVector;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.vector_expr_index = *vector_expr;
    instruction.int_expr_index = *axis_expr;
    instruction.float_expr_index = *factor_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileReplaceMeshNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> mesh_expr = context.BuildValueExpression("Mesh Object");
  if (!mesh_expr) {
    context.AddError("Mesh Object", "Replace Mesh requires a mesh object input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Replace Mesh flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ReplaceMesh;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.secondary_value_expr_index = *mesh_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileCopyPropertyNode(CompilerContext &context)
{
  const std::optional<uint32_t> source_expr = context.BuildOptionalObject("Source");
  const std::optional<uint32_t> target_expr = context.BuildValueExpression("Target");
  const std::optional<uint32_t> property_expr = context.BuildString("Property");
  if (!target_expr || !property_expr) {
    context.AddError("Target", "Copy Property requires target and property inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Copy Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::CopyProperty;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.secondary_value_expr_index = *target_expr;
    instruction.string_expr_index = *property_expr;
    if (source_expr) {
      instruction.value_expr_index = *source_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBonePoseLocationNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  const std::optional<uint32_t> location_expr = context.BuildVector("Location");
  if (!bone_name_expr || !location_expr) {
    context.AddError("Location", "Set Bone Pose Location requires bone name and location inputs");
    return;
  }
  if (context.node.custom1 < int(LN_BonePoseLocationSpace::ArmatureOffset) ||
      context.node.custom1 > int(LN_BonePoseLocationSpace::PoseChannel))
  {
    context.AddError("Space", "Set Bone Pose Location has an unsupported space mode");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Pose Location flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBonePoseLocation;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.vector_expr_index = *location_expr;
    instruction.int_value = context.node.custom1;
    instruction.secondary_int_value = (context.node.custom2 != 0) ? 1 : 0;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBonePoseTransformNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  const std::optional<uint32_t> location_expr = context.BuildVector("Location");
  const std::optional<uint32_t> rotation_expr = context.BuildRotation("Rotation");
  if (!bone_name_expr || !location_expr || !rotation_expr) {
    context.AddError("Location",
                     "Set Bone Pose Transform requires bone name, location, and rotation inputs");
    return;
  }
  if (context.node.custom1 < int(LN_BonePoseLocationSpace::ArmatureOffset) ||
      context.node.custom1 > int(LN_BonePoseLocationSpace::PoseChannel))
  {
    context.AddError("Space", "Set Bone Pose Transform has an unsupported space mode");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Pose Transform flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBonePoseTransform;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.vector_expr_index = *location_expr;
    instruction.secondary_vector_expr_index = *rotation_expr;
    instruction.int_value = context.node.custom1;
    instruction.secondary_int_value = (context.node.custom2 != 0) ? 1 : 0;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBonePoseScaleNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  const std::optional<uint32_t> scale_expr = context.BuildVector("Scale");
  if (!bone_name_expr || !scale_expr) {
    context.AddError("Scale", "Set Bone Pose Scale requires bone name and scale inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Pose Scale flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBonePoseScale;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.vector_expr_index = *scale_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBoneAttributeNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone Name");
  if (!bone_name_expr) {
    context.AddError("Bone Name", "Set Bone Attribute requires a bone name");
    return;
  }

  std::optional<uint32_t> value_expr;
  switch (context.node.custom1) {
    case 13:
    case 15:
    case 16:
    case 17:
    case 18:
      value_expr = context.BuildValueExpression("Bool");
      break;
    case 2:
      break;
    default:
      context.AddError("Attribute", "Unsupported Set Bone Attribute selection");
      return;
  }
  if (context.node.custom1 != 2 && !value_expr) {
    context.AddError("Value", "Set Bone Attribute requires a value input for the selected attribute");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Bone Attribute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBoneAttribute;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.int_value = context.node.custom1;
    instruction.secondary_int_value = context.node.custom2;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (value_expr) {
      instruction.secondary_value_expr_index = *value_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBoneConstraintTargetNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Armature");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone");
  const std::optional<uint32_t> constraint_name_expr = context.BuildString("Constraint");
  const std::optional<uint32_t> target_expr = context.BuildValueExpression("Target");
  if (!bone_name_expr || !constraint_name_expr || !target_expr) {
    context.AddError("Target", "Set Target requires bone, constraint, and target inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Target flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBoneConstraintTarget;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.secondary_string_expr_index = *constraint_name_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.secondary_value_expr_index = *target_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetBoneConstraintAttributeNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Armature");
  const std::optional<uint32_t> bone_name_expr = context.BuildString("Bone");
  const std::optional<uint32_t> constraint_name_expr = context.BuildString("Constraint");
  const std::optional<uint32_t> attribute_expr = context.BuildString("Attribute");
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!bone_name_expr || !constraint_name_expr || !attribute_expr || !value_expr) {
    context.AddError("Value", "Set Attribute requires bone, constraint, attribute, and value");
    return;
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Attribute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetBoneConstraintAttribute;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *bone_name_expr;
    instruction.secondary_string_expr_index = *constraint_name_expr;
    instruction.tertiary_string_expr_index = *attribute_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.secondary_value_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

const char *EditorNodeValueSocketIdentifier(const int value_type)
{
  switch (value_type) {
    case 1:
      return "Integer Value";
    case 2:
      return "Boolean Value";
    case 3:
      return "Vector Value";
    case 4:
      return "Color Value";
    case 5:
      return "String Value";
    case 6:
      return "Material Value";
    case 7:
      return "Value";
    default:
      return "Float Value";
  }
}

static void CompileGeometryNodesSetterNode(CompilerContext &context,
                                           const char *node_label,
                                           const LN_OpCode opcode,
                                           const bool direct_node_socket)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> modifier_expr = context.BuildString("Modifier");
  const std::optional<uint32_t> node_or_input_expr = context.BuildString(
      direct_node_socket ? "Node Name" : "Input");
  const std::optional<uint32_t> socket_expr = direct_node_socket ?
                                                  context.BuildString("Socket") :
                                                  std::nullopt;
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  const std::optional<uint32_t> value_expr = BuildEditorNodeValueExpression(context);
  if (!modifier_expr || !node_or_input_expr || (direct_node_socket && !socket_expr) ||
      !value_expr)
  {
    context.AddError(value_socket,
                     std::string(node_label) + " requires modifier, target, and value inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      std::string(node_label) + " flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.string_expr_index = *modifier_expr;
    instruction.secondary_string_expr_index = *node_or_input_expr;
    if (socket_expr) {
      instruction.tertiary_string_expr_index = *socket_expr;
    }
    instruction.secondary_value_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGeometryNodesInputNode(CompilerContext &context)
{
  CompileGeometryNodesSetterNode(
      context, "Set Geometry Nodes Input", LN_OpCode::SetGeometryNodesInput, false);
}

static void CompileCompositorEditorNodeValue(CompilerContext &context)
{
  using blender::ID_Type;
  const blender::ID *target_id = context.node.id;
  const int16_t target_id_code = target_id != nullptr ? GS(target_id->name) : 0;
  const blender::bNodeTree *target_tree = nullptr;
  if (target_id_code == blender::ID_SCE) {
    target_tree = reinterpret_cast<const blender::Scene *>(target_id)->compositing_node_group;
  }
  else if (target_id_code == blender::ID_NT) {
    target_tree = reinterpret_cast<const blender::bNodeTree *>(target_id);
  }
  if (target_id == nullptr || (target_id->tag & blender::ID_TAG_MISSING) != 0 ||
      target_tree == nullptr || target_tree->type != blender::NTREE_COMPOSIT)
  {
    context.AddWarning("Node Name",
                       "Set Editor Node Value skipped: no compositor node tree selected");
    return;
  }

  const std::optional<uint32_t> node_name_expr = context.BuildString("Node Name");
  const std::optional<uint32_t> socket_expr = context.BuildString("Socket");
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  const std::optional<uint32_t> value_expr = BuildEditorNodeValueExpression(context);
  if (!node_name_expr || !socket_expr || !value_expr) {
    context.AddError(
        value_socket, "Set Editor Node Value requires node, socket, and value inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Editor Node Value flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t target_name_expr = AddConstantStringExpression(context.program,
                                                                 target_id->name + 2);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCompositorNodeSocketValue;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = target_id_code;
    instruction.string_expr_index = target_name_expr;
    instruction.secondary_string_expr_index = *node_name_expr;
    instruction.tertiary_string_expr_index = *socket_expr;
    instruction.secondary_value_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetEditorNodeValueNode(CompilerContext &context)
{
  constexpr int editor_shift = 4;
  const int editor_type = (context.node.custom2 >> editor_shift) & 0x3;
  switch (editor_type) {
    case 1:
      CompileMaterialEditorNodeValue(
          context, "Set Editor Node Value", LN_OpCode::SetMaterialNodeSocketValue);
      return;
    case 2:
      CompileGeometryNodesSetterNode(
          context, "Set Editor Node Value", LN_OpCode::SetGeometryNodeSocketValue, true);
      return;
    case 3:
      CompileCompositorEditorNodeValue(context);
      return;
    default:
      context.AddError("Flow", "Set Editor Node Value has an unsupported editor type");
      return;
  }
}

void CompileMakeNodeTreeUniqueNode(CompilerContext &context)
{
  using blender::ID_Type;
  const int editor_type = context.node.custom1;
  if (!ELEM(editor_type, 1, 2, 3)) {
    context.AddError("Flow", "Make Node Tree Unique has an unsupported editor type");
    return;
  }

  std::optional<uint32_t> object_expr;
  std::optional<uint32_t> slot_expr;
  std::optional<uint32_t> target_expr;
  if (editor_type == 1) {
    object_expr = context.BuildOptionalObject("Object");
    slot_expr = context.BuildInt("Slot");
    if (!slot_expr) {
      context.AddError("Slot", "Make Node Tree Unique requires a material slot");
      return;
    }
  }
  else if (editor_type == 2) {
    object_expr = context.BuildOptionalObject("Object");
    target_expr = context.BuildString("Modifier");
    if (!target_expr) {
      context.AddError("Modifier", "Make Node Tree Unique requires a modifier");
      return;
    }
  }
  else {
    const blender::ID *target_id = context.node.id;
    if (target_id == nullptr || GS(target_id->name) != blender::ID_SCE ||
        (target_id->tag & blender::ID_TAG_MISSING) != 0 ||
        reinterpret_cast<const blender::Scene *>(target_id)->compositing_node_group == nullptr)
    {
      context.AddWarning("Flow",
                         "Make Node Tree Unique skipped: no compositor Scene selected");
      return;
    }
    target_expr = AddConstantStringExpression(context.program, target_id->name + 2);
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Make Node Tree Unique flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::MakeNodeTreeUnique;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = editor_type;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (slot_expr) {
      instruction.int_expr_index = *slot_expr;
    }
    if (target_expr) {
      instruction.string_expr_index = *target_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetNodeMuteNode(CompilerContext &context)
{
  using blender::ID_Type;
  const blender::ID *target_id = context.node.id;
  const blender::bNodeTree *target_tree = nullptr;
  const int16_t target_id_code = target_id != nullptr ? GS(target_id->name) : 0;
  if (target_id != nullptr) {
    switch (target_id_code) {
      case blender::ID_MA:
        target_tree = reinterpret_cast<const blender::Material *>(target_id)->nodetree;
        break;
      case blender::ID_SCE:
        target_tree = reinterpret_cast<const blender::Scene *>(target_id)
                          ->compositing_node_group;
        break;
      case blender::ID_NT:
        target_tree = reinterpret_cast<const blender::bNodeTree *>(target_id);
        break;
      default:
        break;
    }
  }
  if (target_id == nullptr || (target_id->tag & blender::ID_TAG_MISSING) != 0 ||
      target_tree == nullptr ||
      !ELEM(target_tree->type,
            blender::NTREE_SHADER,
            blender::NTREE_GEOMETRY,
            blender::NTREE_COMPOSIT))
  {
    context.AddWarning("Node Name", "Set Node Mute skipped: no supported node tree selected");
    return;
  }

  const std::optional<uint32_t> node_name_expr = context.BuildString("Node Name");
  const std::optional<uint32_t> muted_expr = context.BuildBool("Muted");
  if (!node_name_expr || !muted_expr) {
    context.AddError("Node Name", "Set Node Mute requires node and muted inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Node Mute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t target_name_expr = AddConstantStringExpression(context.program,
                                                                 target_id->name + 2);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetNodeMute;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = target_id_code;
    instruction.string_expr_index = target_name_expr;
    instruction.secondary_string_expr_index = *node_name_expr;
    instruction.bool_expr_index = *muted_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileEnableDisableModifierNode(CompilerContext &context)
{
  constexpr int target_name = 0;
  constexpr int target_stack_position = 1;
  constexpr int target_persistent_id = 2;
  constexpr int stack_index = 2;

  if (context.node.custom1 != target_name && context.node.custom1 != target_stack_position &&
      context.node.custom1 != target_persistent_id)
  {
    context.AddError("Modifier", "Enable or Disable Modifier has an unsupported target mode");
    return;
  }
  if (context.node.custom1 == target_stack_position &&
      (context.node.custom2 < 0 || context.node.custom2 > stack_index))
  {
    context.AddError("Index", "Enable or Disable Modifier has an unsupported stack position");
    return;
  }

  const int32_t target = context.node.custom1 == target_name ? 0 :
                         context.node.custom1 == target_persistent_id ? 4 :
                                                                        context.node.custom2 + 1;
  const std::optional<uint32_t> modifier_expr = target == 0 ? context.BuildString("Modifier") :
                                                              std::nullopt;
  const std::optional<uint32_t> index_expr = target == 3 ? context.BuildInt("Index") :
                                                target == 4 ? context.BuildInt("Modifier ID") :
                                                              std::nullopt;
  const std::optional<uint32_t> enabled_expr = context.BuildBool("Enabled");
  if ((target == 0 && !modifier_expr) || (ELEM(target, 3, 4) && !index_expr) || !enabled_expr) {
    context.AddError("Enabled", "Enable or Disable Modifier requires a target and enabled input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Enable or Disable Modifier flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::EnableDisableModifier;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = target;
    instruction.bool_expr_index = *enabled_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (modifier_expr) {
      instruction.string_expr_index = *modifier_expr;
    }
    if (index_expr) {
      instruction.int_expr_index = *index_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileAssignGeometryNodesModifierNode(CompilerContext &context)
{
  constexpr int operation_append = 0;
  constexpr int operation_insert = 1;
  constexpr int operation_replace = 2;
  constexpr int target_name = 0;
  constexpr int target_index = 3;
  constexpr int target_persistent_id = 4;

  const int operation = context.node.custom1;
  const int replace_target = context.node.custom2;
  if (!ELEM(operation, operation_append, operation_insert, operation_replace)) {
    context.AddError("Flow", "Assign Geometry Nodes Modifier has an unsupported operation");
    return;
  }
  if (operation == operation_replace && (replace_target < target_name ||
                                          replace_target > target_persistent_id))
  {
    context.AddError("Modifier", "Assign Geometry Nodes Modifier has an unsupported target");
    return;
  }

  const blender::ID *tree_id = context.node.id;
  using blender::ID_Type;
  if (tree_id == nullptr || GS(tree_id->name) != blender::ID_NT) {
    context.AddWarning("Modifier", "Assign Geometry Nodes Modifier skipped: no node group selected");
    return;
  }
  const blender::bNodeTree *node_group = reinterpret_cast<const blender::bNodeTree *>(tree_id);
  if ((tree_id->tag & blender::ID_TAG_MISSING) != 0 ||
      node_group->type != blender::NTREE_GEOMETRY ||
      node_group->geometry_node_asset_traits == nullptr ||
      (node_group->geometry_node_asset_traits->flag & blender::GEO_NODE_ASSET_MODIFIER) == 0)
  {
    context.AddWarning(
        "Modifier",
        "Assign Geometry Nodes Modifier skipped: selected node group is not modifier-compatible");
    return;
  }

  LN_Value node_group_value;
  node_group_value.type = LN_ValueType::DatablockRef;
  node_group_value.exists = true;
  node_group_value.reference_name = tree_id->name + 2;
  const uint32_t node_group_expr = AddConstantValueExpression(context.program, node_group_value);

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  std::optional<uint32_t> string_expr;
  std::optional<uint32_t> index_expr;
  if (operation == operation_replace && replace_target == target_name) {
    string_expr = context.BuildString("Modifier");
  }
  else if (operation != operation_replace) {
    string_expr = context.BuildString("Modifier Name");
  }
  if (operation == operation_insert ||
      (operation == operation_replace && replace_target == target_index))
  {
    index_expr = context.BuildInt("Index");
  }
  else if (operation == operation_replace && replace_target == target_persistent_id) {
    index_expr = context.BuildInt("Modifier ID");
  }
  const bool needs_string = operation != operation_replace || replace_target == target_name;
  const bool needs_index = operation == operation_insert ||
                           (operation == operation_replace &&
                            ELEM(replace_target, target_index, target_persistent_id));
  if ((needs_string && !string_expr) || (needs_index && !index_expr))
  {
    context.AddError("Modifier", "Assign Geometry Nodes Modifier requires a valid target input");
    return;
  }

  uint32_t result_property_ref_index = LN_INVALID_INDEX;
  if (context.OutputIsLinked("Modifier ID")) {
    result_property_ref_index = AddTreePropertyRef(
        context.program,
        AssignGeometryNodesModifierResultPropertyName(context.node),
        LN_ValueType::Int,
        MakeDefaultValue(LN_ValueType::Int));
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Assign Geometry Nodes Modifier flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::AssignGeometryNodesModifier;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = operation;
    instruction.secondary_int_value = replace_target;
    instruction.secondary_value_expr_index = node_group_expr;
    instruction.property_ref_index = result_property_ref_index;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (string_expr) {
      instruction.string_expr_index = *string_expr;
    }
    if (index_expr) {
      instruction.int_expr_index = *index_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileDrawLineNode(CompilerContext &context)
{
  const std::optional<uint32_t> color_expr = context.BuildColor("Color");
  const std::optional<uint32_t> from_expr = context.BuildVector("From");
  const std::optional<uint32_t> to_expr = context.BuildVector("To");
  if (!color_expr || !from_expr || !to_expr) {
    context.AddError("To", "Draw Line requires color, from, and to inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Draw Line flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::DrawLine;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.color_expr_index = *color_expr;
    instruction.vector_expr_index = *from_expr;
    instruction.secondary_vector_expr_index = *to_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

static void CompileDrawBoxLikeNode(CompilerContext &context, const bool cube)
{
  const std::optional<uint32_t> color_expr = context.BuildColor("Color");
  const std::optional<uint32_t> origin_expr = context.BuildVector("Origin");
  const std::optional<uint32_t> width_expr = context.BuildFloat("Width");
  const std::optional<uint32_t> length_expr = cube ? width_expr : context.BuildFloat("Length");
  const std::optional<uint32_t> height_expr = cube ? width_expr : context.BuildFloat("Height");
  if (!color_expr || !origin_expr || !width_expr || !length_expr || !height_expr) {
    context.AddError("Width", cube ? "Draw Cube requires color, origin, and width" :
                                     "Draw Box requires color, origin, width, length, and height");
    return;
  }
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(cube ? "Draw Cube flow must be driven by an event or branch" :
             "Draw Box flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::DrawBox;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.color_expr_index = *color_expr;
    instruction.vector_expr_index = *origin_expr;
    instruction.float_expr_index = *width_expr;
    instruction.secondary_float_expr_index = *length_expr;
    instruction.tertiary_float_expr_index = *height_expr;
    instruction.bool_value = context.node.custom2 != 0;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileDrawCubeNode(CompilerContext &context)
{
  CompileDrawBoxLikeNode(context, true);
}

void CompileDrawBoxNode(CompilerContext &context)
{
  CompileDrawBoxLikeNode(context, false);
}

void CompileDrawNode(CompilerContext &context)
{
  if (context.node.custom1 == 0 || context.node.custom1 == 1) {
    const std::optional<uint32_t> color_expr = context.BuildColor("Color");
    const std::optional<uint32_t> origin_expr = context.BuildVector("Origin");
    const std::optional<uint32_t> target_expr = context.BuildVector("Target");
    if (!color_expr || !origin_expr || !target_expr) {
      context.AddError("Target", "Draw line mode requires color, origin, and target inputs");
      return;
    }
    const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Draw flow must be driven by an event or branch");
    if (!flow_events) {
      return;
    }
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = context.node.custom1 == 1 ? LN_OpCode::DrawArrow : LN_OpCode::DrawLine;
      instruction.source_ref_index = context.MakeSourceRef("Flow");
      instruction.color_expr_index = *color_expr;
      instruction.vector_expr_index = *origin_expr;
      instruction.secondary_vector_expr_index = *target_expr;
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }
  if (context.node.custom1 == 2) {
    const std::optional<uint32_t> color_expr = context.BuildColor("Color");
    const std::optional<uint32_t> points_expr = context.BuildValueExpression("Points");
    if (!color_expr || !points_expr) {
      context.AddError("Points", "Draw path mode requires color and points inputs");
      return;
    }
    const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Draw flow must be driven by an event or branch");
    if (!flow_events) {
      return;
    }
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::DrawPath;
      instruction.source_ref_index = context.MakeSourceRef("Flow");
      instruction.color_expr_index = *color_expr;
      instruction.value_expr_index = *points_expr;
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }
  if (context.node.custom1 == 3 || context.node.custom1 == 4) {
    CompileDrawBoxLikeNode(context, context.node.custom1 == 3);
    return;
  }
  if (context.node.custom1 == 5) {
    const std::optional<uint32_t> color_expr = context.BuildColor("Color");
    if (!color_expr) {
      context.AddError("Color", "Draw mesh mode requires a color input");
      return;
    }
    const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Draw flow must be driven by an event or branch");
    if (!flow_events) {
      return;
    }
    const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::DrawMesh;
      instruction.source_ref_index = context.MakeSourceRef("Flow");
      instruction.color_expr_index = *color_expr;
      instruction.value_expr_index = object_expr.value_or(LN_INVALID_INDEX);
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }
  if (context.node.custom1 == 6) {
    const std::optional<uint32_t> length_expr = context.BuildFloat("Length");
    if (!length_expr) {
      context.AddError("Length", "Draw axis mode requires a length input");
      return;
    }
    const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Draw flow must be driven by an event or branch");
    if (!flow_events) {
      return;
    }
    const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::DrawAxis;
      instruction.source_ref_index = context.MakeSourceRef("Flow");
      instruction.float_expr_index = *length_expr;
      instruction.value_expr_index = object_expr.value_or(LN_INVALID_INDEX);
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }
  context.AddError("Mode", "Unsupported Draw mode");
}

void CompileUnsupportedCommandNode(CompilerContext &context)
{
  context.AddError("Node", "This native node is registered but does not have a runtime command implementation yet");
}

void CompileGetObjectAttributeNode(CompilerContext &context)
{
  /* GetObjectAttribute is a MainThreadOnly parameter node.
   * It is compiled as a value expression in the condition expression switch. */
}

void CompileRotateTowardNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> target_expr = context.BuildVector("Target");
  const std::optional<uint32_t> factor_expr = context.BuildFloat("Factor");
  const std::optional<uint32_t> rot_axis_expr = context.BuildInt("Rot Axis");
  const std::optional<uint32_t> front_axis_expr = context.BuildInt("Front Axis");
  if (!target_expr || !factor_expr || !rot_axis_expr || !front_axis_expr) {
    context.AddError("Target",
                     "Rotate To requires target, factor, rotation axis, and front axis inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Rotate To flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RotateToward;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.vector_expr_index = *target_expr;
    instruction.float_expr_index = *factor_expr;
    instruction.int_expr_index = *rot_axis_expr;
    instruction.secondary_int_expr_index = *front_axis_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetObjectAttributeNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Attribute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  if (SetObjectAttributeUsesTransformInput(context.node.custom1)) {
    const std::optional<uint32_t> position_expr = context.BuildVector("Position");
    const std::optional<uint32_t> rotation_expr = context.BuildRotation("Rotation");
    const std::optional<uint32_t> scale_expr = context.BuildVector("Scale");
    if (!position_expr || !rotation_expr || !scale_expr) {
      context.AddError("Position",
                       "Set Attribute transform requires position, rotation, and scale inputs");
      return;
    }

    const bool local = context.node.custom1 == 10;
    const LN_OpCode position_opcode = local ? LN_OpCode::SetLocalPosition :
                                             LN_OpCode::SetWorldPosition;
    const LN_OpCode orientation_opcode = local ? LN_OpCode::SetLocalOrientation :
                                                LN_OpCode::SetWorldOrientation;
    const LN_OpCode scale_opcode = local ? LN_OpCode::SetLocalScale : LN_OpCode::SetWorldScale;
    const MT_Vector3 position_fallback = VectorExpressionConstantFallback(context.program,
                                                                          *position_expr);
    const MT_Vector3 rotation_fallback = VectorExpressionConstantFallback(context.program,
                                                                          *rotation_expr);
    const MT_Vector3 scale_fallback = VectorExpressionConstantFallback(context.program,
                                                                       *scale_expr);
    for (const FlowPath &path : flow_events->paths) {
      AppendSetObjectAttributeVectorInstruction(
          context, path, object_expr, position_opcode, "Position", *position_expr, position_fallback);
      AppendSetObjectAttributeVectorInstruction(
          context, path, object_expr, orientation_opcode, "Rotation", *rotation_expr, rotation_fallback);
      AppendSetObjectAttributeVectorInstruction(
          context, path, object_expr, scale_opcode, "Scale", *scale_expr, scale_fallback);
    }
    return;
  }

  const std::optional<LN_OpCode> opcode = SetObjectAttributeOpcode(context.node.custom1);
  if (!opcode) {
    context.AddError("Attribute Type", "Unsupported Set Attribute type");
    return;
  }

  if (context.node.custom1 == 11) {
    const std::optional<uint32_t> color_expr = BuildMaskedSetObjectAttributeColorExpression(
        context, object_expr);
    if (!color_expr) {
      return;
    }

    const MT_Vector4 fallback = ColorExpressionConstantFallback(context.program, *color_expr);
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = *opcode;
      instruction.source_ref_index = context.MakeSourceRef("Color");
      if (object_expr) {
        instruction.value_expr_index = *object_expr;
      }
      instruction.color_expr_index = *color_expr;
      instruction.color_value = fallback;
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }

  if (context.node.custom1 == 12) {
    const std::optional<uint32_t> visible_expr = context.BuildBool("Visible");
    const std::optional<uint32_t> include_children_expr = context.BuildBool("Include Children");
    if (!visible_expr || !include_children_expr) {
      context.AddError("Visible", "Set Attribute visibility requires boolean inputs");
      return;
    }

    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = *opcode;
      instruction.source_ref_index = context.MakeSourceRef("Visible");
      if (object_expr) {
        instruction.value_expr_index = *object_expr;
      }
      instruction.bool_expr_index = *visible_expr;
      instruction.secondary_bool_expr_index = *include_children_expr;
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }

  const std::optional<uint32_t> vector_expr = BuildSetObjectAttributeVectorExpression(context,
                                                                                     object_expr);
  if (!vector_expr) {
    return;
  }

  const MT_Vector3 fallback = VectorExpressionConstantFallback(context.program, *vector_expr);
  for (const FlowPath &path : flow_events->paths) {
    AppendSetObjectAttributeVectorInstruction(
        context, path, object_expr, *opcode, "Value", *vector_expr, fallback);
  }
}

void CompileSetRigidBodyAttributeNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(
          "Set Rigid Body Attribute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::SetRigidBodyAttribute;
  instruction.source_ref_index = context.MakeSourceRef("Flow");
  instruction.int_value = context.node.custom1;
  if (object_expr) {
    instruction.value_expr_index = *object_expr;
  }

  switch (static_cast<LN_RigidBodyAttribute>(context.node.custom1)) {
    case LN_RigidBodyAttribute::Mass:
    case LN_RigidBodyAttribute::Friction:
    case LN_RigidBodyAttribute::Restitution:
    case LN_RigidBodyAttribute::MinLinearVelocity:
    case LN_RigidBodyAttribute::MaxLinearVelocity:
    case LN_RigidBodyAttribute::MinAngularVelocity:
    case LN_RigidBodyAttribute::MaxAngularVelocity:
    case LN_RigidBodyAttribute::GravityFactor: {
      const std::optional<uint32_t> value_expr = context.BuildFloat("Value");
      if (!value_expr) {
        context.AddError("Value", "Set Rigid Body Attribute requires a float value input");
        return;
      }
      instruction.float_expr_index = *value_expr;
      instruction.vector_value = MT_Vector3(FloatExpressionConstantFallback(context.program,
                                                                            *value_expr),
                                            0.0f,
                                            0.0f);
      break;
    }
    case LN_RigidBodyAttribute::Damping: {
      const std::optional<uint32_t> linear_expr = context.BuildFloat("Linear Damping");
      const std::optional<uint32_t> angular_expr = context.BuildFloat("Angular Damping");
      if (!linear_expr || !angular_expr) {
        context.AddError("Linear Damping",
                         "Set Rigid Body Attribute damping requires linear and angular damping");
        return;
      }
      instruction.float_expr_index = *linear_expr;
      instruction.secondary_float_expr_index = *angular_expr;
      instruction.vector_value = MT_Vector3(FloatExpressionConstantFallback(context.program,
                                                                            *linear_expr),
                                            FloatExpressionConstantFallback(context.program,
                                                                            *angular_expr),
                                            0.0f);
      break;
    }
    case LN_RigidBodyAttribute::Ccd:
    case LN_RigidBodyAttribute::AllowPhysicsRotation: {
      const std::optional<uint32_t> enabled_expr = context.BuildBool("Enabled");
      if (!enabled_expr) {
        context.AddError("Enabled", "Set Rigid Body Attribute requires a boolean enabled input");
        return;
      }
      instruction.bool_expr_index = *enabled_expr;
      instruction.bool_value = BoolExpressionConstantFallback(context.program, *enabled_expr);
      break;
    }
    case LN_RigidBodyAttribute::Sleeping: {
      const std::optional<uint32_t> allow_expr = context.BuildBool("Allow Sleeping");
      const std::optional<uint32_t> wake_expr = context.BuildBool("Wake");
      if (!allow_expr || !wake_expr) {
        context.AddError("Allow Sleeping",
                         "Set Rigid Body Attribute sleeping requires allow and wake inputs");
        return;
      }
      instruction.bool_expr_index = *allow_expr;
      instruction.secondary_bool_expr_index = *wake_expr;
      instruction.bool_value = BoolExpressionConstantFallback(context.program, *allow_expr);
      instruction.secondary_int_value =
          BoolExpressionConstantFallback(context.program, *wake_expr) ? 1 : 0;
      break;
    }
    case LN_RigidBodyAttribute::AxisLocks: {
      const std::optional<uint32_t> translation_expr = context.BuildVector("Lock Translation");
      const std::optional<uint32_t> rotation_expr = context.BuildVector("Lock Rotation");
      if (!translation_expr || !rotation_expr) {
        context.AddError("Lock Translation",
                         "Set Rigid Body Attribute axis locks require translation and rotation locks");
        return;
      }
      instruction.vector_expr_index = *translation_expr;
      instruction.secondary_vector_expr_index = *rotation_expr;
      instruction.vector_value = VectorExpressionConstantFallback(context.program,
                                                                  *translation_expr);
      instruction.secondary_vector_value = VectorExpressionConstantFallback(context.program,
                                                                            *rotation_expr);
      break;
    }
  }

  for (const FlowPath &path : flow_events->paths) {
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStartLogicTreeNode(CompilerContext &context)
{
  const std::optional<uint32_t> tree_expr = context.BuildString("Tree Name");
  if (!tree_expr) {
    context.AddError("Tree Name", "Start Logic Tree requires a tree name input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Start Logic Tree flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetLogicTreeEnabled;
    instruction.bool_value = true;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *tree_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStopLogicTreeNode(CompilerContext &context)
{
  const std::optional<uint32_t> tree_expr = context.BuildString("Tree Name");
  if (!tree_expr) {
    context.AddError("Tree Name", "Stop Logic Tree requires a tree name input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Stop Logic Tree flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetLogicTreeEnabled;
    instruction.bool_value = false;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *tree_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileRunLogicTreeNode(CompilerContext &context)
{
  const std::optional<uint32_t> tree_expr = context.BuildString("Tree Name");
  if (!tree_expr) {
    context.AddError("Tree Name", "Run Logic Tree requires a tree name input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Run Logic Tree flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RunLogicTreeOnce;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *tree_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileInstallLogicTreeNode(CompilerContext &context)
{
  const std::optional<uint32_t> tree_expr = context.BuildString("Tree Name");
  if (!tree_expr) {
    context.AddError("Tree Name", "Install Logic Tree requires a tree name input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");

  bool default_init = true;
  if (const std::optional<LN_Value> init_val = context.ReadValue("Initialize", LN_ValueType::Bool)) {
    if (init_val->type == LN_ValueType::Bool) {
      default_init = init_val->bool_value;
    }
  }
  const std::optional<uint32_t> init_expr = context.BuildBool("Initialize");

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Install Logic Tree flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::InstallLogicTree;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *tree_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    if (init_expr) {
      instruction.bool_expr_index = *init_expr;
    }
    instruction.bool_value = default_init;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCameraNode(CompilerContext &context)
{
  const std::optional<uint32_t> camera_expr = context.BuildCameraOrActive("Camera");
  if (!camera_expr) {
    context.AddError("Camera", "Set Camera requires a camera input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Camera flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetActiveCamera;
    instruction.source_ref_index = context.MakeSourceRef("Camera");
    instruction.value_expr_index = *camera_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCameraFovNode(CompilerContext &context)
{
  CompileCameraFloatCommand(context,
                            LN_OpCode::SetCameraFov,
                            "FOV",
                            "Set FOV requires a camera input",
                            "Set FOV requires a float FOV input",
                            "Set FOV flow must be driven by an event or branch");
}

void CompileSetCameraOrthoScaleNode(CompilerContext &context)
{
  CompileCameraFloatCommand(
      context,
      LN_OpCode::SetCameraOrthoScale,
      "Scale",
      "Set Orthographic Scale requires a camera input",
      "Set Orthographic Scale requires a float scale input",
      "Set Orthographic Scale flow must be driven by an event or branch");
}

void CompileSetLightPowerNode(CompilerContext &context)
{
  const std::optional<uint32_t> power_expr = context.BuildFloat("Power");
  if (!power_expr) {
    context.AddError("Power", "Set Light Power requires a float power input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Light Power flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const float fallback = FloatExpressionConstantFallback(context.program, *power_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetLightPower;
    instruction.source_ref_index = context.MakeSourceRef("Power");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.float_expr_index = *power_expr;
    instruction.vector_value = MT_Vector3(fallback, 0.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetResolutionNode(CompilerContext &context)
{
  const std::optional<uint32_t> width_expr = context.BuildInt("X");
  const std::optional<uint32_t> height_expr = context.BuildInt("Y");
  if (!width_expr || !height_expr) {
    context.AddError(!width_expr ? "X" : "Y",
                     !width_expr ? "Set Resolution requires an integer X input" :
                                   "Set Resolution requires an integer Y input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Resolution flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetWindowSize;
    instruction.source_ref_index = context.MakeSourceRef("X");
    instruction.int_expr_index = *width_expr;
    instruction.secondary_int_expr_index = *height_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetVSyncNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set VSync flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t mode_expr = AddConstantIntExpression(context.program, int32_t(context.node.custom1));
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetVSync;
    instruction.source_ref_index = context.MakeSourceRef("Mode");
    instruction.int_expr_index = mode_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileTogglePropertyNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Toggle Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Toggle Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::Bool;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Bool, default_value);
  LN_BoolExpression current_value;
  current_value.kind = LN_BoolExpressionKind::SnapshotGameProperty;
  current_value.property_ref_index = property_ref_index;
  const uint32_t current_expr = context.program.AddBoolExpression(current_value);
  const uint32_t toggled_expr = AddNotBoolExpression(context.program, current_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Property");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Bool;
    instruction.bool_expr_index = toggled_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

static std::optional<uint32_t> BuildModifyPropertyOperationExpression(CompilerContext &context,
                                                                      const uint32_t current_expr,
                                                                      const uint32_t value_expr)
{
  LN_FloatExpression expression;
  expression.input0 = current_expr;
  expression.input1 = value_expr;

  switch (context.node.custom1) {
    case blender::NODE_MATH_ADD:
      expression.kind = LN_FloatExpressionKind::Add;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_SUBTRACT:
      expression.kind = LN_FloatExpressionKind::Subtract;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_MULTIPLY:
      expression.kind = LN_FloatExpressionKind::Multiply;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_DIVIDE:
      expression.kind = LN_FloatExpressionKind::Divide;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_POWER:
      expression.kind = LN_FloatExpressionKind::Power;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_MODULO:
      expression.kind = LN_FloatExpressionKind::Modulo;
      return context.program.AddFloatExpression(expression);
    case blender::NODE_MATH_FLOOR: {
      expression.kind = LN_FloatExpressionKind::Divide;
      const uint32_t divide_expr = context.program.AddFloatExpression(expression);
      LN_FloatExpression floor_expr;
      floor_expr.kind = LN_FloatExpressionKind::Floor;
      floor_expr.input0 = divide_expr;
      return context.program.AddFloatExpression(floor_expr);
    }
    default:
      context.AddError("Operation", "Modify Property uses an unsupported operation");
      return std::nullopt;
  }
}

void CompileModifyPropertyImpl(CompilerContext &context, bool force_clamped)
{
  const char *node_name = force_clamped ? "Modify Property Clamped" : "Modify Property";
  const bool clamped = force_clamped || (context.node.custom2 & modify_property_clamp) != 0;
  if ((context.node.custom2 & modify_property_mode_attribute) != 0) {
    context.AddError("Mode",
                     "Modify Property Attribute mode is not supported by native logic nodes; use "
                     "Set Object Attribute for object attributes");
    return;
  }

  std::optional<std::string> name = ReadRequiredConstantString(
      context, "Property", std::string(node_name) + " requires a constant property name");
  if (!name) {
    name = ReadRequiredConstantString(
        context, "Name", std::string(node_name) + " requires a constant property name");
  }
  if (!name) {
    return;
  }

  const std::optional<uint32_t> delta_expr = context.BuildFloat("Value");
  if (!delta_expr) {
    context.AddError("Value", std::string(node_name) + " requires a float value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(std::string(node_name) + " flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::Float;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Float, default_value);
  LN_FloatExpression current_value;
  current_value.kind = LN_FloatExpressionKind::SnapshotGameProperty;
  current_value.property_ref_index = property_ref_index;
  const uint32_t current_expr = context.program.AddFloatExpression(current_value);

  std::optional<uint32_t> final_expr = BuildModifyPropertyOperationExpression(
      context, current_expr, *delta_expr);
  if (!final_expr) {
    return;
  }

  if (clamped) {
    const std::optional<uint32_t> min_expr = context.BuildFloat("Min");
    const std::optional<uint32_t> max_expr = context.BuildFloat("Max");
    if (!min_expr || !max_expr) {
      context.AddError("Min", "Modify Property Clamped requires min and max inputs");
      return;
    }
    LN_FloatExpression clamp_expr;
    clamp_expr.kind = LN_FloatExpressionKind::Clamp;
    clamp_expr.input0 = *final_expr;
    clamp_expr.input1 = *min_expr;
    clamp_expr.input2 = *max_expr;
    final_expr = context.program.AddFloatExpression(clamp_expr);
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Float;
    instruction.float_expr_index = *final_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileModifyPropertyNode(CompilerContext &context)
{
  CompileModifyPropertyImpl(context, false);
}

void CompileModifyPropertyClampedNode(CompilerContext &context)
{
  CompileModifyPropertyImpl(context, true);
}

void CompileAddObjectNode(CompilerContext &context)
{
  const std::optional<uint32_t> source_object_expr = context.BuildValueExpression("Object to Add");
  const std::optional<uint32_t> copy_transform_expr = context.BuildOptionalObject("Copy Transform");
  const std::optional<uint32_t> life_expr = context.BuildFloat("Life");
  const std::optional<uint32_t> full_copy_expr = context.BuildBool("Full Copy");
  if (!source_object_expr || !life_expr || !full_copy_expr) {
    context.AddError("Object to Add", "Add Object requires object, life, and full copy inputs");
    return;
  }

  uint32_t result_property_ref_index = LN_INVALID_INDEX;
  if (context.OutputIsLinked("Added Object") || context.OutputIsLinked("Object")) {
    result_property_ref_index = AddTreePropertyRef(context.program,
                                                   AddObjectResultPropertyName(context.node),
                                                   LN_ValueType::ObjectRef,
                                                   MakeDefaultValue(LN_ValueType::ObjectRef));
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Add Object flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::AddObject;
    instruction.source_ref_index = context.MakeSourceRef("Object to Add");
    instruction.float_expr_index = *life_expr;
    instruction.bool_expr_index = *full_copy_expr;
    instruction.property_ref_index = result_property_ref_index;
    instruction.secondary_value_expr_index = *source_object_expr;
    if (copy_transform_expr) {
      instruction.value_expr_index = *copy_transform_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetParentNode(CompilerContext &context)
{
  const std::optional<uint32_t> child_expr = context.BuildOptionalObject("Child Object");
  const std::optional<uint32_t> parent_expr = context.BuildValueExpression("Parent Object");
  const std::optional<uint32_t> compound_expr = context.BuildBool("Compound");
  const std::optional<uint32_t> ghost_expr = context.BuildBool("Ghost");
  if (!parent_expr || !compound_expr || !ghost_expr) {
    context.AddError("Parent Object", "Set Parent requires parent, compound, and ghost inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Parent flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetParent;
    instruction.source_ref_index = context.MakeSourceRef("Parent Object");
    instruction.bool_expr_index = *compound_expr;
    instruction.secondary_bool_expr_index = *ghost_expr;
    instruction.secondary_value_expr_index = *parent_expr;
    if (child_expr) {
      instruction.value_expr_index = *child_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileApplyImpulseNode(CompilerContext &context)
{
  const std::optional<uint32_t> attach_expr = context.BuildVector("Attach");
  if (!attach_expr) {
    context.AddError("Attach", "Apply Impulse requires a vector attach input");
    return;
  }

  const std::optional<uint32_t> impulse_expr = context.BuildVector("Impulse");
  if (!impulse_expr) {
    context.AddError("Impulse", "Apply Impulse requires a vector impulse input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Apply Impulse flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const MT_Vector3 impulse_fallback = VectorExpressionConstantFallback(context.program,
                                                                       *impulse_expr);
  const MT_Vector3 attach_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *attach_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ApplyImpulse;
    instruction.source_ref_index = context.MakeSourceRef("Impulse");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *impulse_expr;
    instruction.secondary_vector_expr_index = *attach_expr;
    instruction.vector_value = impulse_fallback;
    instruction.secondary_vector_value = attach_fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileApplyForceToTargetNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> target_position_expr = context.BuildVector("Target Position");
  const std::optional<uint32_t> force_expr = context.BuildFloat("Force");
  const std::optional<uint32_t> stop_distance_expr = context.BuildFloat("Stop Distance");
  const std::optional<uint32_t> reached_threshold_expr = context.BuildFloat("Reached Threshold");
  if (!target_position_expr || !force_expr || !stop_distance_expr) {
    context.AddError("Target Position",
                     "Apply Force To Target requires target position, force, and stop distance inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Apply Force To Target flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  std::vector<uint32_t> done_outputs;
  std::vector<uint32_t> reached_outputs;
  const MT_Vector3 target_position_fallback = VectorExpressionConstantFallback(
      context.program, *target_position_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ApplyForceToTarget;
    instruction.source_ref_index = context.MakeSourceRef("Force");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *target_position_expr;
    instruction.float_expr_index = *force_expr;
    instruction.secondary_float_expr_index = reached_threshold_expr.value_or(*stop_distance_expr);
    instruction.tertiary_float_expr_index = *stop_distance_expr;
    instruction.vector_value = target_position_fallback;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);

    LN_BoolExpression done_expression;
    done_expression.kind = LN_BoolExpressionKind::InstructionExecuted;
    done_expression.input0 = instruction_index;
    done_outputs.push_back(context.program.AddBoolExpression(done_expression));

    LN_BoolExpression reached_expression;
    reached_expression.kind = LN_BoolExpressionKind::InstructionReached;
    reached_expression.input0 = instruction_index;
    reached_outputs.push_back(context.program.AddBoolExpression(reached_expression));
  }

  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
  const uint32_t reached_expression = CombineBoolExpressionsWithOr(context.program,
                                                                   reached_outputs);
  context.CacheBoolOutputPin("Reached", reached_expression);
  context.CacheBoolOutputPin("When Reached", reached_expression);
}

void CompileSetGamePropertyIntNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Set Game Property Int requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildInt("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Int requires an integer value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Game Property Int must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::Int;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Int, default_value);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Int;
    instruction.int_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGamePropertyFloatNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Set Game Property Float requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildFloat("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Float requires a float value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Game Property Float must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::Float;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Float, default_value);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Float;
    instruction.float_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGamePropertyBoolNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Set Game Property Bool requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildBool("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Bool requires a boolean value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Game Property Bool must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::Bool;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Bool, default_value);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Bool;
    instruction.bool_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGamePropertyStringNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Set Game Property String requires a constant property name");
  if (!name) {
    return;
  }

  std::optional<uint32_t> value_expr = context.BuildString("Value");
  if (!value_expr) {
    if (const std::optional<uint32_t> generic_expr = context.BuildValueExpression("Value")) {
      LN_StringExpression string_expr;
      string_expr.kind = LN_StringExpressionKind::FromGenericValue;
      string_expr.value_expr_index = *generic_expr;
      value_expr = context.program.AddStringExpression(string_expr);
    }
  }
  if (!value_expr) {
    context.AddError("Value", "Set Game Property String requires a string value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Game Property String must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::String;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::String, default_value);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::String;
    instruction.string_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetTreePropertyNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context,
      "Property",
      "Name",
      "Set Tree Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Tree Property requires a valid value input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Tree Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t property_ref_index = AddTreePropertyRef(
      context.program, *name, LN_ValueType::Float, MakeDefaultValue(LN_ValueType::Float));
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetTreeProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.value_expr_index = *value_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCursorPositionNode(CompilerContext &context)
{
  const std::optional<uint32_t> screen_x_expr = context.BuildFloat("Screen X");
  const std::optional<uint32_t> screen_y_expr = context.BuildFloat("Screen Y");
  if (!screen_x_expr || !screen_y_expr) {
    context.AddError("Screen X", "Set Cursor Position requires float screen coordinates");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Cursor Position flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression position_expr;
  position_expr.kind = LN_VectorExpressionKind::Combine;
  position_expr.input0 = *screen_x_expr;
  position_expr.input1 = *screen_y_expr;
  position_expr.input2 = AddConstantFloatExpression(context.program, 0.0f);
  position_expr.vector_value = MT_Vector3(
      FloatExpressionConstantFallback(context.program, *screen_x_expr),
      FloatExpressionConstantFallback(context.program, *screen_y_expr),
      0.0f);
  const uint32_t vector_expr_index = context.program.AddVectorExpression(position_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCursorPosition;
    instruction.source_ref_index = context.MakeSourceRef("Screen X");
    instruction.vector_expr_index = vector_expr_index;
    instruction.vector_value = position_expr.vector_value;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileGamepadVibrationNode(CompilerContext &context)
{
  const std::optional<uint32_t> index_expr = context.BuildInt("Index");
  const std::optional<uint32_t> left_expr = context.BuildFloat("Left");
  const std::optional<uint32_t> right_expr = context.BuildFloat("Right");
  const std::optional<uint32_t> time_expr = context.BuildFloat("Time");
  if (!index_expr || !left_expr || !right_expr || !time_expr) {
    context.AddError("Index", "Gamepad Vibration requires gamepad index and strength inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Gamepad Vibration flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression vibration_expr;
  vibration_expr.kind = LN_VectorExpressionKind::Combine;
  vibration_expr.input0 = *left_expr;
  vibration_expr.input1 = *right_expr;
  vibration_expr.input2 = *time_expr;
  vibration_expr.vector_value = MT_Vector3(
      FloatExpressionConstantFallback(context.program, *left_expr),
      FloatExpressionConstantFallback(context.program, *right_expr),
      FloatExpressionConstantFallback(context.program, *time_expr));
  const uint32_t vibration_expr_index = context.program.AddVectorExpression(vibration_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGamepadVibration;
    instruction.source_ref_index = context.MakeSourceRef("Left");
    instruction.int_expr_index = *index_expr;
    instruction.vector_expr_index = vibration_expr_index;
    instruction.vector_value = vibration_expr.vector_value;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileGamepadLookNode(CompilerContext &context)
{
  const std::optional<uint32_t> body_expr = context.BuildValueExpression("Body Object");
  const std::optional<uint32_t> head_expr = context.BuildValueExpression("Head Object");
  const std::optional<uint32_t> invert_expr = context.BuildVector("Inverted");
  const std::optional<uint32_t> index_expr = context.BuildInt("Index");
  const std::optional<uint32_t> sensitivity_expr = context.BuildFloat("Sensitivity");
  const std::optional<uint32_t> exponent_expr = context.BuildFloat("Exponent");
  const std::optional<uint32_t> use_cap_x_expr = context.BuildBool("Cap Left / Right");
  const std::optional<uint32_t> cap_x_expr = context.BuildVector("Left / Right Range");
  const std::optional<uint32_t> use_cap_y_expr = context.BuildBool("Cap Up / Down");
  const std::optional<uint32_t> cap_y_expr = context.BuildVector("Up / Down Range");
  const std::optional<uint32_t> threshold_expr = context.BuildFloat("Threshold");
  if (!body_expr || !head_expr || !invert_expr || !index_expr || !sensitivity_expr || !exponent_expr ||
      !use_cap_x_expr || !cap_x_expr || !use_cap_y_expr || !cap_y_expr || !threshold_expr)
  {
    context.AddError("Index", "Gamepad Look requires object and numeric inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Gamepad Look flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression stick_expr;
  stick_expr.kind = LN_VectorExpressionKind::GamepadStick;
  stick_expr.input0 = *index_expr;
  stick_expr.input1 = *threshold_expr;
  stick_expr.input2 = uint32_t(context.node.custom1 & 1);
  stick_expr.float_expr_index = AddConstantFloatExpression(context.program, 1.0f);
  stick_expr.float_value = 1.0f;
  const uint32_t stick_expr_index = context.program.AddVectorExpression(stick_expr);

  const MT_Vector3 invert_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *invert_expr);
  const MT_Vector3 cap_x_fallback = VectorExpressionConstantFallback(context.program,
                                                                     *cap_x_expr);
  const MT_Vector3 cap_y_fallback = VectorExpressionConstantFallback(context.program,
                                                                     *cap_y_expr);
  const float sensitivity_fallback = FloatExpressionConstantFallback(context.program,
                                                                     *sensitivity_expr);
  const float exponent_fallback = FloatExpressionConstantFallback(context.program,
                                                                  *exponent_expr);
  const bool use_cap_x_fallback = BoolExpressionConstantFallback(context.program,
                                                                 *use_cap_x_expr);
  const bool use_cap_y_fallback = BoolExpressionConstantFallback(context.program,
                                                                 *use_cap_y_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::GamepadLook;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.value_expr_index = *body_expr;
    instruction.property_ref_index = *head_expr;
    instruction.vector_expr_index = stick_expr_index;
    instruction.secondary_vector_expr_index = *invert_expr;
    instruction.color_expr_index = *cap_x_expr;
    instruction.int_expr_index = *cap_y_expr;
    instruction.bool_expr_index = *use_cap_x_expr;
    instruction.secondary_bool_expr_index = *use_cap_y_expr;
    instruction.float_expr_index = *sensitivity_expr;
    instruction.string_expr_index = *exponent_expr;
    instruction.vector_value = MT_Vector3(
        invert_fallback.x(), invert_fallback.y(), sensitivity_fallback);
    instruction.secondary_vector_value = MT_Vector3(
        cap_x_fallback.x(), cap_x_fallback.y(), use_cap_x_fallback ? 1.0f : 0.0f);
    instruction.color_value = MT_Vector4(cap_y_fallback.x(),
                                         cap_y_fallback.y(),
                                         exponent_fallback,
                                         use_cap_y_fallback ? 1.0f : 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileMouseLookNode(CompilerContext &context)
{
  const std::optional<uint32_t> body_expr = context.BuildValueExpression("Body");
  const std::optional<uint32_t> head_expr = context.BuildValueExpression("Head");
  const std::optional<uint32_t> invert_expr = context.BuildVector("Inverted");
  const std::optional<uint32_t> sensitivity_expr = context.BuildFloat("Sensitivity");
  const std::optional<uint32_t> use_cap_x_expr = context.BuildBool("Cap Left / Right");
  const std::optional<uint32_t> cap_x_expr = context.BuildVector("Cap Left / Right Range");
  const std::optional<uint32_t> use_cap_y_expr = context.BuildBool("Cap Up / Down");
  const std::optional<uint32_t> cap_y_expr = context.BuildVector("Cap Up / Down Range");
  const std::optional<uint32_t> smoothing_expr = context.BuildFloat("Smoothing");
  if (!body_expr || !head_expr || !invert_expr || !sensitivity_expr || !use_cap_x_expr ||
      !cap_x_expr || !use_cap_y_expr || !cap_y_expr || !smoothing_expr)
  {
    context.AddError("Body", "Mouse Look requires body, head, and numeric inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Mouse Look flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const MT_Vector3 invert_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *invert_expr);
  const MT_Vector3 cap_x_fallback = VectorExpressionConstantFallback(context.program,
                                                                     *cap_x_expr);
  const MT_Vector3 cap_y_fallback = VectorExpressionConstantFallback(context.program,
                                                                     *cap_y_expr);
  const float sensitivity_fallback = FloatExpressionConstantFallback(context.program,
                                                                     *sensitivity_expr);
  const float smoothing_fallback = FloatExpressionConstantFallback(context.program,
                                                                   *smoothing_expr);
  const bool use_cap_x_fallback = BoolExpressionConstantFallback(context.program,
                                                                 *use_cap_x_expr);
  const bool use_cap_y_fallback = BoolExpressionConstantFallback(context.program,
                                                                 *use_cap_y_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::MouseLook;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.value_expr_index = *body_expr;
    instruction.property_ref_index = *head_expr;
    instruction.secondary_vector_expr_index = *invert_expr;
    instruction.color_expr_index = *cap_x_expr;
    instruction.int_expr_index = *cap_y_expr;
    instruction.bool_expr_index = *use_cap_x_expr;
    instruction.secondary_bool_expr_index = *use_cap_y_expr;
    instruction.float_expr_index = *sensitivity_expr;
    instruction.string_expr_index = *smoothing_expr;
    instruction.int_value = context.node.custom1;
    instruction.bool_value = (context.node.custom2 & 1) != 0;
    instruction.vector_value = MT_Vector3(
        invert_fallback.x(), invert_fallback.y(), sensitivity_fallback);
    instruction.secondary_vector_value = MT_Vector3(
        cap_x_fallback.x(), cap_x_fallback.y(), use_cap_x_fallback ? 1.0f : 0.0f);
    instruction.color_value = MT_Vector4(cap_y_fallback.x(),
                                         cap_y_fallback.y(),
                                         smoothing_fallback,
                                         use_cap_y_fallback ? 1.0f : 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

FlowPath GuardFlowPath(LN_Program &program, const FlowPath &path, const uint32_t guard_expr)
{
  FlowPath guarded_path = path;
  guarded_path.bool_guard_expr_index = (path.bool_guard_expr_index == LN_INVALID_INDEX) ?
                                           guard_expr :
                                           AddAndBoolExpression(program,
                                                                path.bool_guard_expr_index,
                                                                guard_expr);
  return guarded_path;
}

void CompileCharacterJumpNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Jump flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::CharacterJump;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCharacterGravityNode(CompilerContext &context)
{
  const std::optional<uint32_t> gravity_expr = context.BuildVector("Gravity");
  if (!gravity_expr) {
    context.AddError("Gravity", "Set Gravity requires a gravity vector input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Gravity flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const MT_Vector3 gravity_fallback = VectorExpressionConstantFallback(context.program,
                                                                       *gravity_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCharacterGravity;
    instruction.source_ref_index = context.MakeSourceRef("Gravity");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *gravity_expr;
    instruction.vector_value = gravity_fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCharacterJumpSpeedNode(CompilerContext &context)
{
  const std::optional<uint32_t> force_expr = context.BuildFloat("Force");
  if (!force_expr) {
    context.AddError("Force", "Set Jump Force requires a float force input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Jump Force flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const float force_fallback = FloatExpressionConstantFallback(context.program, *force_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCharacterJumpSpeed;
    instruction.source_ref_index = context.MakeSourceRef("Force");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.float_expr_index = *force_expr;
    instruction.vector_value = MT_Vector3(force_fallback, 0.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCharacterMaxJumpsNode(CompilerContext &context)
{
  const std::optional<uint32_t> max_jumps_expr = context.BuildInt("Max Jumps");
  if (!max_jumps_expr) {
    context.AddError("Max Jumps", "Set Max Jumps requires an integer max jumps input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Max Jumps flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const int32_t max_jumps_fallback = IntExpressionConstantFallback(context.program,
                                                                   *max_jumps_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCharacterMaxJumps;
    instruction.source_ref_index = context.MakeSourceRef("Max Jumps");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.int_expr_index = *max_jumps_expr;
    instruction.int_value = max_jumps_fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCharacterWalkDirectionNode(CompilerContext &context)
{
  const std::optional<uint32_t> vector_expr = context.BuildVector("Vector");
  if (!vector_expr) {
    context.AddError("Vector", "Walk requires a movement vector input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Walk flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t local_expr = AddConstantBoolExpression(context.program, context.node.custom1 != 0);
  const MT_Vector3 vector_fallback = VectorExpressionConstantFallback(context.program, *vector_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCharacterWalkDirection;
    instruction.source_ref_index = context.MakeSourceRef("Vector");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *vector_expr;
    instruction.bool_expr_index = local_expr;
    instruction.bool_value = context.node.custom1 != 0;
    instruction.vector_value = vector_fallback;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCharacterVelocityNode(CompilerContext &context)
{
  const std::optional<uint32_t> velocity_expr = context.BuildVector("Velocity");
  const std::optional<uint32_t> time_expr = context.BuildFloat("Time");
  if (!velocity_expr || !time_expr) {
    context.AddError(!velocity_expr ? "Velocity" : "Time",
                     !velocity_expr ? "Set Velocity requires a velocity vector input" :
                                      "Set Velocity requires a float time input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Velocity flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t local_expr = AddConstantBoolExpression(context.program, context.node.custom1 != 0);
  const MT_Vector3 velocity_fallback = VectorExpressionConstantFallback(context.program,
                                                                        *velocity_expr);
  const float time_fallback = FloatExpressionConstantFallback(context.program, *time_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCharacterVelocity;
    instruction.source_ref_index = context.MakeSourceRef("Velocity");
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    instruction.vector_expr_index = *velocity_expr;
    instruction.float_expr_index = *time_expr;
    instruction.bool_expr_index = local_expr;
    instruction.bool_value = context.node.custom1 != 0;
    instruction.vector_value = velocity_fallback;
    instruction.secondary_vector_value = MT_Vector3(time_fallback, 0.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileVehicleScalarCommand(CompilerContext &context,
                                 const LN_OpCode opcode,
                                 const char *value_socket,
                                 const char *input_error,
                                 const char *flow_error)
{
  const std::optional<uint32_t> vehicle_expr = context.BuildOptionalObject("Vehicle");
  const std::optional<uint32_t> wheels_expr = context.BuildInt("Wheels");
  const std::optional<uint32_t> value_expr = context.BuildFloat(value_socket);
  if (!wheels_expr || !value_expr) {
    context.AddError(!wheels_expr ? "Wheels" : value_socket,
                     !wheels_expr ? "Vehicle node requires an integer wheels input" : input_error);
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow(flow_error);
  if (!flow_events) {
    return;
  }

  const uint32_t axis_expr = AddConstantIntExpression(context.program, context.node.custom1);
  const int32_t wheels_fallback = IntExpressionConstantFallback(context.program, *wheels_expr);
  const float value_fallback = FloatExpressionConstantFallback(context.program, *value_expr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = opcode;
    instruction.source_ref_index = context.MakeSourceRef(value_socket);
    if (vehicle_expr) {
      instruction.value_expr_index = *vehicle_expr;
    }
    instruction.int_expr_index = *wheels_expr;
    instruction.secondary_int_expr_index = axis_expr;
    instruction.float_expr_index = *value_expr;
    instruction.int_value = wheels_fallback;
    instruction.secondary_int_value = context.node.custom1;
    instruction.vector_value = MT_Vector3(value_fallback, 0.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileVehicleControlNode(CompilerContext &context)
{
  const std::optional<uint32_t> vehicle_expr = context.BuildOptionalObject("Vehicle");
  const std::optional<uint32_t> throttle_expr = context.BuildFloat("Throttle");
  const std::optional<uint32_t> brake_expr = context.BuildFloat("Brake");
  const std::optional<uint32_t> handbrake_expr = context.BuildFloat("Handbrake");
  const std::optional<uint32_t> steering_expr = context.BuildFloat("Steering");
  if (!throttle_expr || !brake_expr || !handbrake_expr || !steering_expr) {
    context.AddError("Vehicle",
                     "Vehicle Control requires throttle, brake, handbrake, and steering inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Vehicle Control flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression control_expr;
  control_expr.kind = LN_VectorExpressionKind::Combine;
  control_expr.input0 = *throttle_expr;
  control_expr.input1 = *brake_expr;
  control_expr.input2 = *handbrake_expr;
  control_expr.vector_value = MT_Vector3(FloatExpressionConstantFallback(context.program,
                                                                         *throttle_expr),
                                         FloatExpressionConstantFallback(context.program,
                                                                         *brake_expr),
                                         FloatExpressionConstantFallback(context.program,
                                                                         *handbrake_expr));
  const uint32_t control_expr_index = context.program.AddVectorExpression(control_expr);
  const float steering_fallback = FloatExpressionConstantFallback(context.program, *steering_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::VehicleControl;
    instruction.source_ref_index = context.MakeSourceRef("Throttle");
    if (vehicle_expr) {
      instruction.value_expr_index = *vehicle_expr;
    }
    instruction.vector_expr_index = control_expr_index;
    instruction.float_expr_index = *steering_expr;
    instruction.vector_value = control_expr.vector_value;
    instruction.secondary_vector_value = MT_Vector3(steering_fallback, 0.0f, 0.0f);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileVehicleAccelerateNode(CompilerContext &context)
{
  CompileVehicleScalarCommand(context,
                              LN_OpCode::VehicleApplyEngineForce,
                              "Power",
                              "Accelerate requires a float power input",
                              "Accelerate flow must be driven by an event or branch");
}

void CompileVehicleBrakeNode(CompilerContext &context)
{
  CompileVehicleScalarCommand(context,
                              LN_OpCode::VehicleApplyBraking,
                              "Power",
                              "Brake requires a float power input",
                              "Brake flow must be driven by an event or branch");
}

void CompileVehicleSteerNode(CompilerContext &context)
{
  CompileVehicleScalarCommand(context,
                              LN_OpCode::VehicleApplySteering,
                              "Steer",
                              "Steer requires a float steer input",
                              "Steer flow must be driven by an event or branch");
}

void CompileVehicleSetAttributesNode(CompilerContext &context)
{
  const std::optional<uint32_t> vehicle_expr = context.BuildOptionalObject("Vehicle");
  const std::optional<uint32_t> wheels_expr = context.BuildInt("Wheels");
  const std::optional<uint32_t> suspension_enabled_expr = context.BuildBool("Suspension");
  const std::optional<uint32_t> suspension_value_expr = context.BuildFloat("Suspension Value");
  const std::optional<uint32_t> stiffness_enabled_expr = context.BuildBool("Stiffness");
  const std::optional<uint32_t> stiffness_value_expr = context.BuildFloat("Stiffness Value");
  const std::optional<uint32_t> damping_enabled_expr = context.BuildBool("Damping");
  const std::optional<uint32_t> damping_value_expr = context.BuildFloat("Damping Value");
  const std::optional<uint32_t> friction_enabled_expr = context.BuildBool("Friction");
  const std::optional<uint32_t> friction_value_expr = context.BuildFloat("Friction Value");
  if (!wheels_expr || !suspension_enabled_expr || !suspension_value_expr || !stiffness_enabled_expr ||
      !stiffness_value_expr || !damping_enabled_expr || !damping_value_expr ||
      !friction_enabled_expr || !friction_value_expr)
  {
    context.AddError("Vehicle", "Set Vehicle Attributes requires wheels, toggles, and values");
    return;
  }

  const std::optional<FlowEventsResult> flow_events =
      context.ResolveRequiredPrimaryFlow("Set Vehicle Attributes flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t axis_expr = AddConstantIntExpression(context.program, context.node.custom1);
  const int32_t wheels_fallback = IntExpressionConstantFallback(context.program, *wheels_expr);
  struct VehicleAttributeSpec {
    LN_OpCode opcode;
    const char *source_socket;
    uint32_t enabled_expr;
    uint32_t value_expr;
    float value_fallback;
  };

  const std::array<VehicleAttributeSpec, 4> specs = {{
      {LN_OpCode::SetVehicleSuspensionCompression,
       "Suspension Value",
       *suspension_enabled_expr,
       *suspension_value_expr,
       FloatExpressionConstantFallback(context.program, *suspension_value_expr)},
      {LN_OpCode::SetVehicleSuspensionStiffness,
       "Stiffness Value",
       *stiffness_enabled_expr,
       *stiffness_value_expr,
       FloatExpressionConstantFallback(context.program, *stiffness_value_expr)},
      {LN_OpCode::SetVehicleSuspensionDamping,
       "Damping Value",
       *damping_enabled_expr,
       *damping_value_expr,
       FloatExpressionConstantFallback(context.program, *damping_value_expr)},
      {LN_OpCode::SetVehicleWheelFriction,
       "Friction Value",
       *friction_enabled_expr,
       *friction_value_expr,
       FloatExpressionConstantFallback(context.program, *friction_value_expr)},
  }};

  for (const FlowPath &path : flow_events->paths) {
    for (const VehicleAttributeSpec &spec : specs) {
      LN_Instruction instruction;
      instruction.opcode = spec.opcode;
      instruction.source_ref_index = context.MakeSourceRef(spec.source_socket);
      if (vehicle_expr) {
        instruction.value_expr_index = *vehicle_expr;
      }
      instruction.int_expr_index = *wheels_expr;
      instruction.secondary_int_expr_index = axis_expr;
      instruction.float_expr_index = spec.value_expr;
      instruction.int_value = wheels_fallback;
      instruction.secondary_int_value = context.node.custom1;
      instruction.vector_value = MT_Vector3(spec.value_fallback, 0.0f, 0.0f);
      AppendFlowInstruction(context.program,
                            GuardFlowPath(context.program, path, spec.enabled_expr),
                            instruction);
    }
  }
}

void CompileEventSourceNode(CompilerContext &context, LN_Event event)
{
  context.has_event_node = true;
  const LN_PinDefinition *pulse_pin = FindFirstExecutionOutputPin(context.definition);
  const char *pulse_socket = pulse_pin ? pulse_pin->name.c_str() : "Out";
  const uint32_t source_ref_index = AddSourceRef(
      context.program, context.tree, context.node, pulse_socket);
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::Nop;
  instruction.source_ref_index = source_ref_index;
  context.program.AddInstruction(event, instruction);
}

void CompileConstantOutputNode(CompilerContext &context)
{
  if (context.definition.outputs.empty()) {
    return;
  }

  const blender::bNodeSocket *socket = FindOutputSocket(context.node,
                                                        context.definition.outputs[0].name);
  if (socket == nullptr) {
    return;
  }

  const std::optional<LN_Value> value = EvaluateOutputValue(context.node,
                                                            *socket,
                                                            context.node_definitions,
                                                            context.input_links,
                                                            context.value_cache);
  if (!value) {
    return;
  }

  LN_Constant constant;
  constant.source_ref_index = context.MakeSourceRef(context.definition.outputs[0].name.c_str());
  constant.value = *value;
  context.program.AddConstant(constant);
}

#define EVENT_HANDLER(kind, id, event)                                                       \
  {{LN_NodeKind::kind, {CompileHandlerKind::EventSource, false, false, true}, id},          \
   CompileActionKind::EmitEventSource,                                                       \
   FlowResolverKind::EventSource,                                                            \
   event,                                                                                    \
   nullptr}

#define CONST_HANDLER(kind, handler_kind, id, flow_kind)                                     \
  {{LN_NodeKind::kind, {CompileHandlerKind::handler_kind, false, false, true}, id},         \
   CompileActionKind::EmitConstantOutput,                                                    \
   flow_kind,                                                                                \
   LN_Event::OnInit,                                                                         \
   nullptr}

#define EXPRESSION_HANDLER(kind, handler_kind, id, flow_kind)                                \
  {{LN_NodeKind::kind, {CompileHandlerKind::handler_kind, false, false, true}, id},         \
   CompileActionKind::Custom,                                                                \
   flow_kind,                                                                                \
   LN_Event::OnInit,                                                                         \
   CompileExpressionOutputsNode}

#define COMMAND_HANDLER(kind, id, fn)                                                        \
  {{LN_NodeKind::kind, {CompileHandlerKind::Command, true, true, true}, id},                \
   CompileActionKind::Custom,                                                                \
   FlowResolverKind::Invalid,                                                                \
   LN_Event::OnInit,                                                                         \
   fn}

#define UNSUPPORTED_COMMAND_HANDLER(kind, id)                                                \
  {{LN_NodeKind::kind, {CompileHandlerKind::Command, true, true, false}, id},               \
   CompileActionKind::Custom,                                                                \
   FlowResolverKind::Invalid,                                                                \
   LN_Event::OnInit,                                                                         \
   CompileUnsupportedCommandNode}

#define DATA_CONTAINER_HANDLER(kind, id)                                                     \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id},           \
   CompileActionKind::Custom,                                                                \
   FlowResolverKind::Invalid,                                                                \
   LN_Event::OnInit,                                                                         \
   CompileDataContainerNode}

#define DATA_ACTION_HANDLER(kind, id)                                                        \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id},           \
   CompileActionKind::Custom,                                                                \
   FlowResolverKind::FlowConditionRoute,                                                     \
   LN_Event::OnInit,                                                                         \
   CompileDataContainerNode}

#define CUSTOM_EXPR_HANDLER(kind, id, flow_kind, fn)                                         \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id},           \
   CompileActionKind::Custom,                                                                \
   flow_kind,                                                                                \
   LN_Event::OnInit,                                                                         \
   fn}

#define SNAPSHOT_TRANSFORM_HANDLER(kind, id)                                                 \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id},           \
   CompileActionKind::Custom,                                                                \
   FlowResolverKind::Invalid,                                                                \
   LN_Event::OnInit,                                                                         \
   CompileSnapshotTransformNode}

static constexpr InternalCompileHandler internal_compile_handlers[] = {
    EVENT_HANDLER(EventOnInit, "event.init", LN_Event::OnInit),
    EVENT_HANDLER(EventOnFixedUpdate, "event.fixed_update", LN_Event::OnFixedUpdate),
    CONST_HANDLER(ValueBool, ValueExpression, "value.bool", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueInt, ValueExpression, "value.int", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueFloat, ValueExpression, "value.float", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueString, ValueExpression, "value.string", FlowResolverKind::Invalid),
    CONST_HANDLER(StringOperation,
                  ValueExpression,
                  "string.operation",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(FormattedString,
                  ValueExpression,
                  "string.format",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(ValueColor, ValueExpression, "value.color", FlowResolverKind::Invalid),
    CONST_HANDLER(ColorRGB, ValueExpression, "color.rgb", FlowResolverKind::Invalid),
    CONST_HANDLER(ColorRGBA, ValueExpression, "color.rgba", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueVector, ValueExpression, "value.vector", FlowResolverKind::Invalid),
    CONST_HANDLER(Euler, ValueExpression, "vector.euler", FlowResolverKind::Invalid),
    CONST_HANDLER(SeparateEuler,
                  ValueExpression,
                  "rotation.separate_euler",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(CombineXY, ValueExpression, "vector.xy", FlowResolverKind::Invalid),
    CONST_HANDLER(CombineXYZ, ValueExpression, "vector.xyz", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(CombineXYZW, "vector.xyzw", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(ResizeVector, "vector.resize", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(XYZToMatrix, "matrix.from_xyz", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(MatrixToXYZ, "matrix.to_xyz", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(VectorRotate, "vector.rotate", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(VectorToRotation, "vector.to_rotation", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(FilePath, "path.file", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetSound, "sound.get", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetImage, "image.get", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetFont, "font.get", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetObjectID, "object.id", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetAxisVector, "object.axis_vector", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(JoinPath, "path.join", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetMasterFolder, "path.master_folder", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CONST_HANDLER(SeparateXY, ValueExpression, "vector.separate_xy", FlowResolverKind::Invalid),
    CONST_HANDLER(SeparateXYZ,
                  ValueExpression,
                  "vector.separate_xyz",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(InvertValue, ValueExpression, "value.invert", FlowResolverKind::Invalid),
    CONST_HANDLER(ClampValue, ValueExpression, "value.clamp", FlowResolverKind::Invalid),
    CONST_HANDLER(MapRange, ValueExpression, "value.map", FlowResolverKind::Invalid),
    CONST_HANDLER(Threshold, ValueExpression, "value.threshold", FlowResolverKind::Invalid),
    CONST_HANDLER(RangedThreshold,
                  ValueExpression,
                  "value.ranged_threshold",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(WithinRange,
                  ValueExpression,
                  "value.within_range",
                  FlowResolverKind::Invalid),
    CONST_HANDLER(Math, ValueExpression, "math.float", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(OnNextFrame, Expression, "flow.next_frame", FlowResolverKind::FlowConditionRoute),
    CUSTOM_EXPR_HANDLER(Once, "flow.once", FlowResolverKind::PrecompiledRoute, CompileOnceNode),
    CUSTOM_EXPR_HANDLER(BooleanEdge,
                        "flow.boolean_edge",
                        FlowResolverKind::PollingEventRoute,
                        CompileBooleanEdgeNode),
    EXPRESSION_HANDLER(Branch, Expression, "flow.branch", FlowResolverKind::Branch),
    EXPRESSION_HANDLER(Gate, Expression, "flow.gate", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GateList, Expression, "flow.gate_list", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(ValueChanged,
                 Expression,
                 "flow.value_changed",
                 FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(ValueChangedTo,
                 Expression,
                 "flow.value_changed_to",
                 FlowResolverKind::PollingEventRoute),
    CUSTOM_EXPR_HANDLER(Delay, "flow.delay", FlowResolverKind::LatentCompletionRoute, CompileDelayNode),
    CUSTOM_EXPR_HANDLER(Timer, "flow.timer", FlowResolverKind::LatentCompletionRoute, CompileTimerNode),
    CUSTOM_EXPR_HANDLER(Pulsify, "flow.pulsify", FlowResolverKind::LatentCompletionRoute, CompilePulsifyNode),
    CUSTOM_EXPR_HANDLER(Barrier, "flow.barrier", FlowResolverKind::LatentCompletionRoute, CompileBarrierNode),
    CUSTOM_EXPR_HANDLER(Cooldown,
                        "flow.cooldown",
                        FlowResolverKind::PrecompiledRoute,
                        CompileCooldownNode),
    {{LN_NodeKind::Loop, {CompileHandlerKind::Expression, false, false, true}, "flow.loop"},
     CompileActionKind::Custom,
     FlowResolverKind::Loop,
     LN_Event::OnInit,
     CompileLoopNode},
    {{LN_NodeKind::LoopFromList, {CompileHandlerKind::Expression, false, false, true}, "flow.loop_from_list"},
     CompileActionKind::Custom,
     FlowResolverKind::Loop,
     LN_Event::OnInit,
     CompileLoopNode},
    DATA_CONTAINER_HANDLER(FindObject, "objects.find_object"),
    DATA_CONTAINER_HANDLER(ObjectByName, "objects.object_by_name"),
    DATA_CONTAINER_HANDLER(GetOwner, "objects.get_owner"),
    DATA_CONTAINER_HANDLER(GetGlobalProperty, "properties.global_get"),
    DATA_ACTION_HANDLER(ListGlobalProperties, "properties.global_list"),
    DATA_CONTAINER_HANDLER(LoadVariable, "properties.variable_load"),
    DATA_CONTAINER_HANDLER(LoadVariableDict, "properties.variable_dict_load"),
    CUSTOM_EXPR_HANDLER(ListSavedVariables,
              "properties.variable_list",
              FlowResolverKind::FlowConditionRoute,
              CompileListSavedVariablesNode),
    DATA_CONTAINER_HANDLER(GetScene, "scene.get_scene"),
    DATA_CONTAINER_HANDLER(GetCollection, "scene.get_collection"),
    DATA_CONTAINER_HANDLER(GetCollectionObjects, "scene.get_collection_objects"),
    DATA_CONTAINER_HANDLER(GetCollectionObjectNames, "scene.get_collection_object_names"),
    DATA_CONTAINER_HANDLER(ListLength, "data.list_length"),
    DATA_CONTAINER_HANDLER(ListGetItem, "data.list_get_item"),
    CUSTOM_EXPR_HANDLER(ListRandomItem,
              "data.list_random_item",
              FlowResolverKind::Invalid,
              CompileListRandomItemNode),
    DATA_CONTAINER_HANDLER(MakeList, "data.make_list"),
    DATA_CONTAINER_HANDLER(ListExtend, "data.list_extend"),
    DATA_CONTAINER_HANDLER(DictGetKey, "data.dict_get_key"),
    DATA_CONTAINER_HANDLER(MakeDict, "data.make_dict"),
    DATA_CONTAINER_HANDLER(DictLength, "data.dict_length"),
    DATA_CONTAINER_HANDLER(DictGetKeys, "data.dict_get_keys"),
    DATA_CONTAINER_HANDLER(EmptyList, "data.empty_list"),
    DATA_CONTAINER_HANDLER(EmptyDict, "data.empty_dict"),
    DATA_CONTAINER_HANDLER(DictHasKey, "data.dict_has_key"),
    DATA_CONTAINER_HANDLER(ListDuplicate, "data.list_duplicate"),
    DATA_CONTAINER_HANDLER(DictMerge, "data.dict_merge"),
    DATA_ACTION_HANDLER(ListAppend, "data.list_append"),
    DATA_ACTION_HANDLER(ListRemoveIndex, "data.list_remove_index"),
    DATA_ACTION_HANDLER(ListRemoveValue, "data.list_remove_value"),
    DATA_ACTION_HANDLER(ListSetIndex, "data.list_set_index"),
    DATA_ACTION_HANDLER(DictSetKey, "data.dict_set_key"),
    DATA_ACTION_HANDLER(DictRemoveKey, "data.dict_remove_key"),
    DATA_CONTAINER_HANDLER(ListFromItems, "data.list_from_items"),
    EXPRESSION_HANDLER(GetBoneTailWorld,
                 Expression,
                 "animation.bone_tail_world",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetBoneLength, Expression, "animation.bone_length", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetBoneCenterWorld,
                 Expression,
                 "animation.bone_center_world",
                 FlowResolverKind::Invalid),
    DATA_CONTAINER_HANDLER(ListContains, "data.list_contains"),
    EXPRESSION_HANDLER(LimitRange, Expression, "value.limit", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(RandomValue, Expression, "value.random", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(Typecast, "value.typecast", FlowResolverKind::Invalid, CompileTypecastNode),
    CUSTOM_EXPR_HANDLER(ValueValid,
              "value.valid",
              FlowResolverKind::Invalid,
              CompileValueValidNode),
    EXPRESSION_HANDLER(ValueSwitch, Expression, "value.switch", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(ValueSwitchList,
              "value.switch_list",
              FlowResolverKind::Invalid,
              CompileValueSwitchListNode),
    CUSTOM_EXPR_HANDLER(ValueSwitchListCompare,
              "value.switch_list_compare",
              FlowResolverKind::Invalid,
              CompileValueSwitchListCompareNode),
    EXPRESSION_HANDLER(StoreValue, Expression, "value.store", FlowResolverKind::FlowConditionRoute),
    EXPRESSION_HANDLER(HasProperty,
                 Expression,
                 "property.exists",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(IsNone, Expression, "value.is_none", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(NotNone, Expression, "value.not_none", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(KeyboardKey, Expression, "input.key", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(KeyboardActive,
                 Expression,
                 "input.keyboard_active",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(KeyCode, ValueExpression, "input.key_code", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(KeyLogger, Expression, "input.key_logger", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(MouseButton, Expression, "input.mouse", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(MouseMoved,
                 Expression,
                 "input.mouse_moved",
                 FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(MouseWheel,
                 Expression,
                 "input.mouse_wheel",
                 FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(MouseOver,
                 Expression,
                 "input.mouse_over",
                 FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(CursorPosition,
                 ValueExpression,
                 "input.cursor_position",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(CursorMovement,
                 ValueExpression,
                 "input.cursor_movement",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GamepadActive,
                 Expression,
                 "input.gamepad_active",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GamepadButton,
                 Expression,
                 "input.gamepad_button",
                 FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(GamepadStick,
                 ValueExpression,
                 "input.gamepad_stick",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetParent, ValueExpression, "object.parent", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetChild, ValueExpression, "object.child", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(GetChildByName,
              "object.child_by_name",
              FlowResolverKind::Invalid,
              CompileGetChildByNameNode),
    SNAPSHOT_TRANSFORM_HANDLER(GetGravity, "scene.gravity"),
    EXPRESSION_HANDLER(GetTimescale, Expression, "scene.timescale", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(Time, Expression, "time.data", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(DeltaFactor, Expression, "time.delta_factor", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetActiveCamera,
                 ValueExpression,
                 "scene.active_camera",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(WorldToScreen, Expression, "scene.world_to_screen", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(ScreenToWorld,
                 Expression,
                 "scene.screen_to_world",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetFullscreen,
                 Expression,
                 "render.fullscreen",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetResolution, Expression, "render.resolution", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetVSync, Expression, "render.vsync", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetCollisionGroup,
                 Expression,
                 "physics.collision_group",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetCharacterInfo,
           Expression,
           "physics.character_info",
           FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetGamePropertyInt,
                 Expression,
                 "property.get_int",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetGamePropertyFloat,
                 Expression,
                 "property.get_float",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetGamePropertyBool,
                 Expression,
                 "property.get_bool",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetGamePropertyString,
                 Expression,
                 "property.get_string",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetTreeProperty,
                 ValueExpression,
                 "property.get_tree",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetObjectAttribute,
                 ValueExpression,
                 "object.get_attribute",
                 FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(GetRigidBodyAttribute,
              "physics.get_rigid_body_attribute",
              FlowResolverKind::Invalid,
              CompileExpressionOutputsNode),
    EXPRESSION_HANDLER(GetDistance,
                 Expression,
                 "object.distance",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetGroupCenterPosition,
                 Expression,
                 "object.group_center_position",
                 FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(EvaluateProperty,
              "object.evaluate_property",
              FlowResolverKind::Invalid,
              CompileEvaluatePropertyNode),
    EXPRESSION_HANDLER(GetBoneHeadWorld,
                 Expression,
                 "animation.bone_head_world",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetBoneHeadPoseWorld,
                 Expression,
                 "animation.bone_head_pose_world",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetBoneTailPoseWorld,
                 Expression,
                 "animation.bone_tail_pose_world",
                 FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetBoneCenterPoseWorld,
                 Expression,
                 "animation.bone_center_pose_world",
                 FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(GetBoneAttribute,
              "animation.bone_attribute",
              FlowResolverKind::Invalid,
              CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetBonePoseRotation,
              "animation.bone_pose_rotation",
              FlowResolverKind::Invalid,
              CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetBonePoseScale,
              "animation.bone_pose_scale",
              FlowResolverKind::Invalid,
              CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetBonePoseTransform,
              "animation.bone_pose_transform",
              FlowResolverKind::Invalid,
              CompileExpressionOutputsNode),
    EXPRESSION_HANDLER(GetLightColor, Expression, "light.color", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetLightPower, Expression, "light.power", FlowResolverKind::Invalid),
    CONST_HANDLER(Compare, Expression, "value.compare", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(VectorMath, Expression, "math.vector", FlowResolverKind::Invalid),
    COMMAND_HANDLER(ToggleProperty, "property.toggle", CompileTogglePropertyNode),
    COMMAND_HANDLER(ModifyProperty, "property.modify", CompileModifyPropertyNode),
    COMMAND_HANDLER(ModifyPropertyClamped,
                    "property.modify_clamped",
                    CompileModifyPropertyClampedNode),
    COMMAND_HANDLER(SetTreeProperty, "property.set_tree", CompileSetTreePropertyNode),
    COMMAND_HANDLER(ToggleTreeProperty, "property.toggle_tree", CompileToggleTreePropertyNode),
    COMMAND_HANDLER(RemoveObject, "object.remove", CompileRemoveObjectNode),
    COMMAND_HANDLER(AddObject, "object.add", CompileAddObjectNode),
    COMMAND_HANDLER(SetParent, "object.set_parent", CompileSetParentNode),
    COMMAND_HANDLER(RemoveParent, "object.remove_parent", CompileRemoveParentNode),
    COMMAND_HANDLER(SetGravity, "scene.set_gravity", CompileSetGravityNode),
    COMMAND_HANDLER(SetTimescale, "scene.set_timescale", CompileSetTimescaleNode),
    COMMAND_HANDLER(SetCamera, "scene.set_camera", CompileSetCameraNode),
    COMMAND_HANDLER(SetCameraFov, "scene.set_camera_fov", CompileSetCameraFovNode),
    COMMAND_HANDLER(SetCameraOrthoScale,
                    "scene.set_camera_ortho_scale",
                    CompileSetCameraOrthoScaleNode),
    COMMAND_HANDLER(SetFullscreen, "render.set_fullscreen", CompileSetFullscreenNode),
    COMMAND_HANDLER(SetResolution, "render.set_resolution", CompileSetResolutionNode),
    COMMAND_HANDLER(SetVSync, "render.set_vsync", CompileSetVSyncNode),
    COMMAND_HANDLER(ShowFramerate, "render.show_framerate", CompileShowFramerateNode),
    COMMAND_HANDLER(ShowProfile, "render.show_profile", CompileShowProfileNode),
    COMMAND_HANDLER(SetCollisionGroup,
                    "physics.set_collision_group",
                    CompileSetCollisionGroupNode),
    COMMAND_HANDLER(SetPhysics, "physics.set", CompileSetPhysicsNode),
    COMMAND_HANDLER(SetDynamics, "physics.set_dynamics", CompileSetDynamicsNode),
    COMMAND_HANDLER(RebuildCollisionShape,
                    "physics.rebuild_collision_shape",
                    CompileRebuildCollisionShapeNode),
    COMMAND_HANDLER(SetRigidBodyAttribute,
                    "physics.set_rigid_body_attribute",
                    CompileSetRigidBodyAttributeNode),
    COMMAND_HANDLER(CharacterJump, "physics.character_jump", CompileCharacterJumpNode),
    COMMAND_HANDLER(SetCharacterGravity,
            "physics.character_set_gravity",
            CompileSetCharacterGravityNode),
    COMMAND_HANDLER(SetCharacterJumpSpeed,
            "physics.character_set_jump_speed",
            CompileSetCharacterJumpSpeedNode),
    COMMAND_HANDLER(SetCharacterMaxJumps,
            "physics.character_set_max_jumps",
            CompileSetCharacterMaxJumpsNode),
    COMMAND_HANDLER(SetCharacterWalkDirection,
            "physics.character_set_walk_direction",
            CompileSetCharacterWalkDirectionNode),
    COMMAND_HANDLER(SetCharacterVelocity,
            "physics.character_set_velocity",
            CompileSetCharacterVelocityNode),
        COMMAND_HANDLER(VehicleControl,
          "physics.vehicle_control",
          CompileVehicleControlNode),
    COMMAND_HANDLER(VehicleAccelerate,
            "physics.vehicle_accelerate",
            CompileVehicleAccelerateNode),
    COMMAND_HANDLER(VehicleBrake, "physics.vehicle_brake", CompileVehicleBrakeNode),
    COMMAND_HANDLER(VehicleSteer, "physics.vehicle_steer", CompileVehicleSteerNode),
    COMMAND_HANDLER(VehicleSetAttributes,
            "physics.vehicle_set_attributes",
            CompileVehicleSetAttributesNode),
    COMMAND_HANDLER(SetCursorVisibility,
                    "input.set_cursor_visibility",
                    CompileSetCursorVisibilityNode),
    COMMAND_HANDLER(SetCursorPosition,
                    "input.set_cursor_position",
                    CompileSetCursorPositionNode),
    COMMAND_HANDLER(GamepadVibration,
                    "input.gamepad_vibration",
                    CompileGamepadVibrationNode),
    COMMAND_HANDLER(GamepadLook, "input.gamepad_look", CompileGamepadLookNode),
    COMMAND_HANDLER(MouseLook, "input.mouse_look", CompileMouseLookNode),
    COMMAND_HANDLER(ApplyImpulse, "physics.apply_impulse", CompileApplyImpulseNode),
    COMMAND_HANDLER(MakeLightUnique, "light.make_unique", CompileMakeLightUniqueNode),
    COMMAND_HANDLER(SetLightColor, "light.set_color", CompileSetLightColorNode),
    COMMAND_HANDLER(SetLightPower, "light.set_power", CompileSetLightPowerNode),
    COMMAND_HANDLER(SetLightShadow, "light.set_shadow", CompileSetLightShadowNode),
    COMMAND_HANDLER(ApplyMovement, "transform.apply_movement", CompileApplyMovementNode),
    COMMAND_HANDLER(ApplyRotation, "transform.apply_rotation", CompileApplyRotationNode),
    COMMAND_HANDLER(ApplyForce, "physics.apply_force", CompileApplyForceNode),
    COMMAND_HANDLER(ApplyForceToTarget,
                    "physics.apply_force_to_target",
                    CompileApplyForceToTargetNode),
    COMMAND_HANDLER(ApplyTorque, "physics.apply_torque", CompileApplyTorqueNode),
    COMMAND_HANDLER(SetGamePropertyInt,
                    "property.set_int",
                    CompileSetGamePropertyIntNode),
    COMMAND_HANDLER(SetGamePropertyFloat,
                    "property.set_float",
                    CompileSetGamePropertyFloatNode),
    COMMAND_HANDLER(SetGamePropertyBool,
                    "property.set_bool",
                    CompileSetGamePropertyBoolNode),
    COMMAND_HANDLER(SetGamePropertyString,
                    "property.set_string",
                    CompileSetGamePropertyStringNode),
    COMMAND_HANDLER(Print, "debug.print", CompilePrintNode),
    COMMAND_HANDLER(QuitGame, "game.quit", CompileQuitGameNode),
    COMMAND_HANDLER(RestartGame, "game.restart", CompileRestartGameNode),
    COMMAND_HANDLER(LoadBlendFile, "game.load_blend_file", CompileLoadBlendFileNode),
    COMMAND_HANDLER(PlayAction, "animation.play_action", CompilePlayActionNode),
    COMMAND_HANDLER(StopAction, "animation.stop_action", CompileStopActionNode),
    COMMAND_HANDLER(SetActionFrame, "animation.set_action_frame", CompileSetActionFrameNode),
    COMMAND_HANDLER(StopAllSounds, "audio.stop_all_sounds", CompileStopAllSoundsNode),
    COMMAND_HANDLER(PlaySound, "audio.play_sound", CompilePlaySoundNode),
    COMMAND_HANDLER(PlaySound3D, "audio.play_sound_3d", CompilePlaySound3DNode),
    COMMAND_HANDLER(PauseSound, "audio.pause_sound", CompilePauseSoundNode),
    COMMAND_HANDLER(ResumeSound, "audio.resume_sound", CompileResumeSoundNode),
    COMMAND_HANDLER(StopSound, "audio.stop_sound", CompileStopSoundNode),
    COMMAND_HANDLER(StartLogicTree, "logic.tree.start", CompileStartLogicTreeNode),
    COMMAND_HANDLER(StopLogicTree, "logic.tree.stop", CompileStopLogicTreeNode),
    COMMAND_HANDLER(RunLogicTree, "logic.tree.run_once", CompileRunLogicTreeNode),
    COMMAND_HANDLER(InstallLogicTree, "logic.tree.install", CompileInstallLogicTreeNode),
    COMMAND_HANDLER(SendEvent, "events.send_event", CompileSendEventNode),
    COMMAND_HANDLER(SetGlobalProperty, "properties.global_set", CompileSetGlobalPropertyNode),
    COMMAND_HANDLER(SaveVariable, "properties.variable_save", CompileSaveVariableNode),
    COMMAND_HANDLER(SaveVariableDict, "properties.variable_dict_save", CompileSaveVariableDictNode),
    COMMAND_HANDLER(ClearVariables, "properties.variable_clear", CompileClearVariablesNode),
    COMMAND_HANDLER(RemoveVariable, "properties.variable_remove", CompileRemoveVariableNode),
    COMMAND_HANDLER(Translate, "transform.translate", CompileTranslateNode),
    COMMAND_HANDLER(Navigate, "transform.navigate", CompileNavigateNode),
    COMMAND_HANDLER(FollowPath, "transform.follow_path", CompileFollowPathNode),
    COMMAND_HANDLER(SetCollectionVisibility,
            "scene.set_collection_visibility",
            CompileSetCollectionVisibilityNode),
        COMMAND_HANDLER(SetOverlayCollection,
          "scene.set_overlay_collection",
          CompileSetOverlayCollectionNode),
        COMMAND_HANDLER(RemoveOverlayCollection,
          "scene.remove_overlay_collection",
          CompileRemoveOverlayCollectionNode),
    COMMAND_HANDLER(MoveToward, "transform.move_to", CompileMoveTowardNode),
    COMMAND_HANDLER(SlowFollow, "transform.slow_follow", CompileSlowFollowNode),
    COMMAND_HANDLER(LoadScene, "scene.load_scene", CompileLoadSceneNode),
    COMMAND_HANDLER(SetScene, "scene.set_scene", CompileSetSceneNode),
    COMMAND_HANDLER(SaveGame, "game.save_game", CompileSaveGameNode),
    COMMAND_HANDLER(LoadGame, "game.load_game", CompileLoadGameNode),
    COMMAND_HANDLER(AlignAxisToVector, "transform.align_axis", CompileAlignAxisToVectorNode),
    COMMAND_HANDLER(RotateToward, "transform.rotate_to", CompileRotateTowardNode),
    COMMAND_HANDLER(SetObjectAttribute, "object.set_attribute", CompileSetObjectAttributeNode),
    COMMAND_HANDLER(ReplaceMesh, "object.replace_mesh", CompileReplaceMeshNode),
    COMMAND_HANDLER(CopyProperty, "property.copy_property", CompileCopyPropertyNode),
    COMMAND_HANDLER(SetBonePoseLocation,
                    "animation.set_bone_pose_location",
                    CompileSetBonePoseLocationNode),
    COMMAND_HANDLER(SetBonePoseRotation,
                    "animation.set_bone_pose_rotation",
                    CompileSetBonePoseRotationNode),
    COMMAND_HANDLER(SetBonePoseScale,
                    "animation.set_bone_pose_scale",
                    CompileSetBonePoseScaleNode),
    COMMAND_HANDLER(SetBonePoseTransform,
                    "animation.set_bone_pose_transform",
                    CompileSetBonePoseTransformNode),
    COMMAND_HANDLER(SetBoneAttribute,
            "animation.set_bone_attribute",
          CompileSetBoneAttributeNode),
    COMMAND_HANDLER(SetBoneConstraintInfluence,
                    "animation.set_bone_constraint_influence",
                    CompileSetBoneConstraintInfluenceNode),
    COMMAND_HANDLER(SetBoneConstraintTarget,
            "animation.set_bone_constraint_target",
          CompileSetBoneConstraintTargetNode),
    COMMAND_HANDLER(SetBoneConstraintAttribute,
            "animation.set_bone_constraint_attribute",
          CompileSetBoneConstraintAttributeNode),
    COMMAND_HANDLER(SetMaterialSlot, "material.set_slot", CompileSetMaterialSlotNode),
    COMMAND_HANDLER(SetMaterialParameter,
                    "material.set_parameter",
                    CompileSetMaterialParameterNode),
        CUSTOM_EXPR_HANDLER(GetMaterialFromSlot,
          "material.get_from_slot",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        CUSTOM_EXPR_HANDLER(GetMaterialSlotCount,
          "material.slot_count",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        CUSTOM_EXPR_HANDLER(GetMaterialName,
          "material.name",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        CUSTOM_EXPR_HANDLER(GetMaterialParameter,
          "material.get_parameter",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        COMMAND_HANDLER(SetGeometryNodesInput,
            "geometry.set_modifier_input",
          CompileSetGeometryNodesInputNode),
        CUSTOM_EXPR_HANDLER(GetEditorNodeValue,
          "nodes.get_editor_node_value",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        COMMAND_HANDLER(SetEditorNodeValue,
            "nodes.set_editor_node_value",
          CompileSetEditorNodeValueNode),
        COMMAND_HANDLER(MakeNodeTreeUnique,
            "nodes.make_tree_unique",
          CompileMakeNodeTreeUniqueNode),
        COMMAND_HANDLER(SetNodeMute,
            "nodes.set_mute",
          CompileSetNodeMuteNode),
        COMMAND_HANDLER(EnableDisableModifier,
            "geometry.enable_disable_modifier",
          CompileEnableDisableModifierNode),
        COMMAND_HANDLER(AssignGeometryNodesModifier,
            "geometry.assign_nodes_modifier",
          CompileAssignGeometryNodesModifierNode),
        CUSTOM_EXPR_HANDLER(GetNodeGroupSocketValue,
          "node_group.get_socket",
          FlowResolverKind::Invalid,
          CompileExpressionOutputsNode),
        UNSUPPORTED_COMMAND_HANDLER(SetNodeGroupSocketValue,
            "node_group.set_socket"),
        UNSUPPORTED_COMMAND_HANDLER(PlayMaterialSequence,
            "material.play_sequence"),
        COMMAND_HANDLER(StartSpeaker, "audio.start_speaker", CompileStartSpeakerNode),
        COMMAND_HANDLER(DrawLine, "debug.draw_line", CompileDrawLineNode),
        COMMAND_HANDLER(DrawCube, "debug.draw_cube", CompileDrawCubeNode),
        COMMAND_HANDLER(DrawBox, "debug.draw_box", CompileDrawBoxNode),
        COMMAND_HANDLER(Draw, "debug.draw", CompileDrawNode),
        UNSUPPORTED_COMMAND_HANDLER(LoadFileContent, "file.load_content"),
        UNSUPPORTED_COMMAND_HANDLER(SetCustomCursor, "ui.set_custom_cursor"),
    EXPRESSION_HANDLER(ReceiveEvent,
                       Expression,
                       "events.receive_event",
                       FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(LogicTreeStatus, Expression, "logic.tree_status", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(ActionDone, Expression, "animation.action_done", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(ObjectsColliding, Expression, "physics.objects_colliding", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(Collision, Expression, "physics.is_colliding", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(OnCollision, Expression, "physics.on_collision", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(AnimationStatus, Expression, "animation.animation_status", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(Raycast,
                        "physics.raycast",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(RaycastAll,
                        "physics.raycast_all",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(ShapeCast,
                        "physics.shape_cast",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(ShapeCastAll,
                        "physics.shape_cast_all",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    EXPRESSION_HANDLER(MouseRay, Expression, "physics.mouse_ray", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(CameraRay, Expression, "physics.camera_ray", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(EvaluateCurve, Expression, "curve.evaluate", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(RandomFloat,
              "math.random_float",
              FlowResolverKind::Invalid,
              CompileRandomFloatNode),
    CUSTOM_EXPR_HANDLER(RandomInt,
              "math.random_int",
              FlowResolverKind::Invalid,
              CompileRandomIntNode),
    CUSTOM_EXPR_HANDLER(RandomVector,
              "math.random_vector",
              FlowResolverKind::Invalid,
              CompileRandomVectorNode),
    CUSTOM_EXPR_HANDLER(Formula, "math.formula", FlowResolverKind::Invalid, CompileFormulaNode),
    CUSTOM_EXPR_HANDLER(TweenValue,
              "time.tween_value",
              FlowResolverKind::PollingEventRoute,
              CompileTweenValueNode),
    COMMAND_HANDLER(AddPhysicsConstraint,
                    "physics.add_constraint",
                    CompileAddPhysicsConstraintNode),
    CUSTOM_EXPR_HANDLER(GetRigidBodyConstraints,
                        "physics.get_constraints",
                        FlowResolverKind::Invalid,
                        CompileGetRigidBodyConstraintsNode),
    COMMAND_HANDLER(RemovePhysicsConstraint,
                    "physics.remove_constraint",
                    CompileRemovePhysicsConstraintNode),
    CUSTOM_EXPR_HANDLER(ProjectileRay,
              "physics.projectile_ray",
              FlowResolverKind::Invalid,
              CompileProjectileRayNode),
    COMMAND_HANDLER(SpawnPool, "objects.spawn_pool", CompileSpawnPoolNode),
};

#undef EVENT_HANDLER
#undef CONST_HANDLER
#undef EXPRESSION_HANDLER
#undef COMMAND_HANDLER
#undef UNSUPPORTED_COMMAND_HANDLER
#undef DATA_CONTAINER_HANDLER
#undef DATA_ACTION_HANDLER
#undef CUSTOM_EXPR_HANDLER
#undef SNAPSHOT_TRANSFORM_HANDLER

const InternalCompileHandler *FindInternalCompileHandler(const LN_NodeKind kind)
{
  static const auto &lookup = []() {
    std::array<const InternalCompileHandler *, 512> table{};
    for (const InternalCompileHandler &handler : internal_compile_handlers) {
      const size_t index = static_cast<size_t>(handler.descriptor.node_kind);
      if (index < table.size()) {
        table[index] = &handler;
      }
    }
    return table;
  }();
  const size_t index = static_cast<size_t>(kind);
  if (index >= lookup.size()) {
    return nullptr;
  }
  return lookup[index];
}

static const char *compile_handler_kind_name(const CompileHandlerKind kind)
{
  switch (kind) {
    case CompileHandlerKind::Unsupported:
      return "Unsupported";
    case CompileHandlerKind::EventSource:
      return "EventSource";
    case CompileHandlerKind::ValueExpression:
      return "ValueExpression";
    case CompileHandlerKind::Expression:
      return "Expression";
    case CompileHandlerKind::Command:
      return "Command";
  }
  return "Unknown";
}

static std::string compile_handler_label(const CompileHandlerDescriptor &descriptor)
{
  std::ostringstream stream;
  stream << (descriptor.handler_id != nullptr ? descriptor.handler_id : "<null handler id>");
  stream << " kind=" << int(descriptor.node_kind);
  return stream.str();
}

static void add_compile_inventory_error(std::vector<std::string> &errors,
                                        const CompileHandlerDescriptor &descriptor,
                                        const std::string &message)
{
  errors.push_back(compile_handler_label(descriptor) + ": " + message);
}

static const CompileHandlerDescriptor *find_descriptor_in_span(
    const CompileHandlerDescriptor *descriptors,
    const size_t count,
    const LN_NodeKind kind)
{
  for (size_t index = 0; index < count; index++) {
    if (descriptors[index].node_kind == kind) {
      return &descriptors[index];
    }
  }
  return nullptr;
}

static bool validate_compile_handler_descriptors(const CompileHandlerDescriptor *descriptors,
                                                 const size_t count,
                                                 std::vector<std::string> &errors)
{
  if (descriptors == nullptr) {
    errors.push_back("compiler handler descriptor table is null");
    return false;
  }
  if (count == 0) {
    errors.push_back("compiler handler descriptor table is empty");
    return false;
  }

  std::array<bool, 512> seen_kinds{};
  for (size_t index = 0; index < count; index++) {
    const CompileHandlerDescriptor &descriptor = descriptors[index];
    const size_t kind_index = static_cast<size_t>(descriptor.node_kind);
    if (kind_index >= seen_kinds.size()) {
      add_compile_inventory_error(errors, descriptor, "node kind is outside validation table");
    }
    else if (seen_kinds[kind_index]) {
      add_compile_inventory_error(errors, descriptor, "duplicate compiler handler node kind");
    }
    else {
      seen_kinds[kind_index] = true;
    }

    if (descriptor.handler_id == nullptr || descriptor.handler_id[0] == '\0') {
      add_compile_inventory_error(errors, descriptor, "handler id is empty");
    }
    if (descriptor.info.kind == CompileHandlerKind::Unsupported) {
      add_compile_inventory_error(errors, descriptor, "handler kind is unsupported");
    }
    if (descriptor.info.kind == CompileHandlerKind::Command) {
      if (!descriptor.info.emits_commands) {
        add_compile_inventory_error(errors, descriptor, "command handler does not emit commands");
      }
      if (!descriptor.info.requires_flow_input) {
        add_compile_inventory_error(errors, descriptor, "command handler does not require flow input");
      }
    }
    else if (descriptor.info.emits_commands) {
      add_compile_inventory_error(errors,
                                  descriptor,
                                  std::string("non-command handler emits commands: ") +
                                      compile_handler_kind_name(descriptor.info.kind));
    }
    if (!descriptor.info.has_runtime_implementation &&
        descriptor.info.kind != CompileHandlerKind::Command)
    {
      add_compile_inventory_error(errors,
                                  descriptor,
                                  "only command handlers may be marked unsupported at runtime");
    }
  }

  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    const CompileHandlerDescriptor *descriptor = find_descriptor_in_span(descriptors,
                                                                         count,
                                                                         definition.kind);
    if (descriptor == nullptr) {
      errors.push_back(definition.idname + ": registry node has no compiler handler descriptor");
      continue;
    }
    if (descriptor->info.emits_commands != definition.has_side_effects) {
      add_compile_inventory_error(
          errors, *descriptor, definition.idname + ": command emission does not match side effects");
    }
    if (definition.has_side_effects && descriptor->info.kind != CompileHandlerKind::Command) {
      add_compile_inventory_error(
          errors, *descriptor, definition.idname + ": side-effect node is not a command handler");
    }
  }

  return errors.empty();
}

static bool validate_builtin_compile_handler_inventory(std::vector<std::string> &errors)
{
  std::array<CompileHandlerDescriptor, std::size(internal_compile_handlers)> descriptors{};
  for (size_t index = 0; index < descriptors.size(); index++) {
    descriptors[index] = internal_compile_handlers[index].descriptor;
  }
  const bool descriptors_valid = validate_compile_handler_descriptors(descriptors.data(),
                                                                      descriptors.size(),
                                                                      errors);

  for (const InternalCompileHandler &handler : internal_compile_handlers) {
    const CompileHandlerDescriptor &descriptor = handler.descriptor;
    const LN_TreeCompiler::CompileHandlerInfo &info = descriptor.info;
    switch (handler.action_kind) {
      case CompileActionKind::EmitEventSource:
        if (info.kind != CompileHandlerKind::EventSource) {
          add_compile_inventory_error(errors, descriptor, "event action lacks event handler kind");
        }
        if (handler.flow_kind != FlowResolverKind::EventSource) {
          add_compile_inventory_error(errors, descriptor, "event action lacks event flow resolver");
        }
        if (handler.compile_fn != nullptr) {
          add_compile_inventory_error(errors, descriptor, "event action must not have compile function");
        }
        break;
      case CompileActionKind::EmitConstantOutput:
        if (info.kind != CompileHandlerKind::ValueExpression &&
            info.kind != CompileHandlerKind::Expression)
        {
          add_compile_inventory_error(errors,
                                      descriptor,
                                      "constant-output action lacks expression handler kind");
        }
        if (handler.compile_fn != nullptr) {
          add_compile_inventory_error(errors,
                                      descriptor,
                                      "constant-output action must not have compile function");
        }
        break;
      case CompileActionKind::Custom:
        if (handler.compile_fn == nullptr) {
          add_compile_inventory_error(errors, descriptor, "custom action has no compile function");
        }
        break;
      case CompileActionKind::None:
        add_compile_inventory_error(errors, descriptor, "handler has no compile action");
        break;
    }

    if (info.kind == CompileHandlerKind::Command && handler.action_kind != CompileActionKind::Custom)
    {
      add_compile_inventory_error(errors, descriptor, "command handler is not custom compiled");
    }
    if (info.kind == CompileHandlerKind::Command && handler.flow_kind != FlowResolverKind::Invalid)
    {
      add_compile_inventory_error(errors,
                                  descriptor,
                                  "command handler must use explicit command flow resolution");
    }
    if (info.kind != CompileHandlerKind::Command && info.requires_flow_input) {
      add_compile_inventory_error(errors,
                                  descriptor,
                                  "non-command handler requires command flow input");
    }
  }

  return descriptors_valid && errors.empty();
}

}  // namespace ln_compiler

LN_TreeCompiler::CompileHandlerInfo LN_TreeCompiler::GetCompileHandlerInfo(
    const LN_NodeKind kind)
{
  if (const ln_compiler::InternalCompileHandler *handler =
          ln_compiler::FindInternalCompileHandler(kind))
  {
    return handler->descriptor.info;
  }

  return {CompileHandlerKind::Unsupported, false, false, false};
}

LN_TreeCompiler::CompileDispatch LN_TreeCompiler::GetCompileDispatch(const LN_NodeKind kind)
{
  const ln_compiler::InternalCompileHandler *handler =
      ln_compiler::FindInternalCompileHandler(kind);
  if (handler == nullptr) {
    return CompileDispatch::Unsupported;
  }
  switch (handler->action_kind) {
    case ln_compiler::CompileActionKind::EmitEventSource:
      return CompileDispatch::EventSource;
    case ln_compiler::CompileActionKind::EmitConstantOutput:
      return CompileDispatch::ConstantOutput;
    case ln_compiler::CompileActionKind::Custom:
      return CompileDispatch::CustomCompile;
    case ln_compiler::CompileActionKind::None:
      return CompileDispatch::Unsupported;
  }
  return CompileDispatch::Unsupported;
}

const LN_TreeCompiler::CompileHandlerDescriptor *LN_TreeCompiler::FindCompileHandlerDescriptor(
    const LN_NodeKind kind)
{
  if (const ln_compiler::InternalCompileHandler *handler =
          ln_compiler::FindInternalCompileHandler(kind))
  {
    return &handler->descriptor;
  }
  return nullptr;
}

const LN_TreeCompiler::CompileHandlerDescriptor *LN_TreeCompiler::GetCompileHandlerDescriptors(
    size_t &r_count)
{
  static const auto descriptors = [] {
    std::array<CompileHandlerDescriptor,
               std::size(ln_compiler::internal_compile_handlers)> values{};
    for (size_t index = 0; index < values.size(); index++) {
      values[index] = ln_compiler::internal_compile_handlers[index].descriptor;
    }
    return values;
  }();

  r_count = descriptors.size();
  return descriptors.data();
}

bool LN_TreeCompiler::ValidateCompileHandlerInventory(std::vector<std::string> *r_errors)
{
  std::vector<std::string> errors;
  const bool valid = ln_compiler::validate_builtin_compile_handler_inventory(errors);
  if (r_errors != nullptr) {
    *r_errors = std::move(errors);
  }
  return valid;
}

bool LN_TreeCompiler::ValidateCompileHandlerDescriptors(
    const CompileHandlerDescriptor *descriptors,
    const size_t count,
    std::vector<std::string> *r_errors)
{
  std::vector<std::string> errors;
  const bool valid = ln_compiler::validate_compile_handler_descriptors(descriptors,
                                                                       count,
                                                                       errors);
  if (r_errors != nullptr) {
    *r_errors = std::move(errors);
  }
  return valid;
}

std::string LN_TreeCompiler::BuildSourceChecksum(const blender::bNodeTree &tree)
{
  return ln_compiler::BuildChecksum(tree);
}

std::shared_ptr<LN_Program> LN_TreeCompiler::Compile(const blender::bNodeTree &tree) const
{
  using namespace ln_compiler;

  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  program->m_sourceTreeName = IDNameWithoutPrefix(tree.id);
  program->m_sourceTreeLibraryPath = tree.id.lib ? SafeString(tree.id.lib->filepath) : "";
  program->m_name = program->m_sourceTreeName.empty() ? "Logic Nodes Program" :
                                                    program->m_sourceTreeName + " Program";
  program->m_sourceChecksum = BuildChecksum(tree);
  program->m_compileReport.SetSourceTreeName(program->m_sourceTreeName);

  if (tree.type != blender::NTREE_LOGIC || std::strcmp(tree.idname, "LogicNodeTree") != 0) {
    program->m_compileReport.SetDisabledReason("Only LogicNodeTree data-blocks can compile");
    program->m_compileReport.AddIssue(LN_CompileSeverity::Error,
                                      "Node tree is not a LogicNodeTree");
    return program;
  }

  std::vector<const blender::bNode *> nodes;
  NodeDefinitionMap node_definitions;
  for (const blender::bNode *node = static_cast<const blender::bNode *>(tree.nodes.first);
       node;
       node = node->next)
  {
    if (IsFrameNode(*node) || IsRerouteNode(*node)) {
      continue;
    }

    nodes.push_back(node);
    const LN_NodeDefinition *definition = m_registry.FindNodeDefinition(node->idname);
    if (definition == nullptr) {
      AddNodeIssue(*program,
                   tree,
                   *node,
                   nullptr,
                   LN_CompileSeverity::Error,
                   "Unknown Logic Nodes node type '" + SafeString(node->idname) + "'");
      continue;
    }
    const CompileHandlerInfo handler_info = GetCompileHandlerInfo(definition->kind);
    if (handler_info.kind == CompileHandlerKind::Unsupported) {
      AddNodeIssue(*program,
                   tree,
                   *node,
                   nullptr,
                   LN_CompileSeverity::Error,
                   "Logic Nodes node type has no native compiler handler '" +
                       SafeString(node->idname) + "'");
      continue;
    }
    if (!handler_info.has_runtime_implementation) {
      AddNodeIssue(*program,
                   tree,
                   *node,
                   nullptr,
                   LN_CompileSeverity::Error,
                   "Logic Nodes node type does not have a runtime command implementation "
                   "and is unavailable in this release '" +
                       SafeString(node->idname) + "'");
      continue;
    }
    node_definitions.emplace(node, definition);
  }

  RawInputLinkMap raw_input_links;
  RawOutputLinkMap raw_output_links;
  for (const blender::bNodeLink *link = static_cast<const blender::bNodeLink *>(tree.links.first);
       link;
       link = link->next)
  {
    if (!IsUsedLink(*link)) {
      continue;
    }

    raw_input_links[link->tosock].push_back(link);
    raw_output_links[link->fromsock].push_back(link);
  }

  InputLinkMap input_links;
  EdgeMap edges;
  std::vector<ResolvedLink> resolved_links;
  for (const blender::bNodeLink *link = static_cast<const blender::bNodeLink *>(tree.links.first);
       link;
       link = link->next)
  {
    if (!IsUsedLink(*link) || IsRerouteNode(*link->fromnode)) {
      continue;
    }

    std::unordered_set<const blender::bNode *> visited_sources;
    const std::optional<ResolvedEndpoint> source = ResolveSourceEndpoint(
        *link->fromnode, *link->fromsock, raw_input_links, visited_sources);
    if (!source) {
      continue;
    }

    std::vector<ResolvedEndpoint> targets;
    std::unordered_set<const blender::bNode *> visited_targets;
    AppendTargetEndpoints(*link->tonode,
                          *link->tosock,
                          raw_output_links,
                          targets,
                          visited_targets);
    for (const ResolvedEndpoint &target : targets) {
      if (target.node == nullptr || target.socket == nullptr) {
        continue;
      }

      ResolvedLink resolved_link;
      resolved_link.fromnode = source->node;
      resolved_link.fromsock = source->socket;
      resolved_link.tonode = target.node;
      resolved_link.tosock = target.socket;
      resolved_links.push_back(resolved_link);
      input_links[target.socket].push_back(resolved_link);
      edges[source->node].push_back(target.node);
    }
  }

  const ActiveNodeSet active_nodes = ComputeActiveNodes(
      nodes, node_definitions, input_links, resolved_links);

  std::unordered_set<const blender::bNode *> validation_relevant_nodes = active_nodes.active;
  for (const ResolvedLink &link : resolved_links) {
    if (IsNodeActive(active_nodes, link.fromnode) || IsNodeActive(active_nodes, link.tonode)) {
      validation_relevant_nodes.insert(link.fromnode);
      validation_relevant_nodes.insert(link.tonode);
    }
  }

  for (const ResolvedLink &link : resolved_links) {
    const bool active_link = IsNodeActive(active_nodes, link.fromnode) &&
                             IsNodeActive(active_nodes, link.tonode);
    const auto from_definition_iter = node_definitions.find(link.fromnode);
    const auto to_definition_iter = node_definitions.find(link.tonode);
    if (from_definition_iter == node_definitions.end() ||
        to_definition_iter == node_definitions.end())
    {
      continue;
    }

    const LN_PinDefinition *from_pin = FindPinDefinition(from_definition_iter->second->outputs,
                                                         *link.fromsock);
    const LN_PinDefinition *to_pin = FindPinDefinition(to_definition_iter->second->inputs,
                                                       *link.tosock);
    if (from_pin == nullptr || to_pin == nullptr) {
      AddNodeIssue(*program,
                   tree,
                   *link.tonode,
                   link.tosock->name,
                   active_link ? LN_CompileSeverity::Error : LN_CompileSeverity::Warning,
                   "Link uses a socket that is not part of the native node definition");
      continue;
    }
    if (!LogicPinsCompatible(*from_pin, *to_pin)) {
      AddNodeIssue(*program,
                   tree,
                   *link.tonode,
                   link.tosock->name,
                   active_link ? LN_CompileSeverity::Error : LN_CompileSeverity::Warning,
                   LinkTypeMismatchDiagnostic(*from_pin, *to_pin));
    }
  }

  WarnInactiveNodes(*program, tree, nodes, node_definitions, active_nodes);

  for (const blender::bNode *node : nodes) {
    if (validation_relevant_nodes.find(node) == validation_relevant_nodes.end()) {
      continue;
    }

    const auto definition_iter = node_definitions.find(node);
    if (definition_iter == node_definitions.end()) {
      continue;
    }
    const bool node_is_active = IsNodeActive(active_nodes, node);

    if (node_is_active && definition_iter->second->kind == LN_NodeKind::Math) {
      if (!IsSupportedLogicMathOperation(node->custom1)) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     "Result",
                     LN_CompileSeverity::Error,
                     "Unsupported Math operation for Logic Nodes V1");
      }
    }

    if (node_is_active && definition_iter->second->kind == LN_NodeKind::VectorMath) {
      if (!IsSupportedLogicVectorMathOperation(node->custom1)) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     "Result",
                     LN_CompileSeverity::Error,
                     "Unsupported Vector Math operation for Logic Nodes V1");
      }
    }

    if (node_is_active && definition_iter->second->kind == LN_NodeKind::StringOperation) {
      if (!IsSupportedLogicStringOperation(node->custom1)) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     "String",
                     LN_CompileSeverity::Error,
                     "Unsupported String Operation mode for Logic Nodes V1");
      }
    }

    for (const LN_PinDefinition &input : definition_iter->second->inputs) {
      const blender::bNodeSocket *socket = FindInputSocket(*node, input.name);
      if (socket == nullptr) {
        const bool optional_missing_socket =
            input.value_type == LN_ValueType::ObjectRef ||
            input.value_type == LN_ValueType::DatablockRef ||
            input.value_type == LN_ValueType::SceneRef ||
            input.value_type == LN_ValueType::CollectionRef;
        if (node_is_active && (input.requires_link || !optional_missing_socket)) {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       input.name.c_str(),
                       LN_CompileSeverity::Error,
                       RequiredInputSocketMissingDiagnostic(input));
        }
        continue;
      }
      const LN_PinDefinition *matched_input = FindPinDefinition(definition_iter->second->inputs,
                                                                *socket);
      if (matched_input != &input) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     input.name.c_str(),
                     node_is_active ? LN_CompileSeverity::Error : LN_CompileSeverity::Warning,
                     SocketTypeMismatchDiagnostic(input, *socket));
        continue;
      }
      const auto links_iter = input_links.find(socket);
      const int link_count = (links_iter == input_links.end()) ? 0 :
                                                              int(links_iter->second.size());
      if (node_is_active && link_count > 1) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     input.name.c_str(),
                     LN_CompileSeverity::Error,
                     "Multiple links to a single input are not supported in V1");
      }
      if (node_is_active && input.requires_link && link_count == 0) {
        AddNodeIssue(*program,
                     tree,
                     *node,
                     input.name.c_str(),
                     LN_CompileSeverity::Error,
                     RequiredInputLinkMissingDiagnostic(input));
      }
    }
  }

  EdgeMap active_edges;
  EdgeMap active_flow_edges;
  active_edges.reserve(edges.size());
  active_flow_edges.reserve(edges.size());
  for (const ResolvedLink &link : resolved_links) {
    if (!IsNodeActive(active_nodes, link.fromnode) || !IsNodeActive(active_nodes, link.tonode)) {
      continue;
    }
    active_edges[link.fromnode].push_back(link.tonode);
    if (IsExecutionFlowLink(link, node_definitions)) {
      active_flow_edges[link.fromnode].push_back(link.tonode);
    }
  }

  TopologyResult topology;
  std::unordered_map<const blender::bNode *, uint8_t> visit_state;
  for (const blender::bNode *node : nodes) {
    if (!IsNodeActive(active_nodes, node)) {
      continue;
    }
    if (!VisitNode(*node, active_edges, visit_state, topology)) {
      const blender::bNode *cycle_node = topology.cycle_node ? topology.cycle_node : node;
      AddNodeIssue(*program,
                   tree,
                   *cycle_node,
                   nullptr,
                   LN_CompileSeverity::Error,
                   "Cycles are not supported in Logic Nodes V1");
      break;
    }
  }

  TopologyResult flow_topology;
  std::unordered_map<const blender::bNode *, uint8_t> flow_visit_state;
  for (const blender::bNode *node : nodes) {
    if (!IsNodeActive(active_nodes, node)) {
      continue;
    }
    if (!VisitNode(*node, active_flow_edges, flow_visit_state, flow_topology)) {
      const blender::bNode *cycle_node = flow_topology.cycle_node ? flow_topology.cycle_node :
                                                                    node;
      AddNodeIssue(*program,
                   tree,
                   *cycle_node,
                   nullptr,
                   LN_CompileSeverity::Error,
                   "Execution flow cycles are not supported in Logic Nodes V1");
      break;
    }
  }

  ValidateEventInputs(*program, tree, nodes, node_definitions, input_links, active_nodes);

  if (program->m_compileReport.HasErrors()) {
    program->m_compileReport.SetDisabledReason("Logic Nodes compilation failed");
    return program;
  }

  std::reverse(topology.order.begin(), topology.order.end());
  for (const blender::bNode *node : topology.order) {
    if (!IsNodeActive(active_nodes, node)) {
      continue;
    }
    program->m_sourceNodeOrder.push_back(node->identifier);
  }

  bool has_event_node = false;
  LoopFrameCache loop_frame_cache;
  g_active_loop_frame_cache = &loop_frame_cache;
  ValueCache value_cache;
  BoolExpressionCache bool_expression_cache;
  FlowEventsCache flow_events_cache;
  IntExpressionCache int_expression_cache;
  FloatExpressionCache float_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;
  for (const blender::bNode *node : topology.order) {
    if (!IsNodeActive(active_nodes, node)) {
      continue;
    }

    const LN_NodeDefinition &definition = *node_definitions.at(node);
    const InternalCompileHandler *handler = FindInternalCompileHandler(definition.kind);
    if (handler == nullptr) {
      continue;
    }

    CompilerContext context{tree,
                            *node,
                            definition,
                            *program,
                            node_definitions,
                            active_nodes,
                            input_links,
                            value_cache,
                            bool_expression_cache,
                            flow_events_cache,
                            int_expression_cache,
                            float_expression_cache,
                            string_expression_cache,
                            vector_expression_cache,
                            color_expression_cache,
                            value_expression_cache,
                            has_event_node,
                            loop_frame_cache};
    switch (handler->action_kind) {
      case CompileActionKind::None:
        break;
      case CompileActionKind::EmitEventSource:
        CompileEventSourceNode(context, handler->source_event);
        break;
      case CompileActionKind::EmitConstantOutput:
        CompileConstantOutputNode(context);
        break;
      case CompileActionKind::Custom:
        if (handler->compile_fn != nullptr) {
          handler->compile_fn(context);
        }
        break;
    }
  }

  if (program->m_compileReport.HasErrors()) {
    program->m_compileReport.SetDisabledReason("Logic Nodes compilation failed");
    return program;
  }

  if (!has_event_node) {
    program->m_compileReport.AddIssue(
        LN_CompileSeverity::Warning,
        "Logic tree has no On Init / On Update event node (flow still runs on the physics tick "
        "when driven by sensors, branches, or conditions; add On Update if you want an explicit "
        "tick root or to silence this hint)");
  }

  g_active_loop_frame_cache = nullptr;
  return program;
}
