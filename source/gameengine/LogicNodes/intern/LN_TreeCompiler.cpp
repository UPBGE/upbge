/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler.cpp
 *  \ingroup logicnodes
 */

#include "LN_TreeCompiler.h"
#include "LN_FormulaEval.hh"
#include "LN_InputMap.h"
#include "LN_SoundGroups.h"
#include "LN_TreeCompiler_internal.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
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
#include "KX_Parenting.h"

#include "BKE_colortools.hh"
#include "DNA_color_types.h"

namespace ln_compiler {

static_assert(LN_CURVE_SAMPLE_COUNT == CM_TABLE + 1,
              "Logic Nodes curve tables must match Blender CurveMapping resolution");

thread_local LoopFrameCache *g_active_loop_frame_cache = nullptr;

struct TopologyResult {
  std::vector<const blender::bNode *> order;
  const blender::bNode *cycle_node = nullptr;
};

struct FlowPath {
  LN_Event event = LN_Event::OnUpdate;
  uint32_t bool_guard_expr_index = LN_INVALID_INDEX;
  uint32_t loop_frame_index = LN_INVALID_INDEX;
  uint32_t completed_loop_frame_index = LN_INVALID_INDEX;
  /** Started by a UI event: its instructions also run while the game is paused. */
  bool ui_event_route = false;
  /** Started by While Paused: it runs only while the game is paused. */
  bool paused_route = false;
  /** Started by an On UI Event: its query's runtime state. Its instructions run again for each
   * further event of a frame. Deferred results (Opened, Done of a Delay...) are not part of it:
   * ContinuationPaths starts fresh paths. */
  uint32_t ui_event_state_index = LN_INVALID_INDEX;
  /** A Loop body: how much of execution_order its incoming path had. A Loop fed by several
   * paths gets a body for each, and each must stay one block of its own. */
  uint32_t loop_order_depth = 0;
  /* Lexicographic route order. Sequence and Any Flow append their semantic socket rank, while
   * other routes append their deterministic event-local instruction index. Runtime never pays for
   * this compiler-only representation. */
  std::vector<uint32_t> execution_order;
};

struct InstructionSchedule {
  std::array<std::vector<std::vector<uint32_t>>, 3> execution_orders;
  std::array<std::vector<uint32_t>, 3> loop_order_depths;
  std::array<std::vector<uint32_t>, 3> instruction_pulse_expressions;
  std::array<std::vector<uint32_t>, 3> instruction_index_vector_expressions;

  void RecordInstruction(const LN_Event event,
                         const uint32_t instruction_index,
                         const std::vector<uint32_t> &execution_order,
                         const uint32_t loop_order_depth)
  {
    std::vector<std::vector<uint32_t>> &orders = execution_orders[size_t(event)];
    std::vector<uint32_t> &depths = loop_order_depths[size_t(event)];
    if (orders.size() <= instruction_index) {
      orders.resize(size_t(instruction_index) + 1);
      depths.resize(size_t(instruction_index) + 1, 0);
    }
    orders[instruction_index] = execution_order;
    depths[instruction_index] = loop_order_depth;
  }

  void RecordInstructionPulseExpression(const LN_Event event, const uint32_t expression_index)
  {
    instruction_pulse_expressions[size_t(event)].push_back(expression_index);
  }

  void RecordInstructionIndexVectorExpression(const LN_Event event,
                                              const uint32_t expression_index)
  {
    instruction_index_vector_expressions[size_t(event)].push_back(expression_index);
  }
};

thread_local InstructionSchedule *g_active_instruction_schedule = nullptr;
thread_local CompiledOutputCaches g_compiled_output_caches;

class ScopedCompilerScratchState {
 public:
  ScopedCompilerScratchState(LoopFrameCache &loop_frame_cache,
                             InstructionSchedule &instruction_schedule,
                             const CompiledOutputCaches &compiled_output_caches)
      : m_previous_loop_frame_cache(g_active_loop_frame_cache),
        m_previous_instruction_schedule(g_active_instruction_schedule),
        m_previous_compiled_output_caches(g_compiled_output_caches)
  {
    g_active_loop_frame_cache = &loop_frame_cache;
    g_active_instruction_schedule = &instruction_schedule;
    g_compiled_output_caches = compiled_output_caches;
  }

  ~ScopedCompilerScratchState()
  {
    g_active_loop_frame_cache = m_previous_loop_frame_cache;
    g_active_instruction_schedule = m_previous_instruction_schedule;
    g_compiled_output_caches = m_previous_compiled_output_caches;
  }

 private:
  LoopFrameCache *m_previous_loop_frame_cache;
  InstructionSchedule *m_previous_instruction_schedule;
  CompiledOutputCaches m_previous_compiled_output_caches;
};

LN_Event ContinuationEvent(const LN_Event source_event)
{
  return source_event == LN_Event::OnUpdate ? LN_Event::OnUpdate : LN_Event::OnFixedUpdate;
}

LN_RequiredPhase EffectiveRequiredPhase(const LN_NodeDefinition &definition,
                                        const blender::bNode &node)
{
  /* Set Object Attribute deliberately combines simulation transforms with two render-only
   * attributes. Keep Color and Visible available to frame UI/visual logic without permitting
   * position, orientation, scale, or velocity writes between physics steps. */
  if (definition.kind == LN_NodeKind::SetObjectAttribute && node.custom1 != 11 &&
      node.custom1 != 12)
  {
    return LN_RequiredPhase::FixedUpdate;
  }
  return definition.required_phase;
}

/** Logic + Physics nodes that UI routes and While Paused routes may use too: they place, parent
 * or remove objects (a tooltip, a sliding panel, a paused menu's preview) but never move them by
 * physics. Frame logic is tracked for render interpolation, so such writes are not undone. */
static bool UIRouteAllowsNode(const LN_NodeDefinition &definition, const blender::bNode &node)
{
  switch (definition.kind) {
    case LN_NodeKind::SetObjectAttribute:
      /* Not the linear and angular velocities. */
      return !ELEM(node.custom1, 2, 3, 8, 9);
    case LN_NodeKind::SetParent:
    case LN_NodeKind::RemoveParent:
    case LN_NodeKind::RemoveObject:
      return true;
    default:
      return false;
  }
}

bool FlowEventMatchesRequiredPhase(const LN_Event event, const LN_RequiredPhase required_phase)
{
  switch (required_phase) {
    case LN_RequiredPhase::None:
      return true;
    case LN_RequiredPhase::OnInit:
      return event == LN_Event::OnInit;
    case LN_RequiredPhase::Update:
      return event == LN_Event::OnUpdate;
    case LN_RequiredPhase::FixedUpdate:
      /* Initialization is a safe, deterministic place to establish simulation state before the
       * first physics step. Multi-tick continuations of On Init resume on Fixed Update. */
      return event == LN_Event::OnInit || event == LN_Event::OnFixedUpdate;
  }
  return false;
}

const char *RequiredPhaseName(const LN_RequiredPhase required_phase)
{
  switch (required_phase) {
    case LN_RequiredPhase::OnInit:
      return "On Init";
    case LN_RequiredPhase::Update:
      return "Frame Update";
    case LN_RequiredPhase::FixedUpdate:
      return "Logic + Physics";
    case LN_RequiredPhase::None:
      break;
  }
  return "any phase";
}

struct FlowEventsResult {
  std::vector<FlowPath> paths;
  bool valid = true;
};

std::vector<FlowPath> ContinuationPaths(const FlowEventsResult &source_flow,
                                        const uint32_t guard_expression)
{
  std::vector<FlowPath> paths;
  for (const FlowPath &source_path : source_flow.paths) {
    const LN_Event event = ContinuationEvent(source_path.event);
    const auto existing = std::find_if(
        paths.begin(), paths.end(), [event](const FlowPath &path) { return path.event == event; });
    if (existing != paths.end()) {
      existing->ui_event_route = existing->ui_event_route || source_path.ui_event_route;
      existing->paused_route = existing->paused_route && source_path.paused_route;
      continue;
    }
    FlowPath path;
    path.event = event;
    path.bool_guard_expr_index = guard_expression;
    /* A result of something a UI event started (Opened, Done...) belongs to that UI route. */
    path.ui_event_route = source_path.ui_event_route;
    path.paused_route = source_path.paused_route;
    paths.push_back(path);
  }
  return paths;
}

/* One Loop has one state, so ticks of two clocks would both advance it. */
constexpr const char *kLoopMixedDomainsError =
    "Loop cannot mix Frame Update and Logic + Physics trigger paths because one loop has one "
    "clock domain";

bool MixesRecurringContinuationDomains(const FlowEventsResult &source_flow)
{
  std::optional<LN_Event> domain;
  for (const FlowPath &path : source_flow.paths) {
    const LN_Event path_domain = ContinuationEvent(path.event);
    if (domain && *domain != path_domain) {
      return true;
    }
    domain = path_domain;
  }
  return false;
}

using FlowEventsCache = std::unordered_map<const blender::bNodeSocket *, FlowEventsResult>;

static void AddLoopStartEvents(LN_Program &program,
                               const uint32_t loop_frame_index,
                               const std::vector<FlowPath> &incoming_paths)
{
  LN_LoopFrame frame = program.GetLoopFrames()[loop_frame_index];
  for (const FlowPath &path : incoming_paths) {
    frame.start_event_mask |= uint8_t(1u << uint8_t(path.event));
  }
  program.UpdateLoopFrame(loop_frame_index, frame);
}

uint32_t AppendFlowInstruction(LN_Program &program,
                               const FlowPath &path,
                               LN_Instruction instruction)
{
  const LN_Event event = path.event;
  instruction.bool_guard_expr_index = path.bool_guard_expr_index;
  instruction.loop_frame_index = path.loop_frame_index;
  instruction.completed_loop_frame_index = path.completed_loop_frame_index;
  instruction.ui_event_state_index = path.ui_event_state_index;
  if (path.ui_event_route) {
    instruction.flags |= LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE;
  }
  const uint32_t instruction_index = program.AddInstruction(event, instruction);
  if (g_active_instruction_schedule != nullptr) {
    g_active_instruction_schedule->RecordInstruction(
        event, instruction_index, path.execution_order, path.loop_order_depth);
  }
  return instruction_index;
}

static uint32_t AddInstructionPulseExpression(LN_Program &program,
                                              const LN_BoolExpressionKind kind,
                                              const LN_Event event,
                                              const uint32_t instruction_index)
{
  LN_BoolExpression expression;
  expression.kind = kind;
  expression.input0 = instruction_index;
  const uint32_t expression_index = program.AddBoolExpression(expression);
  if (g_active_instruction_schedule != nullptr) {
    g_active_instruction_schedule->RecordInstructionPulseExpression(event, expression_index);
  }
  return expression_index;
}

uint32_t CombineBoolExpressionsWithOr(LN_Program &program,
                                      const std::vector<uint32_t> &expressions)
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
           "' requires an execution output; connect boolean data to Branch.Condition or convert "
           "state changes with Boolean Edge";
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
  return "Socket '" + input.name + "' has type '" + actual + "' but native definition requires '" +
         expected + "'";
}

bool VisitNode(const blender::bNode &node,
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

std::optional<uint32_t> EnsureLoopFrame(LN_Program &program,
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
  const std::optional<uint32_t> condition = BuildPrimaryExecutionExpression(program,
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

  const std::optional<uint32_t> ticks_to_complete_expr = BuildInputIntExpression(
      program,
      loop_node,
      "Ticks to Complete",
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
  if (ticks_to_complete_expr) {
    frame.ticks_to_complete_int_expr_index = *ticks_to_complete_expr;
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

  if (frame.kind == LN_LoopKind::FromList) {
    LN_ValueExpression current_value;
    current_value.kind = LN_ValueExpressionKind::LoopCurrentValue;
    current_value.property_ref_index = frame_index;
    frame.loop_current_value_expr_index = program.AddValueExpression(current_value);
    cache.value_expressions[&loop_node] = frame.loop_current_value_expr_index;
  }

  program.UpdateLoopFrame(frame_index, frame);
  cache.indices[&loop_node] = frame_index;

  if (const blender::bNodeSocket *loop_out = FindOutputSocket(loop_node, "Loop")) {
    bool_expression_cache[loop_out] = frame.loop_active_bool_expr_index;
  }
  cache.index_expressions[&loop_node] = frame.loop_index_int_expr_index;

  return frame_index;
}

std::optional<uint32_t> GetLoopFrameIndexForNode(LN_Program &program,
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

FlowEventsResult ResolveFlowEvents(LN_Program &program,
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

static FlowEventsResult BuildConditionRouteFlowEvents(LN_Program &program,
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
    const uint32_t instruction_index = AppendFlowInstruction(
        program, source_path, route_instruction);

    const uint32_t route_expr_index = AddInstructionPulseExpression(
        program, LN_BoolExpressionKind::InstructionExecuted, source_path.event, instruction_index);
    route_expressions.push_back(route_expr_index);

    FlowPath route_path = source_path;
    route_path.bool_guard_expr_index = route_expr_index;
    route_path.execution_order.push_back(instruction_index + 1u);
    result.paths.push_back(route_path);
  }

  if (route_bool_cache != nullptr) {
    (*route_bool_cache)[&route_socket] = CombineBoolExpressionsWithOr(program, route_expressions);
  }
  flow_events_cache[&route_socket] = result;
  return result;
}

static FlowEventsResult BuildBranchRouteFlowEvents(LN_Program &program,
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

/** Every link from the route (unmuted, through reroutes) goes straight into an Open Object Menu,
 * and nothing follows those menus (their Opened, Closed and Failed lead nowhere, so no Logic +
 * Physics node depends on the step they run in). `r_pause_toggle`: one of them pauses the game
 * and toggles. */
static bool OnlyOpensMenus(const InputLinkMap &input_links,
                           const NodeDefinitionMap &node_definitions,
                           const blender::bNodeSocket &route_socket,
                           bool &r_pause_toggle)
{
  r_pause_toggle = false;
  std::unordered_set<const blender::bNode *> menus;
  for (const auto &item : input_links) {
    for (const ResolvedLink &link : item.second) {
      if (link.fromsock != &route_socket) {
        continue;
      }
      const auto target = node_definitions.find(link.tonode);
      if (target == node_definitions.end() ||
          target->second->kind != LN_NodeKind::OpenObjectMenu)
      {
        return false;
      }
      constexpr int pause_toggle = LN_UI_MENU_NODE_PAUSES_GAME | LN_UI_MENU_NODE_TOGGLES;
      r_pause_toggle = r_pause_toggle || (link.tonode->custom2 & pause_toggle) == pause_toggle;
      menus.insert(link.tonode);
    }
  }
  if (menus.empty()) {
    return false;
  }
  for (const auto &item : input_links) {
    for (const ResolvedLink &link : item.second) {
      if (menus.count(link.fromnode) != 0 &&
          IsExecutionOutputSocket(*node_definitions.at(link.fromnode), *link.fromsock))
      {
        return false;
      }
    }
  }
  return true;
}

static FlowEventsResult BuildPollingRouteFlowEvents(LN_Program &program,
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
  switch (definition.autonomous_phase) {
    case LN_AutonomousPhase::Update:
      source_path.event = LN_Event::OnUpdate;
      break;
    case LN_AutonomousPhase::FixedUpdate:
      source_path.event = LN_Event::OnFixedUpdate;
      /* A key, button or Input Action that only opens (or toggles) menus is read in the frame
       * update, as the menus are: after the UI had the key (Esc opens a pause menu and is Back in
       * it). For a pause menu with Toggle, also while the game is paused, when Logic + Physics
       * steps stop, so its key closes it again. */
      if (bool pause_toggle = false;
          ELEM(definition.kind,
               LN_NodeKind::InputAction,
               LN_NodeKind::KeyboardKey,
               LN_NodeKind::MouseButton,
               LN_NodeKind::GamepadButton) &&
          OnlyOpensMenus(input_links, node_definitions, route_socket, pause_toggle))
      {
        source_path.event = LN_Event::OnUpdate;
        source_path.ui_event_route = pause_toggle;
      }
      break;
    case LN_AutonomousPhase::None:
      AddNodeIssue(program,
                   tree,
                   source_node,
                   route_socket.name,
                   LN_CompileSeverity::Error,
                   "Internal node contract is missing an autonomous event phase");
      flow_events_cache[&route_socket] = result;
      return result;
  }
  switch (definition.kind) {
    case LN_NodeKind::OnUIEvent:
    case LN_NodeKind::OnUISound:
    case LN_NodeKind::ObjectScrollView:
    case LN_NodeKind::ObjectGridView:
      source_path.ui_event_route = true;
      break;
    default:
      break;
  }
  /* On UI Event runs its route once for every matching event of a frame, not only the first; On
   * UI Sound for every player's sound. */
  if (ELEM(definition.kind, LN_NodeKind::OnUIEvent, LN_NodeKind::OnUISound) &&
      *condition < program.GetBoolExpressions().size())
  {
    const LN_BoolExpression &fired = program.GetBoolExpressions()[*condition];
    if (ELEM(fired.kind,
             LN_BoolExpressionKind::UIElementEventFired,
             LN_BoolExpressionKind::UISoundFired) &&
        fired.input0 < program.GetQueryExpressions().size())
    {
      source_path.ui_event_state_index =
          program.GetQueryExpressions()[fired.input0].runtime_state_index;
    }
  }
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
    fallback_path.event = EffectiveRequiredPhase(definition, source_node) ==
                                  LN_RequiredPhase::FixedUpdate ?
                              LN_Event::OnFixedUpdate :
                              LN_Event::OnUpdate;
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
    flow_events_cache[&route_socket] = result;
    return result;
  }

  /* The input route only decides whether the latent state is armed. Once it
   * has been armed, the state owns its continuation. Carrying the original
   * route guard (an event pulse, branch condition or loop frame) into a later
   * tick makes a completed Delay/Timer/Pulsify/Barrier depend on a transient
   * condition that has already expired. Build one fresh continuation per
   * phase and let the latent node's completion expression be its sole guard. */
  source_flow.paths = ContinuationPaths(source_flow, LN_INVALID_INDEX);

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
      NamesMatch(route_socket.name, route_socket.identifier, "Started") ||
      NamesMatch(route_socket.name, route_socket.identifier, "Completed") ||
      NamesMatch(route_socket.name, route_socket.identifier, "Interrupted") ||
      NamesMatch(route_socket.name, route_socket.identifier, "Failed") ||
      NamesMatch(route_socket.name, route_socket.identifier, "On Start") ||
      NamesMatch(route_socket.name, route_socket.identifier, "When Done") ||
      NamesMatch(route_socket.name, route_socket.identifier, "When Reached") ||
      NamesMatch(route_socket.name, route_socket.identifier, "Reached");
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
    /* Arriving is no plain continuation: without its pulse there is no route. */
    if (NamesMatch(route_socket.name, route_socket.identifier, "When Reached") ||
        NamesMatch(route_socket.name, route_socket.identifier, "Reached"))
    {
      flow_events_cache[&route_socket] = result;
      return result;
    }
    condition = AddConstantBoolExpression(program, true);
  }

  /* Object-UI service commands complete only after their deferred scene request is flushed. A
   * downstream node can be compiled before the command-specific compiler caches its route, so the
   * generic fallback must discard expired event, branch, Once, and loop guards for these commands.
   * Ordinary command outputs (Print, Add Object, sound controls, and so on) remain same-tick
   * continuations and must retain their incoming route. */
  if (definition.kind == LN_NodeKind::OpenObjectMenu ||
      definition.kind == LN_NodeKind::CloseObjectMenu ||
      definition.kind == LN_NodeKind::SetOverlayCollection ||
      definition.kind == LN_NodeKind::RemoveOverlayCollection ||
      definition.kind == LN_NodeKind::SetUIFocus ||
      definition.kind == LN_NodeKind::TakeUIControl || definition.kind == LN_NodeKind::PlayAction)
  {
    source_flow.paths = ContinuationPaths(source_flow, LN_INVALID_INDEX);
  }
  else if (*condition < program.GetBoolExpressions().size()) {
    const LN_BoolExpressionKind condition_kind = program.GetBoolExpressions()[*condition].kind;
    if (condition_kind == LN_BoolExpressionKind::InstructionExecuted ||
        condition_kind == LN_BoolExpressionKind::InstructionReached)
    {
      /* The instruction pulse already proves that every incoming event/branch/latent guard was
       * accepted for this execution scope. Rechecking that upstream guard can incorrectly reject
       * an immediate Done route after a deferred command has consumed its completion. Keep the
       * phase, loop scope, and execution-order key; remove only the redundant boolean guard. */
      for (FlowPath &path : source_flow.paths) {
        path.bool_guard_expr_index = LN_INVALID_INDEX;
      }
    }
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

static FlowEventsResult ResolveFlowLinkEvents(LN_Program &program,
                                              const blender::bNodeTree &tree,
                                              const ResolvedLink &link,
                                              const NodeDefinitionMap &node_definitions,
                                              const InputLinkMap &input_links,
                                              ValueCache &value_cache,
                                              FloatExpressionCache &float_expression_cache,
                                              BoolExpressionCache &bool_expression_cache,
                                              FlowEventsCache &flow_events_cache,
                                              LoopFrameCache &loop_frame_cache)
{
  FlowEventsResult result;
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
      if (definition.kind == LN_NodeKind::WhilePaused) {
        LN_BoolExpression paused_expression;
        paused_expression.kind = LN_BoolExpressionKind::GamePaused;
        path.bool_guard_expr_index = program.AddBoolExpression(paused_expression);
        path.paused_route = true;
      }
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
                                                    flow_events_cache,
                                                    loop_frame_cache);
      break;
    }
    case FlowResolverKind::PollingEventRoute: {
      result = BuildPollingRouteFlowEvents(program,
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
      /* Wait for Next Frame and Tween Value finish what their input routes started: when a UI
       * event started them, their results belong to that UI route and run while paused too. */
      if (result.valid &&
          (definition.kind == LN_NodeKind::OnNextFrame ||
           definition.kind == LN_NodeKind::TweenValue) &&
          !std::all_of(result.paths.begin(), result.paths.end(), [](const FlowPath &path) {
            return path.ui_event_route;
          }))
      {
        bool started_by_ui = false;
        for (const LN_PinDefinition &pin : definition.inputs) {
          if (pin.kind != LN_PinKind::Execution || started_by_ui) {
            continue;
          }
          const FlowEventsResult input_flow = ResolveFlowEvents(program,
                                                                tree,
                                                                *link.fromnode,
                                                                pin.name,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache,
                                                                float_expression_cache,
                                                                bool_expression_cache,
                                                                flow_events_cache,
                                                                loop_frame_cache);
          started_by_ui = std::any_of(input_flow.paths.begin(),
                                      input_flow.paths.end(),
                                      [](const FlowPath &path) { return path.ui_event_route; });
        }
        if (started_by_ui) {
          for (FlowPath &path : result.paths) {
            path.ui_event_route = true;
          }
          flow_events_cache[link.fromsock] = result;
        }
      }
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
      const bool is_loop_output = NamesMatch(
          link.fromsock->name, link.fromsock->identifier, "Loop");
      const bool is_completed_output = NamesMatch(
          link.fromsock->name, link.fromsock->identifier, "Completed");
      if (!is_loop_output && !is_completed_output) {
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
      if (MixesRecurringContinuationDomains(result)) {
        AddNodeIssue(program,
                     tree,
                     *link.fromnode,
                     link.fromsock->name,
                     LN_CompileSeverity::Error,
                     kLoopMixedDomainsError);
        result.valid = false;
        break;
      }
      AddLoopStartEvents(program, *loop_frame_index, result.paths);

      if (is_completed_output) {
        std::vector<FlowPath> completed_paths;
        completed_paths.reserve(result.paths.size() * 2);
        /* A Loop fed by several paths completes once, so Completed runs once: one route per
         * event, after the latest body. Started by a UI event on any path, it may run while
         * paused. */
        const auto add_completed = [&](FlowPath completed_path) {
          const auto same = std::find_if(
              completed_paths.begin(), completed_paths.end(), [&](const FlowPath &other) {
                return other.event == completed_path.event;
              });
          if (same == completed_paths.end()) {
            completed_paths.push_back(std::move(completed_path));
            return;
          }
          const bool ui_event_route = same->ui_event_route || completed_path.ui_event_route;
          const bool paused_route = same->paused_route && completed_path.paused_route;
          /* Fed by one On UI Event, Completed runs after each of its events; fed by more, once. */
          const uint32_t ui_event_state_index = same->ui_event_state_index ==
                                                        completed_path.ui_event_state_index ?
                                                    same->ui_event_state_index :
                                                    LN_INVALID_INDEX;
          if (std::lexicographical_compare(same->execution_order.begin(),
                                           same->execution_order.end(),
                                           completed_path.execution_order.begin(),
                                           completed_path.execution_order.end()))
          {
            *same = std::move(completed_path);
          }
          same->ui_event_route = ui_event_route;
          same->paused_route = paused_route;
          same->ui_event_state_index = ui_event_state_index;
        };
        for (const FlowPath &path : result.paths) {
          if (path.loop_frame_index != LN_INVALID_INDEX) {
            AddNodeIssue(program,
                         tree,
                         *link.fromnode,
                         link.fromsock->name,
                         LN_CompileSeverity::Error,
                         "Loop Completed cannot run inside another Loop body; nested Loop bodies "
                         "are not supported");
            result.valid = false;
            break;
          }
          FlowPath completed_path = path;
          /* Keep completion blocks lexicographically after every iteration/body route sharing
           * the same incoming flow. The runtime additionally verifies the frame completion
           * scope, so this ordering is deterministic even when node declaration order differs. */
          completed_path.bool_guard_expr_index = LN_INVALID_INDEX;
          completed_path.completed_loop_frame_index = *loop_frame_index;
          completed_path.execution_order.push_back(std::numeric_limits<uint32_t>::max());
          add_completed(completed_path);

          const LN_Event continuation_event = ContinuationEvent(path.event);
          if (continuation_event != path.event) {
            completed_path.event = continuation_event;
            add_completed(std::move(completed_path));
          }
        }
        if (result.valid) {
          result.paths = std::move(completed_paths);
        }
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

      std::vector<FlowPath> loop_paths;
      loop_paths.reserve(result.paths.size() * 2);
      for (const FlowPath &path : result.paths) {
        if (path.loop_frame_index != LN_INVALID_INDEX) {
          AddNodeIssue(program,
                       tree,
                       *link.fromnode,
                       link.fromsock->name,
                       LN_CompileSeverity::Error,
                       "Nested Loop bodies are not supported; flatten the work or trigger the "
                       "inner Loop from a separate non-loop event path");
          result.valid = false;
          break;
        }
        FlowPath loop_path = path;
        /* The incoming flow is consumed once by LN_LoopFrame::trigger_bool_expr_index in the
         * outer event scope.  Rechecking a scoped route pulse inside an iteration would always
         * fail because loop iterations intentionally use distinct execution scopes. */
        loop_path.bool_guard_expr_index = *loop_guard;
        loop_path.loop_frame_index = *loop_frame_index;
        loop_path.loop_order_depth = uint32_t(loop_path.execution_order.size());
        loop_paths.push_back(loop_path);

        /* On Init has no later On Init tick. Mirror its body and completion routes into Fixed
         * Update so an active multi-tick frame can resume without replaying the one-shot trigger.
         * Recurring phases already continue in their own domain and need no duplicate route. */
        const LN_Event continuation_event = ContinuationEvent(path.event);
        if (continuation_event != path.event) {
          loop_path.event = continuation_event;
          loop_paths.push_back(std::move(loop_path));
        }
      }
      if (result.valid) {
        result.paths = std::move(loop_paths);
      }
      break;
    }
    case FlowResolverKind::Invalid: {
      const blender::bNodeSocket *fromsock = link.fromsock;
      const auto cached = fromsock != nullptr ? flow_events_cache.find(fromsock) :
                                               flow_events_cache.end();
      if (cached != flow_events_cache.end()) {
        result = cached->second;
      }
      else if (handler->descriptor.info.emits_commands && fromsock != nullptr) {
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

FlowEventsResult ResolveFlowEvents(LN_Program &program,
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
  result.valid = false;
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return result;
  }

  const auto target_definition_iter = node_definitions.find(&node);
  if (target_definition_iter != node_definitions.end()) {
    const LN_PinDefinition *input_pin = FindPinDefinition(target_definition_iter->second->inputs,
                                                          *socket);
    if (input_pin == nullptr || input_pin->kind != LN_PinKind::Execution) {
      return result;
    }
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter == input_links.end() || links_iter->second.empty()) {
    return result;
  }

  for (const ResolvedLink &link : links_iter->second) {
    FlowEventsResult link_result = ResolveFlowLinkEvents(program,
                                                         tree,
                                                         link,
                                                         node_definitions,
                                                         input_links,
                                                         value_cache,
                                                         float_expression_cache,
                                                         bool_expression_cache,
                                                         flow_events_cache,
                                                         loop_frame_cache);
    if (!link_result.valid) {
      result.paths.clear();
      return result;
    }
    result.paths.insert(result.paths.end(), link_result.paths.begin(), link_result.paths.end());
  }
  result.valid = !result.paths.empty();
  if (!result.valid || target_definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_RequiredPhase required_phase = EffectiveRequiredPhase(*target_definition_iter->second,
                                                                  node);
  if (required_phase == LN_RequiredPhase::None) {
    return result;
  }
  const bool ui_may_use = UIRouteAllowsNode(*target_definition_iter->second, node);
  const bool phase_matches = std::all_of(
      result.paths.begin(), result.paths.end(), [&](const FlowPath &path) {
        return FlowEventMatchesRequiredPhase(path.event, required_phase) ||
               (ui_may_use && path.event == LN_Event::OnUpdate &&
                (path.ui_event_route || path.paused_route));
      });
  if (!phase_matches) {
    AddNodeIssue(program,
                 tree,
                 node,
                 socket->name,
                 LN_CompileSeverity::Error,
                 std::string("This node requires ") + RequiredPhaseName(required_phase) +
                     " flow; connect it to a matching event or execution path");
    result.paths.clear();
    result.valid = false;
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

  const ResolvedLink *InputLink(const char *pin_name) const
  {
    const blender::bNodeSocket *socket = FindInputSocket(node, pin_name);
    if (socket == nullptr) {
      return nullptr;
    }
    const auto links_iter = input_links.find(socket);
    if (links_iter == input_links.end() || links_iter->second.empty()) {
      return nullptr;
    }
    return &links_iter->second.front();
  }

  LN_ValueType LinkedInputValueType(const char *pin_name) const
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr || link->fromnode == nullptr || link->fromsock == nullptr) {
      return LN_ValueType::Generic;
    }
    const auto definition_iter = node_definitions.find(link->fromnode);
    if (definition_iter == node_definitions.end()) {
      return LN_ValueType::Generic;
    }
    switch (definition_iter->second->kind) {
      case LN_NodeKind::CombineXYZW:
        return LN_ValueType::Vector4;
      case LN_NodeKind::ResizeVector:
        return std::clamp(int(link->fromnode->custom1) + 2, 2, 4) == 4 ? LN_ValueType::Vector4 :
                                                                         LN_ValueType::Vector;
      case LN_NodeKind::MatrixToXYZ:
        return link->fromnode->custom1 == 1 ? LN_ValueType::Rotation : LN_ValueType::Vector;
      default:
        break;
    }
    const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->outputs,
                                                    *link->fromsock);
    return pin != nullptr ? pin->value_type : LN_ValueType::Generic;
  }

  std::optional<uint32_t> BuildLinkedBool(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputBoolExpression(program,
                                     *link->fromnode,
                                     *link->fromsock,
                                     node_definitions,
                                     input_links,
                                     value_cache,
                                     float_expression_cache,
                                     bool_expression_cache);
  }

  std::optional<uint32_t> BuildLinkedInt(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputIntExpression(program,
                                    *link->fromnode,
                                    *link->fromsock,
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

  std::optional<uint32_t> BuildLinkedFloat(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputFloatExpression(program,
                                      *link->fromnode,
                                      *link->fromsock,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      &bool_expression_cache,
                                      &vector_expression_cache);
  }

  std::optional<uint32_t> BuildLinkedString(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputStringExpression(program,
                                       *link->fromnode,
                                       *link->fromsock,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       string_expression_cache);
  }

  std::optional<uint32_t> BuildLinkedVector(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputVectorExpression(program,
                                       *link->fromnode,
                                       *link->fromsock,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       vector_expression_cache);
  }

  std::optional<uint32_t> BuildLinkedColor(const char *pin_name)
  {
    const ResolvedLink *link = InputLink(pin_name);
    if (link == nullptr) {
      return std::nullopt;
    }
    return BuildOutputColorExpression(program,
                                      *link->fromnode,
                                      *link->fromsock,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      color_expression_cache);
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

    AddNodeIssue(
        program, tree, node, flow_pin->name.c_str(), LN_CompileSeverity::Error, error_message);
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

/**
 * Cache the typed boundary expressions needed by active consumers of a custom generic output.
 *
 * Custom expression handlers cache their result in ValueExpressionCache. The typed output
 * builders cannot recreate those handlers lazily because they intentionally operate without the
 * full CompilerContext. Preparing only the conversions that are actually linked keeps generic
 * values composable with every typed socket while avoiding unused runtime expressions.
 */
static void CacheLinkedGenericOutputConversions(CompilerContext &context)
{
  for (const blender::bNodeSocket *output =
           static_cast<const blender::bNodeSocket *>(context.node.outputs.first);
       output;
       output = output->next)
  {
    const LN_PinDefinition *output_pin = FindPinDefinition(context.definition.outputs, *output);
    if (output_pin == nullptr || output_pin->value_type != LN_ValueType::Generic) {
      continue;
    }

    const auto value_iter = context.value_expression_cache.find(output);
    if (value_iter == context.value_expression_cache.end() || !value_iter->second) {
      continue;
    }

    bool needs_bool = false;
    bool needs_int = false;
    bool needs_float = false;
    bool needs_string = false;
    bool needs_vector = false;
    bool needs_color = false;
    for (const auto &input_item : context.input_links) {
      for (const ResolvedLink &link : input_item.second) {
        if (link.fromnode != &context.node || link.fromsock != output ||
            !IsNodeActive(context.active_nodes, link.tonode))
        {
          continue;
        }
        const auto target_definition = context.node_definitions.find(link.tonode);
        if (target_definition == context.node_definitions.end()) {
          continue;
        }
        const LN_PinDefinition *target_pin = FindPinDefinition(target_definition->second->inputs,
                                                               *link.tosock);
        if (target_pin == nullptr) {
          continue;
        }
        switch (target_pin->value_type) {
          case LN_ValueType::Bool:
            needs_bool = true;
            break;
          case LN_ValueType::Int:
            needs_int = true;
            break;
          case LN_ValueType::Float:
            needs_float = true;
            break;
          case LN_ValueType::String:
            needs_string = true;
            break;
          case LN_ValueType::Vector:
          case LN_ValueType::Rotation:
            needs_vector = true;
            break;
          case LN_ValueType::Color:
            needs_color = true;
            break;
          default:
            break;
        }
      }
    }

    const uint32_t value_expression = *value_iter->second;
    if (needs_bool && !context.bool_expression_cache.contains(output)) {
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::FromGenericValue;
      expression.input0 = value_expression;
      context.bool_expression_cache[output] = context.program.AddBoolExpression(expression);
    }
    if (needs_int && !context.int_expression_cache.contains(output)) {
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::FromGenericValue;
      expression.input0 = value_expression;
      context.int_expression_cache[output] = context.program.AddIntExpression(expression);
    }
    if (needs_float && !context.float_expression_cache.contains(output)) {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::FromGenericValue;
      expression.input0 = value_expression;
      context.float_expression_cache[output] = context.program.AddFloatExpression(expression);
    }
    if (needs_string && !context.string_expression_cache.contains(output)) {
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind::FromGenericValue;
      /* Text reads its source through value_expr_index, unlike the other conversions. */
      expression.value_expr_index = value_expression;
      context.string_expression_cache[output] = context.program.AddStringExpression(expression);
    }
    if (needs_vector && !context.vector_expression_cache.contains(output)) {
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::FromGenericValue;
      expression.output_type = LN_ValueType::Vector;
      expression.input0 = value_expression;
      context.vector_expression_cache[output] = context.program.AddVectorExpression(expression);
    }
    if (needs_color && !context.color_expression_cache.contains(output)) {
      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind::FromGenericValue;
      expression.input0 = value_expression;
      context.color_expression_cache[output] = context.program.AddColorExpression(expression);
    }
  }
}

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
    const bool has_static_target = target && target->type == LN_ValueType::ObjectRef &&
                                   target->exists;
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

static const blender::bNodeSocket *FindInputSocketByIdentifier(const blender::bNode &node,
                                                               const char *identifier)
{
  for (const blender::bNodeSocket *socket =
           static_cast<const blender::bNodeSocket *>(node.inputs.first);
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
    case 13:
      return "localScale";
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
    case 13:
      return LN_OpCode::SetLocalScale;
    default:
      return std::nullopt;
  }
}

static void ConfigureVectorCommandInstruction(LN_Instruction &instruction, const LN_OpCode opcode)
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
  return AddBinaryFloatExpression(
      program, LN_FloatExpressionKind::Add, current_expr, scaled_delta_expr);
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
  return AddBinaryVectorExpression(
      program, LN_VectorExpressionKind::Add, current_expr, scaled_delta_expr);
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

  /* Alpha has no component-mask toggle and is always written. Avoid reading the current object
   * color entirely when the default RGB mask writes every component. */
  if (SetObjectAttributeUsesFullDefaultMask(context)) {
    return color_expr;
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

  const uint32_t current_r = AddColorComponentFloatExpression(
      context.program, current_color_expr, 0);
  const uint32_t current_g = AddColorComponentFloatExpression(
      context.program, current_color_expr, 1);
  const uint32_t current_b = AddColorComponentFloatExpression(
      context.program, current_color_expr, 2);
  const uint32_t input_r = AddColorComponentFloatExpression(context.program, *color_expr, 0);
  const uint32_t input_g = AddColorComponentFloatExpression(context.program, *color_expr, 1);
  const uint32_t input_b = AddColorComponentFloatExpression(context.program, *color_expr, 2);
  const uint32_t input_a = AddColorComponentFloatExpression(context.program, *color_expr, 3);
  const uint32_t mask_x = AddVectorComponentFloatExpression(context.program, *mask_expr, 0);
  const uint32_t mask_y = AddVectorComponentFloatExpression(context.program, *mask_expr, 1);
  const uint32_t mask_z = AddVectorComponentFloatExpression(context.program, *mask_expr, 2);

  LN_ColorExpression merged_color_expression;
  merged_color_expression.kind = LN_ColorExpressionKind::Combine;
  merged_color_expression.input0 = AddMaskedFloatExpression(
      context.program, current_r, input_r, mask_x);
  merged_color_expression.input1 = AddMaskedFloatExpression(
      context.program, current_g, input_g, mask_y);
  merged_color_expression.input2 = AddMaskedFloatExpression(
      context.program, current_b, input_b, mask_z);
  merged_color_expression.input3 = input_a;
  return context.program.AddColorExpression(merged_color_expression);
}

/** Position, rotation, scale and transform: a move Smooth Motion could draw across. */
static bool SetObjectAttributeUsesTeleport(const int attribute_type)
{
  return ELEM(attribute_type, 0, 1, 4, 5, 6, 7, 10, 13);
}

/** The Teleport input, when it can be true (nothing is added for an unlinked, unchecked one). */
static std::optional<uint32_t> BuildSetObjectAttributeTeleport(CompilerContext &context)
{
  if (!SetObjectAttributeUsesTeleport(context.node.custom1) ||
      FindInputSocket(context.node, "Teleport") == nullptr)
  {
    return std::nullopt;
  }
  if (!context.InputIsLinked("Teleport")) {
    const std::optional<LN_Value> value = context.ReadValue("Teleport", LN_ValueType::Bool);
    if (!value || value->type != LN_ValueType::Bool || !value->bool_value) {
      return std::nullopt;
    }
  }
  return context.BuildBool("Teleport");
}

/** After the write: the move is a jump Smooth Motion draws at once. */
static void AppendSetObjectAttributeTeleport(CompilerContext &context,
                                             const FlowPath &path,
                                             const std::optional<uint32_t> &object_expr,
                                             const uint32_t teleport_expr)
{
  LN_Instruction instruction;
  instruction.opcode = LN_OpCode::ResetSmoothing;
  instruction.source_ref_index = context.MakeSourceRef("Teleport");
  if (object_expr) {
    instruction.value_expr_index = *object_expr;
  }
  instruction.bool_expr_index = teleport_expr;
  AppendFlowInstruction(context.program, path, instruction);
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<uint32_t> object_expr = object_socket ?
                                                  context.BuildOptionalObject(object_socket) :
                                                  std::optional<uint32_t>();
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      flow_error);
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

#define DEFINE_VECTOR_COMMAND_HANDLER(name, \
                                      opcode, \
                                      value_socket, \
                                      object_socket, \
                                      flow_socket, \
                                      input_error, \
                                      flow_error, \
                                      use_rotation) \
  void name(CompilerContext &context) \
  { \
    CompileVectorCommand(context, \
                         LN_OpCode::opcode, \
                         value_socket, \
                         object_socket, \
                         flow_socket, \
                         input_error, \
                         flow_error, \
                         use_rotation); \
  }

#define DEFINE_VECTOR_BOOL_COMMAND_HANDLER(name, \
                                           opcode, \
                                           value_socket, \
                                           bool_socket, \
                                           object_socket, \
                                           flow_socket, \
                                           input_error, \
                                           bool_error, \
                                           flow_error, \
                                           use_rotation) \
  void name(CompilerContext &context) \
  { \
    CompileVectorBoolCommand(context, \
                             LN_OpCode::opcode, \
                             value_socket, \
                             bool_socket, \
                             object_socket, \
                             flow_socket, \
                             input_error, \
                             bool_error, \
                             flow_error, \
                             use_rotation); \
  }

#define DEFINE_BOOL_COMMAND_HANDLER( \
    name, opcode, value_socket, object_socket, flow_socket, input_error, flow_error) \
  void name(CompilerContext &context) \
  { \
    CompileBoolCommand(context, \
                       LN_OpCode::opcode, \
                       value_socket, \
                       object_socket, \
                       flow_socket, \
                       input_error, \
                       flow_error); \
  }

#define DEFINE_BOOL_PAIR_COMMAND_HANDLER(name, \
                                         opcode, \
                                         value_socket, \
                                         secondary_socket, \
                                         object_socket, \
                                         flow_socket, \
                                         input_error, \
                                         secondary_error, \
                                         flow_error) \
  void name(CompilerContext &context) \
  { \
    CompileBoolPairCommand(context, \
                           LN_OpCode::opcode, \
                           value_socket, \
                           secondary_socket, \
                           object_socket, \
                           flow_socket, \
                           input_error, \
                           secondary_error, \
                           flow_error); \
  }

#define DEFINE_INT_COMMAND_HANDLER( \
    name, opcode, value_socket, object_socket, flow_socket, input_error, flow_error) \
  void name(CompilerContext &context) \
  { \
    CompileIntCommand(context, \
                      LN_OpCode::opcode, \
                      value_socket, \
                      object_socket, \
                      flow_socket, \
                      input_error, \
                      flow_error); \
  }

#define DEFINE_COLOR_COMMAND_HANDLER( \
    name, opcode, value_socket, object_socket, flow_socket, input_error, flow_error) \
  void name(CompilerContext &context) \
  { \
    CompileColorCommand(context, \
                        LN_OpCode::opcode, \
                        value_socket, \
                        object_socket, \
                        flow_socket, \
                        input_error, \
                        flow_error); \
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
void CompileSetCursorVisibilityNode(CompilerContext &context)
{
  const std::optional<uint32_t> visible_expr = context.BuildBool("Visible");
  if (!visible_expr) {
    context.AddError("Visible", "Set Cursor Visibility requires a boolean visible input");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = ResolveCommandFlow(
      context, "Flow", "Set Cursor Visibility flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCursorVisibility;
    instruction.source_ref_index = context.MakeSourceRef("Visible");
    instruction.bool_expr_index = *visible_expr;
    /* Show Only in Menus: the UI shows it while a menu uses player 0's mouse. */
    instruction.int_value = context.node.custom1 == 1 ? 1 : 0;
    AppendFlowInstruction(context.program, path, instruction);
  }
}
DEFINE_BOOL_COMMAND_HANDLER(CompileSetGamePausedNode,
                            SetGamePaused,
                            "Paused",
                            nullptr,
                            "Flow",
                            "Set Game Paused requires a boolean paused input",
                            "Set Game Paused flow must be driven by an event or branch")

DEFINE_BOOL_COMMAND_HANDLER(CompileSetPhysicsNode,
                            SetPhysics,
                            "Active",
                            "Object",
                            "Flow",
                            "Enable Physics Body requires a boolean enabled input",
                            "Enable Physics Body flow must be driven by an event or branch")
DEFINE_BOOL_COMMAND_HANDLER(CompileSetPhysicsPausedNode,
                            SetPhysicsPaused,
                            "Paused",
                            nullptr,
                            "Flow",
                            "Set Physics Paused requires a boolean paused input",
                            "Set Physics Paused flow must be driven by an event or branch")

void CompileSetDynamicsNode(CompilerContext &context)
{
  constexpr int32_t max_physics_type_mode = 7;
  if (context.node.custom1 < 0 || context.node.custom1 > max_physics_type_mode) {
    context.AddError("Physics State", "Set Physics State has an unsupported mode");
    return;
  }

  constexpr int ghost_mode = 2;
  uint32_t enabled_expr = LN_INVALID_INDEX;
  bool enabled = true;
  if (context.node.custom1 == ghost_mode) {
    const std::optional<uint32_t> expr = context.BuildBool("Enabled");
    if (!expr) {
      context.AddError("Enabled", "Set Physics State Ghost mode requires a boolean enabled input");
      return;
    }
    enabled_expr = *expr;
    enabled = BoolExpressionConstantFallback(context.program, *expr);
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Physics State flow must be driven by an event or branch");
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

static std::optional<uint32_t> WrapTypedInputAsValue(CompilerContext &context,
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

/** A text input as a value: a typed text stays constant, anything linked (a number too) is
 * turned into text when it runs. */
static std::optional<uint32_t> BuildTextInputAsValue(CompilerContext &context,
                                                     const char *socket_name)
{
  const std::optional<uint32_t> text_expr = context.BuildString(socket_name);
  if (!text_expr) {
    return std::nullopt;
  }
  const LN_StringExpression &text = context.program.GetStringExpressions()[*text_expr];
  if (text.kind == LN_StringExpressionKind::Constant) {
    LN_Value value;
    value.type = LN_ValueType::String;
    value.exists = true;
    value.string_value = text.string_value;
    return AddConstantValueExpression(context.program, value);
  }
  return WrapTypedInputAsValue(context, LN_ValueExpressionKind::FromString, text_expr);
}

static std::optional<uint32_t> BuildEditorNodeValueExpression(CompilerContext &context)
{
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  if (InputSocketIdentifierIsLinked(context.node, value_socket, context.input_links)) {
    return context.BuildValueExpression(value_socket);
  }
  if (context.node.custom1 != 7 &&
      InputSocketIdentifierIsLinked(context.node, "Value", context.input_links))
  {
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
  const bool per_object_only = (context.node.custom2 & set_material_node_value_per_object_only) !=
                               0;
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
      context.AddWarning(
          "Material", std::string(node_label) + " skipped: no Material is selected or connected");
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
void CompileSetSplitScreenNode(CompilerContext &context);
void CompileSetPlayerGamepadNode(CompilerContext &context);
void CompileGetPlayerGamepadNode(CompilerContext &context);
void CompileGetMenuPlayerNode(CompilerContext &context);
void CompileSetCameraViewportNode(CompilerContext &context);
void CompileSetCameraVisibilityNode(CompilerContext &context);
void CompileSetCollectionMemberNode(CompilerContext &context);
void CompileSetPlayerPointerNode(CompilerContext &context);
void CompileTakeUIControlNode(CompilerContext &context);
void CompileGetPlayerViewNode(CompilerContext &context);
void CompileSetCameraFovNode(CompilerContext &context);
void CompileSetCameraOrthoScaleNode(CompilerContext &context);
void CompileSetLightPowerNode(CompilerContext &context);
void CompileSetResolutionNode(CompilerContext &context);
void CompileSetVSyncNode(CompilerContext &context);
void CompileTogglePropertyNode(CompilerContext &context);
void CompileModifyPropertyNode(CompilerContext &context);
void CompileAddObjectNode(CompilerContext &context);
void CompileInventoryNode(CompilerContext &context);
void CompileDropItemNode(CompilerContext &context);
void CompileSetParentNode(CompilerContext &context);
void CompileApplyImpulseNode(CompilerContext &context);
void CompileApplyForceToTargetNode(CompilerContext &context);
void CompileSetGamePropertyIntNode(CompilerContext &context);
void CompileSetGamePropertyFloatNode(CompilerContext &context);
void CompileSetGamePropertyBoolNode(CompilerContext &context);
void CompileSetGamePropertyStringNode(CompilerContext &context);
void CompileSetTextNode(CompilerContext &context);
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
void CompileFlowGateNode(CompilerContext &context);
void CompileAnyFlowNode(CompilerContext &context);
void CompileSequenceNode(CompilerContext &context);
void CompileCooldownNode(CompilerContext &context);
void CompileOnNextFrameNode(CompilerContext &context);
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
void CompileSetShapeKeyValueNode(CompilerContext &context);
void CompileStopAllSoundsNode(CompilerContext &context);
void CompilePlaySoundNode(CompilerContext &context);
void CompileSetVolumeNode(CompilerContext &context);
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
void CompileSetSaveExistenceNode(CompilerContext &context);
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
void CompileGamePropertyReadNode(CompilerContext &context);
void CompileEvaluatePropertyNode(CompilerContext &context);
void CompileGetGamePropertyNode(CompilerContext &context);
void CompileSetGamePropertyNode(CompilerContext &context);
void CompileTypecastNode(CompilerContext &context);
void CompileValueValidNode(CompilerContext &context);
void CompileMixValueNode(CompilerContext &context);
void CompileFloatCurveNode(CompilerContext &context);
void CompileValueSwitchListNode(CompilerContext &context);
void CompileValueSwitchListCompareNode(CompilerContext &context);
void CompileListRandomItemNode(CompilerContext &context);
void CompileGetChildByNameNode(CompilerContext &context);
void CompileListSavedVariablesNode(CompilerContext &context);
void CompileClearVariablesNode(CompilerContext &context);
void CompileRemoveVariableNode(CompilerContext &context);
void CompileSetOverlayCollectionNode(CompilerContext &context);
void CompileRemoveOverlayCollectionNode(CompilerContext &context);
void CompileObjectScrollViewNode(CompilerContext &context);
void CompileObjectGridViewNode(CompilerContext &context);
void CompileScrollObjectViewNode(CompilerContext &context);
void CompileShowMessageNode(CompilerContext &context);
void CompileInputActionNode(CompilerContext &context);
void CompileKeyboardKeyNode(CompilerContext &context);
void CompileMouseButtonNode(CompilerContext &context);
void CompileKeyCodeNode(CompilerContext &context);
void CompileGetTreePropertyNode(CompilerContext &context);
void CompileGetInputBindingNode(CompilerContext &context);
void CompileRebindInputActionNode(CompilerContext &context);
void CompileInputMapNode(CompilerContext &context);
void CompileOnUIEventNode(CompilerContext &context);
void CompileOnUISoundNode(CompilerContext &context);
void CompileSetUIInteractableNode(CompilerContext &context);
void CompileOpenObjectMenuNode(CompilerContext &context);
void CompileCloseObjectMenuNode(CompilerContext &context);
void CompileSetUIFocusNode(CompilerContext &context);
void CompileGetUIFocusNode(CompilerContext &context);
void CompileScreenAnchorNode(CompilerContext &context);
void CompileRandomFloatNode(CompilerContext &context);
void CompileRandomIntNode(CompilerContext &context);
void CompileRandomVectorNode(CompilerContext &context);
void CompileFormulaNode(CompilerContext &context);
void CompileTweenValueNode(CompilerContext &context);
void CompileAddPhysicsConstraintNode(CompilerContext &context);
void CompileGetRigidBodyConstraintsNode(CompilerContext &context);
void CompileRemovePhysicsConstraintNode(CompilerContext &context);
void CompileProjectileRayNode(CompilerContext &context);
FlowPath GuardFlowPath(LN_Program &program, const FlowPath &path, uint32_t guard_expr);

static LN_CurveTable IdentityCurveTable()
{
  LN_CurveTable table;
  /* Invalid editor storage must fail open: preserve the input instead of unexpectedly clamping
   * it to the default CurveMapping display range. Valid mappings retain their authored clip flag. */
  table.clip_output = false;
  for (int sample_index = 0; sample_index < LN_CURVE_SAMPLE_COUNT; sample_index++) {
    table.samples[size_t(sample_index)] = float(sample_index) /
                                          float(LN_CURVE_SAMPLE_COUNT - 1);
  }
  return table;
}

static bool CurveMappingIsSafeToBake(const blender::CurveMapping &mapping)
{
  const blender::CurveMap &curve = mapping.cm[0];
  if (curve.totpoint < 2 || curve.curve == nullptr) {
    return false;
  }
  for (int point_index = 0; point_index < curve.totpoint; point_index++) {
    if (!std::isfinite(curve.curve[point_index].x) ||
        !std::isfinite(curve.curve[point_index].y))
    {
      return false;
    }
  }
  return true;
}

static uint32_t BakeCurveTable(CompilerContext &context)
{
  LN_CurveTable table = IdentityCurveTable();
  blender::CurveMapping *mapping = static_cast<blender::CurveMapping *>(context.node.storage);
  if (mapping == nullptr || !CurveMappingIsSafeToBake(*mapping)) {
    return context.program.AddCurveTable(table);
  }

  blender::BKE_curvemapping_init(mapping);
  const blender::CurveMap &curve = mapping->cm[0];
  const float table_span = curve.maxtable - curve.mintable;
  if (curve.table == nullptr || !std::isfinite(curve.mintable) ||
      !std::isfinite(curve.maxtable) || !std::isfinite(table_span) || table_span <= 0.0f ||
      !std::isfinite(curve.range) || curve.range <= 0.0f ||
      !std::isfinite(curve.table[0].x) ||
      !std::isfinite(curve.table[LN_CURVE_SAMPLE_COUNT - 1].x) ||
      curve.table[0].x > curve.table[LN_CURVE_SAMPLE_COUNT - 1].x ||
      !std::isfinite(curve.ext_in[0]) || !std::isfinite(curve.ext_in[1]) ||
      !std::isfinite(curve.ext_out[0]) || !std::isfinite(curve.ext_out[1]))
  {
    return context.program.AddCurveTable(table);
  }

  for (int sample_index = 0; sample_index < LN_CURVE_SAMPLE_COUNT; sample_index++) {
    const float sample = curve.table[sample_index].y;
    if (!std::isfinite(sample)) {
      return context.program.AddCurveTable(table);
    }
    table.samples[size_t(sample_index)] = sample;
  }

  table.domain_min = curve.mintable;
  table.domain_max = curve.maxtable;
  table.sample_scale = curve.range;
  table.table_min_x = curve.table[0].x;
  table.table_max_x = curve.table[LN_CURVE_SAMPLE_COUNT - 1].x;
  table.extend_in_x = curve.ext_in[0];
  table.extend_in_y = curve.ext_in[1];
  table.extend_out_x = curve.ext_out[0];
  table.extend_out_y = curve.ext_out[1];
  table.extrapolate = (mapping->flag & blender::CUMA_EXTEND_EXTRAPOLATE) != 0;
  table.clip_output = (mapping->flag & blender::CUMA_DO_CLIP) != 0;
  if (table.clip_output && std::isfinite(mapping->clipr.ymin) &&
      std::isfinite(mapping->clipr.ymax) && mapping->clipr.ymin <= mapping->clipr.ymax)
  {
    table.clip_min = mapping->clipr.ymin;
    table.clip_max = mapping->clipr.ymax;
  }
  else if (table.clip_output) {
    return context.program.AddCurveTable(IdentityCurveTable());
  }
  return context.program.AddCurveTable(table);
}

static std::string FormulaTextFromNode(const blender::bNode &node)
{
  static const char *predefined[] = {
      "a + b",   "abs(a)",     "acos(a)",  "acosh(a)", "asin(a)",  "asinh(a)",
      "atan(a)", "atan2(a,b)", "atanh(a)", "ceil(a)",  "cos(a)",   "cosh(a)",
      "curt(a)", "degrees(a)", "e",        "exp(a)",   "floor(a)", "hypot(a,b)",
      "log(a)",  "log10(a)",   "mod(a,b)", "pi",       "pow(a,b)", "radians(a)",
      "sign(a)", "sin(a)",     "sinh(a)",  "sqrt(a)",  "tan(a)",   "tanh(a)",
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Timescale flow must be driven by an event or branch");
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

static FlowPath AppendPrecompiledRoute(CompilerContext &context,
                                       const FlowPath &source_path,
                                       uint32_t condition_expr,
                                       const char *output_name,
                                       uint32_t semantic_order);

/** The Print option of List Global Properties and List Saved Variables: prints `header` and the
 * list when Flow arrives (not each time the list is read, which may be every step or never). */
static void AppendListPrintOnFlow(CompilerContext &context,
                                  const uint32_t header_expr,
                                  const uint32_t list_expr)
{
  if (!InputSocketIdentifierIsLinked(context.node, "Flow", context.input_links)) {
    return;
  }
  const std::optional<uint32_t> print_expr = context.BuildBool("Print");
  if (!print_expr) {
    return;
  }
  const LN_BoolExpression &print = context.program.GetBoolExpressions()[*print_expr];
  if (print.kind == LN_BoolExpressionKind::Constant && !print.bool_value) {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  LN_StringExpression list_text;
  list_text.kind = LN_StringExpressionKind::FromGenericValue;
  list_text.value_expr_index = list_expr;
  LN_StringExpression text;
  text.kind = LN_StringExpressionKind::Join;
  text.input0 = header_expr;
  text.input1 = context.program.AddStringExpression(list_text);
  LN_ValueExpression message;
  message.kind = LN_ValueExpressionKind::FromString;
  message.input0 = context.program.AddStringExpression(text);
  const uint32_t message_expr = context.program.AddValueExpression(message);
  for (const FlowPath &path : flow_events->paths) {
    const FlowPath print_path = AppendPrecompiledRoute(context, path, *print_expr, "Print", 0);
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Print;
    instruction.source_ref_index = context.MakeSourceRef("Print");
    instruction.value_expr_index = message_expr;
    AppendFlowInstruction(context.program, print_path, instruction);
  }
}

/** "<title> [<name>]: " for AppendListPrintOnFlow. */
static uint32_t ListPrintHeader(CompilerContext &context,
                                const char *title,
                                const std::optional<uint32_t> name_expr)
{
  LN_StringExpression with_name;
  with_name.kind = LN_StringExpressionKind::Join;
  with_name.input0 = AddConstantStringExpression(context.program, std::string(title) + " [");
  with_name.input1 = name_expr.value_or(AddConstantStringExpression(context.program, ""));
  LN_StringExpression header;
  header.kind = LN_StringExpressionKind::Join;
  header.input0 = context.program.AddStringExpression(with_name);
  header.input1 = AddConstantStringExpression(context.program, "]: ");
  return context.program.AddStringExpression(header);
}

void CompileDataContainerNode(CompilerContext &context)
{
  switch (context.definition.kind) {
    case LN_NodeKind::FindObject: {
      /* No object picked: no object, so a node it feeds does nothing (instead of reading its
       * Object input as empty, which means the owner). */
      const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
      context.CacheValueOutputPin(
          "Object", object_expr ? *object_expr :
                                  AddConstantValueExpression(context.program, MakeNoneValue()));
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
      if (const std::optional<uint32_t> default_expr = context.BuildValueExpression(
              "Default Value"))
      {
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
      const std::optional<uint32_t> category_expr = context.BuildString("Category");
      if (category_expr) {
        expression.input0 = *category_expr;
      }
      const uint32_t value_expr = context.program.AddValueExpression(expression);
      context.CacheValueOutputPin("Value", value_expr);
      AppendListPrintOnFlow(
          context, ListPrintHeader(context, "Global Properties", category_expr), value_expr);
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
      if (const std::optional<uint32_t> default_expr = context.BuildValueExpression(
              "Default Value"))
      {
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
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression(
              "Collection"))
      {
        context.CacheValueOutputPin("Collection", *collection_expr);
      }
      break;
    }
    case LN_NodeKind::GetCollectionObjects: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::CollectionObjects;
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression(
              "Collection"))
      {
        expression.input0 = *collection_expr;
      }
      const std::optional<uint32_t> include_children = context.BuildBool(
          "Include Child Collections");
      if (!include_children && context.InputIsLinked("Include Child Collections")) {
        context.AddError("Include Child Collections",
                         "Get Collection Objects Include Child Collections input link did not "
                         "compile");
      }
      expression.bool_expr_index = include_children.value_or(LN_INVALID_INDEX);
      context.CacheValueOutputPin("Objects", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetCollectionObjectNames: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::CollectionObjectNames;
      if (const std::optional<uint32_t> collection_expr = context.BuildValueExpression(
              "Collection"))
      {
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
      context.CacheValueOutputPin("Dictionary",
                                  context.program.AddValueExpression(dict_expression));

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
      const int active_socket_index = std::clamp<int>(
          context.node.custom1, 0, int(std::size(socket_names)) - 1);
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
      /* Only linked items: an empty slot holds no value (it is no None item). */
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::MakeList;
      const auto linked_item = [&](const char *socket) -> std::optional<uint32_t> {
        return InputSocketIdentifierIsLinked(context.node, socket, context.input_links) ?
                   context.BuildValueExpression(socket) :
                   std::nullopt;
      };
      if (const std::optional<uint32_t> item_a = linked_item("Item A")) {
        expression.input0 = *item_a;
      }
      if (const std::optional<uint32_t> item_b = linked_item("Item B")) {
        expression.input1 = *item_b;
      }
      if (const std::optional<uint32_t> item_c = linked_item("Item C")) {
        expression.input2 = *item_c;
      }
      context.CacheValueOutputPin("List", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ListSort:
    case LN_NodeKind::ListFindIndex: {
      const bool sort = context.definition.kind == LN_NodeKind::ListSort;
      LN_ValueExpression expression;
      expression.kind = sort ? LN_ValueExpressionKind::ListSort :
                               LN_ValueExpressionKind::ListFindIndex;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("List")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> key_expr = context.BuildValueExpression(sort ? "Key" :
                                                                                       "Value"))
      {
        expression.input1 = *key_expr;
      }
      if (const std::optional<uint32_t> descending_expr = sort ? context.BuildValueExpression(
                                                                     "Descending") :
                                                                 std::nullopt)
      {
        expression.input2 = *descending_expr;
      }
      context.CacheValueOutputPin(sort ? "List" : "Index",
                                  context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::DictAddValue: {
      const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
      if (done_expr) {
        context.CacheBoolOutputPin("Done", *done_expr);
      }
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::DictAddValue;
      if (const std::optional<uint32_t> dict_expr = context.BuildValueExpression("Dictionary")) {
        expression.input0 = *dict_expr;
      }
      if (const std::optional<uint32_t> key_expr = context.BuildString("Key")) {
        expression.input1 = *key_expr;
      }
      if (const std::optional<uint32_t> amount_expr = context.BuildValueExpression("Amount")) {
        expression.input2 = *amount_expr;
      }
      context.CacheValueOutputPin("Dictionary", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::ItemCount: {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::InventoryItemCount;
      if (const std::optional<uint32_t> list_expr = context.BuildValueExpression("Inventory")) {
        expression.input0 = *list_expr;
      }
      if (const std::optional<uint32_t> item_expr = context.BuildValueExpression("Item")) {
        expression.input1 = *item_expr;
      }
      context.CacheValueOutputPin("Count", context.program.AddValueExpression(expression));
      break;
    }
    case LN_NodeKind::GetItemInfo: {
      static const std::pair<const char *, LN_ItemInfoField> outputs[] = {
          {"Found", LN_ItemInfoField::Found},
          {"Object", LN_ItemInfoField::Object},
          {"Name", LN_ItemInfoField::Name},
          {"Max Stack", LN_ItemInfoField::MaxStack},
          {"Category", LN_ItemInfoField::Category},
          {"Description", LN_ItemInfoField::Description}};
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ItemLibraryInfo;
      if (const std::optional<uint32_t> library_expr = context.BuildValueExpression("Library")) {
        expression.input0 = *library_expr;
      }
      if (const std::optional<uint32_t> item_expr = context.BuildValueExpression("Item")) {
        expression.input1 = *item_expr;
      }
      for (const auto &[pin_name, field] : outputs) {
        if (FindOutputSocket(context.node, pin_name) != nullptr) {
          expression.value = MakeDefaultValue(LN_ValueType::Int);
          expression.value.int_value = int32_t(field);
          context.CacheValueOutputPin(pin_name, context.program.AddValueExpression(expression));
        }
      }
      break;
    }
    case LN_NodeKind::GetRecipeInfo: {
      static const std::pair<const char *, LN_RecipeInfoField> outputs[] = {
          {"Valid", LN_RecipeInfoField::Valid},
          {"Result", LN_RecipeInfoField::Result},
          {"Count", LN_RecipeInfoField::Count},
          {"Ingredients", LN_RecipeInfoField::Ingredients},
          {"Station", LN_RecipeInfoField::Station},
          {"Can Craft", LN_RecipeInfoField::CanCraft},
          {"Results", LN_RecipeInfoField::Results},
          {"Craftable", LN_RecipeInfoField::Craftable}};
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::RecipeInfo;
      expression.input0 = context.BuildValueExpression("Recipe").value_or(LN_INVALID_INDEX);
      expression.input1 = context.BuildValueExpression("Inventory").value_or(LN_INVALID_INDEX);
      for (const auto &[pin_name, field] : outputs) {
        expression.value = MakeDefaultValue(LN_ValueType::Int);
        expression.value.int_value = int32_t(field);
        context.CacheValueOutputPin(pin_name, context.program.AddValueExpression(expression));
      }
      break;
    }
    case LN_NodeKind::FindRecipe: {
      static const std::pair<const char *, LN_RecipeFindField> outputs[] = {
          {"Found", LN_RecipeFindField::Found},
          {"Recipe", LN_RecipeFindField::Recipe},
          {"Index", LN_RecipeFindField::Index}};
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::RecipeFind;
      expression.input0 = context.BuildValueExpression("Recipes").value_or(LN_INVALID_INDEX);
      expression.input1 = context.BuildValueExpression("Items").value_or(LN_INVALID_INDEX);
      expression.input2 = context.BuildValueExpression("Station").value_or(LN_INVALID_INDEX);
      for (const auto &[pin_name, field] : outputs) {
        expression.value = MakeDefaultValue(LN_ValueType::Int);
        expression.value.int_value = int32_t(field);
        context.CacheValueOutputPin(pin_name, context.program.AddValueExpression(expression));
      }
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
      context, "Property", "Name", "Toggle Tree Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Toggle Tree Property flow must be driven by an event or branch");
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

void CompileGamePropertyReadNode(CompilerContext &context)
{
  const bool is_has_property = context.definition.kind == LN_NodeKind::HasProperty;
  if (is_has_property) {
    /* Game property names are never empty, so a blank name is a constant False instead of an
     * error that would disable the whole tree. */
    const std::optional<LN_Value> name = context.ReadValue("Name", LN_ValueType::String);
    if (name && name->type == LN_ValueType::String && name->string_value.empty()) {
      context.AddWarning("Name", "Has Property has no property name and is always False");
      context.CacheBoolOutputPin("If True", AddConstantBoolExpression(context.program, false));
      return;
    }
  }
  const std::optional<std::string> property_name =
      is_has_property ?
          ReadRequiredConstantString(
              context, "Name", "Has Property requires a constant property name") :
          ReadRequiredConstantStringWithFallback(context,
                                                 "Property",
                                                 "Name",
                                                 "Get Game Property requires a constant property name");
  if (!property_name) {
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  if (!object_expr) {
    LN_ValueType property_type = LN_ValueType::None;
    switch (context.definition.kind) {
      case LN_NodeKind::GetGamePropertyBool:
        property_type = LN_ValueType::Bool;
        break;
      case LN_NodeKind::GetGamePropertyInt:
        property_type = LN_ValueType::Int;
        break;
      case LN_NodeKind::GetGamePropertyFloat:
        property_type = LN_ValueType::Float;
        break;
      case LN_NodeKind::GetGamePropertyString:
        property_type = LN_ValueType::String;
        break;
      default:
        break;
    }
    const uint32_t property_ref_index = AddGamePropertyRef(
        context.program, *property_name, property_type, MakeDefaultValue(property_type));
    switch (context.definition.kind) {
      case LN_NodeKind::HasProperty: {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::SnapshotGamePropertyExists;
        expression.property_ref_index = property_ref_index;
        context.CacheBoolOutputPin("If True", context.program.AddBoolExpression(expression));
        return;
      }
      case LN_NodeKind::GetGamePropertyBool: {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        context.CacheBoolOutputPin("Value", context.program.AddBoolExpression(expression));
        return;
      }
      case LN_NodeKind::GetGamePropertyInt: {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        context.CacheIntOutputPin("Value", context.program.AddIntExpression(expression));
        return;
      }
      case LN_NodeKind::GetGamePropertyFloat: {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        context.CacheFloatOutputPin("Value", context.program.AddFloatExpression(expression));
        return;
      }
      case LN_NodeKind::GetGamePropertyString: {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        context.CacheStringOutputPin("Value", context.program.AddStringExpression(expression));
        return;
      }
      default:
        return;
    }
  }

  LN_ValueExpression property;
  property.kind = LN_ValueExpressionKind::ObjectGameProperty;
  property.input0 = *object_expr;
  property.input1 = AddConstantStringExpression(context.program, *property_name);
  const uint32_t property_expr = context.program.AddValueExpression(property);

  switch (context.definition.kind) {
    case LN_NodeKind::HasProperty: {
      LN_BoolExpression is_none;
      is_none.kind = LN_BoolExpressionKind::ValueIsNone;
      is_none.input0 = property_expr;
      LN_BoolExpression exists;
      exists.kind = LN_BoolExpressionKind::Not;
      exists.input0 = context.program.AddBoolExpression(is_none);
      context.CacheBoolOutputPin("If True", context.program.AddBoolExpression(exists));
      return;
    }
    case LN_NodeKind::GetGamePropertyBool: {
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::FromGenericValue;
      expression.input0 = property_expr;
      context.CacheBoolOutputPin("Value", context.program.AddBoolExpression(expression));
      return;
    }
    case LN_NodeKind::GetGamePropertyInt: {
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::FromGenericValue;
      expression.input0 = property_expr;
      context.CacheIntOutputPin("Value", context.program.AddIntExpression(expression));
      return;
    }
    case LN_NodeKind::GetGamePropertyFloat: {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::FromGenericValue;
      expression.input0 = property_expr;
      context.CacheFloatOutputPin("Value", context.program.AddFloatExpression(expression));
      return;
    }
    case LN_NodeKind::GetGamePropertyString: {
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind::FromGenericValue;
      expression.value_expr_index = property_expr;
      context.CacheStringOutputPin("Value", context.program.AddStringExpression(expression));
      return;
    }
    default:
      return;
  }
}

/** Get Object Property: a live read of any type, such as a List, Table or Object. */
void CompileGetGamePropertyNode(CompilerContext &context)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ObjectGameProperty;
  if (const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object")) {
    expression.input0 = *object_expr;
  }
  const std::optional<uint32_t> name_expr = context.BuildString("Property");
  if (!name_expr) {
    context.AddError("Property", "Get Object Property requires a property name");
    return;
  }
  expression.input1 = *name_expr;
  context.CacheValueOutputPin("Value", context.program.AddValueExpression(expression));
}

/** Set Object Property: any value; the target goes in `value_expr_index`, the value next. */
void CompileSetGamePropertyNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context, "Property", "Name", "Set Object Property requires a constant property name");
  if (!name) {
    return;
  }
  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Object Property requires a value input");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Object Property must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, *name, LN_ValueType::Generic, MakeDefaultValue(LN_ValueType::Generic));
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef("Value");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Generic;
    instruction.secondary_value_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileEvaluatePropertyNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<std::string> property_name = ReadRequiredConstantStringWithFallback(
      context, "Property", "Name", "Evaluate Object Property requires a constant property name");
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
  constexpr int target_integer = 0;
  constexpr int target_boolean = 1;
  constexpr int target_string = 2;
  constexpr int target_float = 3;
  constexpr int target_vector = 4;
  const int target = std::clamp<int>(context.node.custom1, target_integer, target_vector);
  const LN_ValueType source_type = context.LinkedInputValueType("Value");

  auto unsupported = [&](const char *target_name) {
    context.AddError("Value",
                     "Typecast Value cannot convert " +
                         std::string(ValueTypeDiagnosticName(source_type)) + " to " + target_name +
                         "; use an explicit component or semantic conversion node");
  };
  auto failed = [&]() {
    context.AddError("Value", "Typecast Value could not compile its linked input");
  };
  auto build_generic = [&]() {
    const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
    if (!value_expr) {
      failed();
    }
    return value_expr;
  };
  auto float_to_int_mode = [&]() {
    switch (context.node.custom2) {
      case 1:
        return LN_FloatToIntMode::Round;
      case 2:
        return LN_FloatToIntMode::Floor;
      case 3:
        return LN_FloatToIntMode::Ceil;
      default:
        return LN_FloatToIntMode::Truncate;
    }
  };

  if (target == target_integer) {
    if (source_type == LN_ValueType::Int) {
      if (const std::optional<uint32_t> source = context.BuildLinkedInt("Value")) {
        context.CacheIntOutputPin("Integer", *source);
      }
      else {
        failed();
      }
      return;
    }

    LN_IntExpression expression;
    if (source_type == LN_ValueType::Bool) {
      const std::optional<uint32_t> source = context.BuildLinkedBool("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_IntExpressionKind::FromBool;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Float) {
      const std::optional<uint32_t> source = context.BuildLinkedFloat("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_IntExpressionKind::FromFloat;
      expression.input0 = *source;
      expression.float_to_int_mode = float_to_int_mode();
    }
    else if (source_type == LN_ValueType::String) {
      const std::optional<uint32_t> source = context.BuildLinkedString("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_IntExpressionKind::FromString;
      expression.input0 = *source;
      expression.float_to_int_mode = float_to_int_mode();
    }
    else if (source_type == LN_ValueType::Generic) {
      const std::optional<uint32_t> source = build_generic();
      if (!source) {
        return;
      }
      expression.kind = LN_IntExpressionKind::FromGenericValue;
      expression.input0 = *source;
      expression.float_to_int_mode = float_to_int_mode();
    }
    else {
      unsupported("integer");
      return;
    }
    context.CacheIntOutputPin("Integer", context.program.AddIntExpression(expression));
    return;
  }

  if (target == target_boolean) {
    if (source_type == LN_ValueType::Bool) {
      if (const std::optional<uint32_t> source = context.BuildLinkedBool("Value")) {
        context.CacheBoolOutputPin("Boolean", *source);
      }
      else {
        failed();
      }
      return;
    }

    LN_BoolExpression expression;
    if (source_type == LN_ValueType::Int) {
      const std::optional<uint32_t> source = context.BuildLinkedInt("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromInt;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Float) {
      const std::optional<uint32_t> source = context.BuildLinkedFloat("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromFloat;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::String) {
      const std::optional<uint32_t> source = context.BuildLinkedString("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromString;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Vector || source_type == LN_ValueType::Rotation) {
      const std::optional<uint32_t> source = context.BuildLinkedVector("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromVector;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Color) {
      const std::optional<uint32_t> source = context.BuildLinkedColor("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromColor;
      expression.input0 = *source;
    }
    else {
      const std::optional<uint32_t> source = build_generic();
      if (!source) {
        return;
      }
      expression.kind = LN_BoolExpressionKind::FromGenericValue;
      expression.input0 = *source;
    }
    context.CacheBoolOutputPin("Boolean", context.program.AddBoolExpression(expression));
    return;
  }

  if (target == target_string) {
    if (source_type == LN_ValueType::String) {
      if (const std::optional<uint32_t> source = context.BuildLinkedString("Value")) {
        context.CacheStringOutputPin("String", *source);
      }
      else {
        failed();
      }
      return;
    }

    LN_StringExpression expression;
    if (source_type == LN_ValueType::Bool) {
      const std::optional<uint32_t> source = context.BuildLinkedBool("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_StringExpressionKind::FromBool;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Int) {
      const std::optional<uint32_t> source = context.BuildLinkedInt("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_StringExpressionKind::FromInt;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Float) {
      const std::optional<uint32_t> source = context.BuildLinkedFloat("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_StringExpressionKind::FromFloat;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Vector || source_type == LN_ValueType::Rotation) {
      const std::optional<uint32_t> source = context.BuildLinkedVector("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_StringExpressionKind::FromVector;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::Color) {
      const std::optional<uint32_t> source = context.BuildLinkedColor("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_StringExpressionKind::FromColor;
      expression.input0 = *source;
    }
    else {
      const std::optional<uint32_t> source = build_generic();
      if (!source) {
        return;
      }
      expression.kind = LN_StringExpressionKind::FromGenericValue;
      expression.value_expr_index = *source;
    }
    context.CacheStringOutputPin("String", context.program.AddStringExpression(expression));
    return;
  }

  if (target == target_float) {
    if (source_type == LN_ValueType::Float) {
      if (const std::optional<uint32_t> source = context.BuildLinkedFloat("Value")) {
        context.CacheFloatOutputPin("Float", *source);
      }
      else {
        failed();
      }
      return;
    }

    LN_FloatExpression expression;
    if (source_type == LN_ValueType::Bool) {
      const std::optional<uint32_t> source = context.BuildLinkedBool("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_FloatExpressionKind::FromBool;
      expression.bool_expr_index = *source;
    }
    else if (source_type == LN_ValueType::Int) {
      const std::optional<uint32_t> source = context.BuildLinkedInt("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_FloatExpressionKind::FromInt;
      expression.input0 = *source;
    }
    else if (source_type == LN_ValueType::String) {
      const std::optional<uint32_t> source = context.BuildLinkedString("Value");
      if (!source) {
        failed();
        return;
      }
      expression.kind = LN_FloatExpressionKind::FromString;
      expression.string_expr_index = *source;
    }
    else if (source_type == LN_ValueType::Generic) {
      const std::optional<uint32_t> source = build_generic();
      if (!source) {
        return;
      }
      expression.kind = LN_FloatExpressionKind::FromGenericValue;
      expression.input0 = *source;
    }
    else {
      unsupported("float");
      return;
    }
    context.CacheFloatOutputPin("Float", context.program.AddFloatExpression(expression));
    return;
  }

  if (source_type == LN_ValueType::Vector || source_type == LN_ValueType::Rotation) {
    if (const std::optional<uint32_t> source = context.BuildLinkedVector("Value")) {
      context.CacheVectorOutputPin("Vector", *source);
    }
    else {
      failed();
    }
    return;
  }

  LN_VectorExpression expression;
  if (source_type == LN_ValueType::Float) {
    const std::optional<uint32_t> source = context.BuildLinkedFloat("Value");
    if (!source) {
      failed();
      return;
    }
    expression.kind = LN_VectorExpressionKind::BroadcastFloat;
    expression.float_expr_index = *source;
  }
  else if (source_type == LN_ValueType::Int) {
    const std::optional<uint32_t> source = context.BuildLinkedInt("Value");
    if (!source) {
      failed();
      return;
    }
    expression.kind = LN_VectorExpressionKind::BroadcastInt;
    expression.input0 = *source;
  }
  else if (source_type == LN_ValueType::Color) {
    const std::optional<uint32_t> source = context.BuildLinkedColor("Value");
    if (!source) {
      failed();
      return;
    }
    expression.kind = LN_VectorExpressionKind::FromColor;
    expression.input0 = *source;
  }
  else if (source_type == LN_ValueType::Vector4 || source_type == LN_ValueType::List ||
           source_type == LN_ValueType::Generic)
  {
    const std::optional<uint32_t> source = build_generic();
    if (!source) {
      return;
    }
    expression.kind = LN_VectorExpressionKind::FromGenericValue;
    expression.input0 = *source;
  }
  else {
    unsupported("vector");
    return;
  }
  context.CacheVectorOutputPin("Vector", context.program.AddVectorExpression(expression));
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
  context.CacheBoolOutputPin(
      "If Valid",
      AddNotBoolExpression(context.program, context.program.AddBoolExpression(none_expr)));
}

void CompileMixValueNode(CompilerContext &context)
{
  const std::optional<uint32_t> a_expr = context.BuildValueExpression("A");
  const std::optional<uint32_t> b_expr = context.BuildValueExpression("B");
  const std::optional<uint32_t> factor_expr = context.BuildFloat("Factor");
  if (!a_expr) {
    context.AddError("A", "Mix Value requires a linked A value");
  }
  if (!b_expr) {
    context.AddError("B", "Mix Value requires a linked B value");
  }
  if (!factor_expr) {
    context.AddError("Factor", "Mix Value requires a float factor");
  }
  if (!a_expr || !b_expr || !factor_expr) {
    return;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Mix;
  expression.input0 = *a_expr;
  expression.input1 = *b_expr;
  expression.input2 = *factor_expr;
  context.CacheValueOutputPin("Result", context.program.AddValueExpression(expression));
}

void CompileFloatCurveNode(CompilerContext &context)
{
  const std::optional<uint32_t> factor_expr = context.BuildFloat("Factor");
  const std::optional<uint32_t> value_expr = context.BuildFloat("Value");
  if (!factor_expr) {
    context.AddError("Factor", "Float Curve requires a float factor input");
  }
  if (!value_expr) {
    context.AddError("Value", "Float Curve requires a float value input");
  }
  if (!factor_expr || !value_expr) {
    return;
  }

  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::FloatCurve;
  expression.input0 = *value_expr;
  expression.input1 = *factor_expr;
  expression.int_value = int32_t(BakeCurveTable(context));
  context.CacheFloatOutputPin("Value", context.program.AddFloatExpression(expression));
}

void CompileValueSwitchListNode(CompilerContext &context)
{
  static const char *conditions[] = {"if A", "elif B", "elif C", "elif D", "elif E", "elif F"};
  static const char *values[] = {"Value A", "Value B", "Value C", "Value D", "Value E", "Value F"};

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ValueSwitchList;
  for (size_t index = 0; index < std::size(conditions); index++) {
    expression.input_indices.push_back(
        context.BuildBool(conditions[index]).value_or(LN_INVALID_INDEX));
    expression.input_indices.push_back(
        context.BuildValueExpression(values[index]).value_or(LN_INVALID_INDEX));
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
    expression.input_indices.push_back(
        context.BuildValueExpression(cases[index]).value_or(LN_INVALID_INDEX));
    expression.input_indices.push_back(
        context.BuildValueExpression(values[index]).value_or(LN_INVALID_INDEX));
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
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  expression.input1 = file_expr.value_or(LN_INVALID_INDEX);
  const uint32_t list_expr = context.program.AddValueExpression(expression);
  context.CacheValueOutputPin("List", list_expr);
  AppendListPrintOnFlow(context, ListPrintHeader(context, "Saved Variables", file_expr), list_expr);
  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileClearVariablesNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Clear Variables flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Remove Variable flow must be driven by an event or branch");
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

struct UICommandCompletionOutputs {
  uint32_t completed = LN_INVALID_INDEX;
  uint32_t succeeded = LN_INVALID_INDEX;
  uint32_t failed = LN_INVALID_INDEX;
};

static UICommandCompletionOutputs AddUICommandCompletionOutputs(CompilerContext &context,
                                                                const char *success_output,
                                                                const char *handle_output);
static void CachePrecompiledRoutes(CompilerContext &context,
                                   const char *output_name,
                                   const std::vector<FlowPath> &paths);

void CompileSetOverlayCollectionNode(CompilerContext &context)
{
  if (!context.InputIsLinked("Camera")) {
    const blender::bNodeSocket *camera_socket = FindInputSocket(context.node, "Camera");
    const auto *camera_default = camera_socket ?
                                     static_cast<const blender::bNodeSocketValueObject *>(
                                         camera_socket->default_value) :
                                     nullptr;
    if (camera_default != nullptr && camera_default->value != nullptr &&
        camera_default->value->type != blender::OB_CAMERA)
    {
      context.AddError("Camera", "Set Overlay Collection camera must be a Camera object");
      return;
    }
  }
  const uint32_t camera_expr = context.BuildOptionalObject("Camera").value_or(LN_INVALID_INDEX);

  if (!context.InputIsLinked("Collection")) {
    const std::optional<LN_Value> collection = context.ReadValue(
        "Collection", LN_ValueType::CollectionRef);
    if (!collection || !collection->exists || collection->type != LN_ValueType::CollectionRef) {
      context.AddError("Collection", "Set Overlay Collection requires a collection");
      return;
    }
  }
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Overlay Collection flow must be driven by an event or branch");
  if (!collection_expr || !flow_events) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Success", nullptr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetOverlayCollection;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = camera_expr;
    instruction.secondary_value_expr_index = *collection_expr;
    instruction.property_ref_index = completion.completed;
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Success")) {
    CachePrecompiledRoutes(context, "Success", ContinuationPaths(*flow_events, completion.succeeded));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flow_events, completion.failed));
  }
}

void CompileRemoveOverlayCollectionNode(CompilerContext &context)
{
  if (!context.InputIsLinked("Collection")) {
    const std::optional<LN_Value> collection = context.ReadValue(
        "Collection", LN_ValueType::CollectionRef);
    if (!collection || !collection->exists || collection->type != LN_ValueType::CollectionRef) {
      context.AddError("Collection", "Remove Overlay Collection requires a collection");
      return;
    }
  }
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Remove Overlay Collection flow must be driven by an event or branch");
  if (!collection_expr || !flow_events) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Success", nullptr);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RemoveOverlayCollection;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *collection_expr;
    instruction.property_ref_index = completion.completed;
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Success")) {
    CachePrecompiledRoutes(context, "Success", ContinuationPaths(*flow_events, completion.succeeded));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flow_events, completion.failed));
  }
}

struct ActionCompletionOutputs {
  uint32_t lifecycle = LN_INVALID_INDEX;
  uint32_t started = LN_INVALID_INDEX;
  uint32_t completed = LN_INVALID_INDEX;
  uint32_t interrupted = LN_INVALID_INDEX;
  uint32_t failed = LN_INVALID_INDEX;
  uint8_t output_mask = LN_ACTION_OUTPUT_NONE;
};

static void CachePrecompiledRoutes(CompilerContext &context,
                                   const char *output_name,
                                   const std::vector<FlowPath> &paths);

static UICommandCompletionOutputs AddUICommandCompletionOutputs(CompilerContext &context,
                                                                const char *success_output,
                                                                const char *handle_output)
{
  UICommandCompletionOutputs outputs;
  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::UICommandCompleted;
  /* Results are only queued for a graph that reads them (see CompleteUIOperation). */
  completed.bool_value = context.OutputFeedsActiveNode(success_output) ||
                         context.OutputFeedsActiveNode("Failed") ||
                         context.OutputFeedsActiveNode("Closed") ||
                         context.OutputFeedsActiveNode("Result") ||
                         (handle_output != nullptr && context.OutputFeedsActiveNode(handle_output));
  outputs.completed = context.program.AddBoolExpression(completed);

  LN_BoolExpression succeeded;
  succeeded.kind = LN_BoolExpressionKind::UICommandSucceeded;
  succeeded.input0 = outputs.completed;
  outputs.succeeded = context.program.AddBoolExpression(succeeded);
  context.CacheBoolOutputPin(success_output, outputs.succeeded);

  LN_BoolExpression failed;
  failed.kind = LN_BoolExpressionKind::UICommandFailed;
  failed.input0 = outputs.completed;
  outputs.failed = context.program.AddBoolExpression(failed);
  context.CacheBoolOutputPin("Failed", outputs.failed);

  LN_IntExpression result;
  result.kind = LN_IntExpressionKind::UICommandResult;
  result.input0 = outputs.completed;
  context.CacheIntOutputPin("Result", context.program.AddIntExpression(result));

  if (handle_output != nullptr) {
    LN_IntExpression handle;
    handle.kind = LN_IntExpressionKind::UICommandHandle;
    handle.input0 = outputs.completed;
    context.CacheIntOutputPin(handle_output, context.program.AddIntExpression(handle));
  }
  return outputs;
}

static ActionCompletionOutputs AddActionCompletionOutputs(CompilerContext &context)
{
  ActionCompletionOutputs outputs;
  struct OutputDefinition {
    const char *name;
    LN_BoolExpressionKind kind;
    uint8_t mask;
    uint32_t ActionCompletionOutputs::*member;
  };
  static constexpr OutputDefinition definitions[] = {
      {"Started", LN_BoolExpressionKind::ActionStarted, LN_ACTION_OUTPUT_STARTED,
       &ActionCompletionOutputs::started},
      {"Completed", LN_BoolExpressionKind::ActionCompleted, LN_ACTION_OUTPUT_COMPLETED,
       &ActionCompletionOutputs::completed},
      {"Interrupted", LN_BoolExpressionKind::ActionInterrupted, LN_ACTION_OUTPUT_INTERRUPTED,
       &ActionCompletionOutputs::interrupted},
      {"Failed", LN_BoolExpressionKind::ActionFailed, LN_ACTION_OUTPUT_FAILED,
       &ActionCompletionOutputs::failed},
  };

  for (const OutputDefinition &definition : definitions) {
    if (context.OutputFeedsActiveNode(definition.name)) {
      outputs.output_mask |= definition.mask;
    }
  }
  if (outputs.output_mask == LN_ACTION_OUTPUT_NONE) {
    return outputs;
  }

  LN_BoolExpression lifecycle;
  lifecycle.kind = LN_BoolExpressionKind::ActionLifecycleEvent;
  outputs.lifecycle = context.program.AddBoolExpression(lifecycle);
  for (const OutputDefinition &definition : definitions) {
    if ((outputs.output_mask & definition.mask) == 0u) {
      continue;
    }
    LN_BoolExpression event;
    event.kind = definition.kind;
    event.input0 = outputs.lifecycle;
    const uint32_t expression_index = context.program.AddBoolExpression(event);
    outputs.*(definition.member) = expression_index;
    context.CacheBoolOutputPin(definition.name, expression_index);
  }
  return outputs;
}

static bool ObjectUIQueryHasConsumedOutput(const CompilerContext &context)
{
  return std::any_of(context.definition.outputs.begin(),
                     context.definition.outputs.end(),
                     [&](const LN_PinDefinition &pin) {
                       return context.OutputFeedsActiveNode(pin.name.c_str());
                     });
}

static void CacheUIQueryBool(CompilerContext &context,
                             const uint32_t query_index,
                             const char *name,
                             const LN_BoolExpressionKind kind)
{
  if (!context.OutputFeedsActiveNode(name)) {
    return;
  }
  LN_BoolExpression expression;
  expression.kind = kind;
  expression.input0 = query_index;
  context.CacheBoolOutputPin(name, context.program.AddBoolExpression(expression));
}

static void CacheUIQueryVector(CompilerContext &context,
                               const uint32_t query_index,
                               const char *name,
                               const LN_VectorExpressionKind kind)
{
  if (!context.OutputFeedsActiveNode(name)) {
    return;
  }
  LN_VectorExpression expression;
  expression.kind = kind;
  expression.input0 = query_index;
  context.CacheVectorOutputPin(name, context.program.AddVectorExpression(expression));
}

static void CacheUIQueryInt(CompilerContext &context,
                            const uint32_t query_index,
                            const char *name,
                            const LN_IntExpressionKind kind)
{
  if (!context.OutputFeedsActiveNode(name)) {
    return;
  }
  LN_IntExpression expression;
  expression.kind = kind;
  expression.input0 = query_index;
  context.CacheIntOutputPin(name, context.program.AddIntExpression(expression));
}

void CompileObjectScrollViewNode(CompilerContext &context)
{
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::ObjectScrollView;
  query.input0 = context.BuildOptionalObject("Viewport Object").value_or(LN_INVALID_INDEX);
  query.input1 = context.BuildOptionalObject("Content Object").value_or(LN_INVALID_INDEX);
  query.input2 = context.BuildOptionalObject("Camera").value_or(LN_INVALID_INDEX);
  query.input3 = context.BuildVector("Minimum Offset").value_or(LN_INVALID_INDEX);
  query.input4 = context.BuildVector("Maximum Offset").value_or(LN_INVALID_INDEX);
  query.input5 = context.BuildVector("Wheel Step").value_or(LN_INVALID_INDEX);
  query.bool_expr_index = context.BuildBool("Enabled").value_or(LN_INVALID_INDEX);
  query.secondary_bool_expr_index = context.BuildBool("Horizontal").value_or(LN_INVALID_INDEX);
  query.tertiary_bool_expr_index = context.BuildBool("Vertical").value_or(LN_INVALID_INDEX);
  query.quaternary_bool_expr_index = context.BuildBool("Cull Outside").value_or(
      LN_INVALID_INDEX);
  query.quinary_bool_expr_index = context.BuildBool("Focus Follow").value_or(LN_INVALID_INDEX);
  query.int_expr_index = context.BuildInt("User").value_or(LN_INVALID_INDEX);
  query.secondary_int_expr_index = context.BuildInt("Priority").value_or(LN_INVALID_INDEX);
  query.float_expr_index = context.BuildFloat("Padding").value_or(LN_INVALID_INDEX);
  query.input9 = context.BuildOptionalObject("Scrollbar").value_or(LN_INVALID_INDEX);
  query.interaction_view = static_cast<LN_InteractionView>(std::clamp(
      int(context.node.custom1),
      int(LN_InteractionView::Automatic),
      int(LN_InteractionView::CustomCamera)));
  query.cache_key = context.node.identifier;
  const uint32_t query_index = context.program.AddQueryExpression(query);

  CacheUIQueryBool(context, query_index, "On Scroll", LN_BoolExpressionKind::UIScrolled);
  CacheUIQueryBool(context, query_index, "At Start", LN_BoolExpressionKind::UIScrollAtStart);
  CacheUIQueryBool(context, query_index, "At End", LN_BoolExpressionKind::UIScrollAtEnd);
  CacheUIQueryVector(context, query_index, "Offset", LN_VectorExpressionKind::UIScrollOffset);
  CacheUIQueryVector(
      context, query_index, "Normalized", LN_VectorExpressionKind::UIScrollNormalized);
}

void CompileObjectGridViewNode(CompilerContext &context)
{
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::ObjectGridView;
  query.input0 = context.BuildOptionalObject("Viewport Object").value_or(LN_INVALID_INDEX);
  query.input1 = context.BuildOptionalObject("Camera").value_or(LN_INVALID_INDEX);
  query.input2 = context.BuildValueExpression("Slots").value_or(LN_INVALID_INDEX);
  query.input3 = context.BuildValueExpression("Items").value_or(LN_INVALID_INDEX);
  query.input4 = context.BuildInt("First Item").value_or(LN_INVALID_INDEX);
  query.input5 = context.BuildInt("Wheel Rows").value_or(LN_INVALID_INDEX);
  query.bool_expr_index = context.BuildBool("Enabled").value_or(LN_INVALID_INDEX);
  query.secondary_bool_expr_index = context.BuildBool("Cull Outside").value_or(
      LN_INVALID_INDEX);
  query.tertiary_bool_expr_index = context.BuildBool("Focus Follow").value_or(LN_INVALID_INDEX);
  query.int_expr_index = context.BuildInt("User").value_or(LN_INVALID_INDEX);
  query.secondary_int_expr_index = context.BuildInt("Priority").value_or(LN_INVALID_INDEX);
  query.tertiary_int_expr_index = context.BuildInt("Columns").value_or(LN_INVALID_INDEX);
  query.float_expr_index = context.BuildFloat("Padding").value_or(LN_INVALID_INDEX);
  /* input6 is unused: the icons come from the list's Inventory Rules (else the scene's Item
   * Library). */
  query.quaternary_bool_expr_index = context.BuildBool("Show Counts").value_or(
      LN_INVALID_INDEX);
  query.quinary_bool_expr_index = context.BuildBool("Show Names").value_or(LN_INVALID_INDEX);
  query.senary_bool_expr_index = context.BuildBool("Show Empty").value_or(LN_INVALID_INDEX);
  query.input7 = context.BuildValueExpression("Slot Template").value_or(LN_INVALID_INDEX);
  query.quaternary_int_expr_index = context.BuildInt("Rows").value_or(LN_INVALID_INDEX);
  query.input8 = context.BuildVector("Spacing").value_or(LN_INVALID_INDEX);
  query.input9 = context.BuildOptionalObject("Scrollbar").value_or(LN_INVALID_INDEX);
  /* The list by name, and what the grid does with it (Inventory, Drop Hints). */
  query.input_indices.assign(LN_GRID_INPUT_COUNT, LN_INVALID_INDEX);
  std::vector<uint32_t> &inputs = query.input_indices;
  inputs[LN_GRID_INPUT_OBJECT] = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_PROPERTY] = context.BuildString("Property").value_or(LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_HANDLE_MOVES] = context.BuildBool("Handle Moves").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_SPLIT_ACTION] = context.BuildString("Split Action").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_SPLIT_AMOUNT] = context.BuildInt("Split Amount").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_SWAP] = context.BuildBool("Swap").value_or(LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_DROP_INTO_WORLD] = context.BuildBool("Drop Into World").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_DROP_AT_POINTER] = context.BuildBool("At Pointer").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_DROP_IN_FRONT] = context.BuildBool("Drop In Front").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_DISTANCE] = context.BuildFloat("Distance").value_or(LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_HOVER_HEIGHT] = context.BuildFloat("Hover Height").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_HINT_TEXT] = context.BuildOptionalObject("Hint Text").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_ACCEPT_HINT] = context.BuildString("Accept Hint").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_REFUSE_HINT] = context.BuildString("Refuse Hint").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_SHOW_CATEGORIES] = context.BuildString("Show Categories").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_SEARCH] = context.BuildString("Search").value_or(LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_USE_ACTION] = context.BuildString("Use Action").value_or(
      LN_INVALID_INDEX);
  inputs[LN_GRID_INPUT_USE_KEYS] = context.BuildString("Use Keys").value_or(LN_INVALID_INDEX);
  const std::optional<LN_Value> property = context.ReadValue("Property", LN_ValueType::String);
  const bool property_linked = context.InputIsLinked("Property");
  const bool names_list_always = !property_linked && property &&
                                 property->type == LN_ValueType::String &&
                                 !property->string_value.empty();
  const bool names_list = property_linked || names_list_always;
  /* A linked Property that is empty while the game runs gives the list to Items. */
  if (names_list_always && context.InputIsLinked("Items")) {
    context.AddWarning("Items", "Items is not used: Property names the list");
  }
  const std::optional<LN_Value> handle_moves = context.ReadValue("Handle Moves",
                                                                 LN_ValueType::Bool);
  if (!names_list && (context.InputIsLinked("Handle Moves") ||
                      (handle_moves && handle_moves->type == LN_ValueType::Bool &&
                       handle_moves->bool_value)))
  {
    context.AddWarning("Handle Moves",
                       "Inventory needs the list named by Object and Property: the grid cannot "
                       "change a list that Items gives");
  }
  query.interaction_view = static_cast<LN_InteractionView>(std::clamp(
      int(context.node.custom1),
      int(LN_InteractionView::Automatic),
      int(LN_InteractionView::CustomCamera)));
  query.grid_horizontal = context.node.custom2 == 1;
  query.cache_key = context.node.identifier;
  const uint32_t query_index = context.program.AddQueryExpression(query);

  CacheUIQueryBool(context, query_index, "On Scroll", LN_BoolExpressionKind::UIScrolled);
  CacheUIQueryBool(context, query_index, "On Rebind", LN_BoolExpressionKind::UIItemsRebound);
  CacheUIQueryBool(context, query_index, "At Start", LN_BoolExpressionKind::UIScrollAtStart);
  CacheUIQueryBool(context, query_index, "At End", LN_BoolExpressionKind::UIScrollAtEnd);
  CacheUIQueryInt(
      context, query_index, "First Visible", LN_IntExpressionKind::UIFirstVisibleIndex);
  CacheUIQueryInt(
      context, query_index, "Last Visible", LN_IntExpressionKind::UILastVisibleIndex);
  CacheUIQueryInt(context, query_index, "Page", LN_IntExpressionKind::UIViewPage);
  CacheUIQueryInt(context, query_index, "Page Count", LN_IntExpressionKind::UIViewPageCount);
  CacheUIQueryVector(
      context, query_index, "Normalized", LN_VectorExpressionKind::UIScrollNormalized);
}

void CompileScrollObjectViewNode(CompilerContext &context)
{
  /* An empty Viewport Object is the owner. */
  const uint32_t viewport = context.BuildOptionalObject("Viewport Object").value_or(
      LN_INVALID_INDEX);
  const std::optional<uint32_t> item = context.BuildValueExpression("Item");
  const std::optional<uint32_t> lines = context.BuildInt("Lines");
  if (!item || !lines) {
    context.AddError("Item", "Scroll Object View needs its Item and Lines inputs");
    return;
  }
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Scroll Object View must be driven by an event or branch");
  if (!flows) {
    return;
  }
  /* Done runs when the request was made: the Viewport Object exists. */
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ScrollObjectView;
    instruction.source_ref_index = context.MakeSourceRef("Viewport Object");
    instruction.value_expr_index = viewport;
    instruction.secondary_value_expr_index = *item;
    instruction.int_expr_index = *lines;
    instruction.int_value = std::clamp(int32_t(context.node.custom1),
                                       int32_t(LN_UIViewScrollMode::ShowItem),
                                       int32_t(LN_UIViewScrollMode::Lines));
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                         LN_BoolExpressionKind::InstructionExecuted,
                                                         path.event,
                                                         instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileShowMessageNode(CompilerContext &context)
{
  /* An empty Object is the owner. */
  const uint32_t object = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  const std::optional<uint32_t> text = context.BuildString("Text");
  const std::optional<uint32_t> seconds = context.BuildFloat("Seconds");
  const std::optional<uint32_t> lines = context.BuildInt("Lines");
  const std::optional<uint32_t> user = context.BuildInt("User");
  const std::optional<uint32_t> count = context.BuildInt("Count");
  if (!text || !seconds || !lines || !user || !count) {
    context.AddError("Text", "Show Message needs its Text, Count, Seconds, Lines and User inputs");
    return;
  }
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Show Message must be driven by an event or branch");
  if (!flows) {
    return;
  }
  /* Done runs when the message was shown. */
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ShowMessage;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = object;
    instruction.string_expr_index = *text;
    instruction.float_expr_index = *seconds;
    instruction.int_expr_index = *lines;
    instruction.secondary_int_expr_index = *user;
    instruction.tertiary_int_expr_index = *count;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                         LN_BoolExpressionKind::InstructionExecuted,
                                                         path.event,
                                                         instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

/** A key or button given by a link must be known when the tree compiles: a key that players
 * change is an Input Action's binding. */
static void RequireConstantInputName(CompilerContext &context,
                                     const char *socket_name,
                                     const char *node_name)
{
  if (!context.InputIsLinked(socket_name)) {
    return;
  }
  const std::optional<LN_Value> name = context.ReadValue(socket_name, LN_ValueType::String);
  if (!name || name->type != LN_ValueType::String || name->string_value.empty()) {
    context.AddError(socket_name,
                     std::string(node_name) +
                         " needs a key known before the game runs; Input Action takes keys "
                         "that players change");
  }
}

void CompileKeyboardKeyNode(CompilerContext &context)
{
  RequireConstantInputName(context, "Key", "Keyboard Key");
  CompileExpressionOutputsNode(context);
}

void CompileMouseButtonNode(CompilerContext &context)
{
  RequireConstantInputName(context, "Button", "Mouse Button");
  CompileExpressionOutputsNode(context);
}

void CompileKeyCodeNode(CompilerContext &context)
{
  RequireConstantInputName(context, "Key", "Key Code");
  CompileExpressionOutputsNode(context);
}

/** As Set Tree Property: a tree property is found by a name known when the tree compiles. */
void CompileGetTreePropertyNode(CompilerContext &context)
{
  if (context.InputIsLinked("Property")) {
    ReadRequiredConstantStringWithFallback(
        context, "Property", "Property", "Get Tree Property requires a constant property name");
  }
  CompileExpressionOutputsNode(context);
}

void CompileInputActionNode(CompilerContext &context)
{
  const uint32_t user = context.BuildInt("User").value_or(LN_INVALID_INDEX);
  const uint32_t action = context.BuildString("Action").value_or(LN_INVALID_INDEX);
  const bool also_ui = (context.node.custom1 & blender::LOGIC_NODE_INPUT_ALSO_UI) != 0;
  const auto cache_state = [&](const char *name, const int32_t status) {
    if (!context.OutputFeedsActiveNode(name)) {
      return;
    }
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::InputActionState;
    expression.input0 = user;
    expression.input1 = action;
    expression.secondary_int_value = status;
    expression.bool_value = also_ui;
    context.CacheBoolOutputPin(name, context.program.AddBoolExpression(expression));
  };
  /* Its Out starts routes on Logic + Physics steps, like a key event (no other root needed); in
   * the frame update when it only opens menus (see BuildPollingRouteFlowEvents). */
  if (context.OutputFeedsActiveNode("Out")) {
    context.has_event_node = true;
  }
  cache_state("Out", MouseInputStatusFromCustom(context.node.custom1));
  cache_state("Active", SCA_InputEvent::ACTIVE);
  cache_state("Pressed", SCA_InputEvent::JUSTACTIVATED);
  cache_state("Released", SCA_InputEvent::JUSTRELEASED);
  if (context.OutputFeedsActiveNode("Value")) {
    LN_FloatExpression expression;
    expression.kind = LN_FloatExpressionKind::InputActionValue;
    expression.input0 = user;
    expression.string_expr_index = action;
    expression.bool_value = also_ui;
    context.CacheFloatOutputPin("Value", context.program.AddFloatExpression(expression));
  }
  if (context.OutputFeedsActiveNode("Vector")) {
    LN_VectorExpression expression;
    expression.kind = LN_VectorExpressionKind::InputActionVector;
    expression.input0 = user;
    expression.input1 = action;
    expression.bool_value = also_ui;
    context.CacheVectorOutputPin("Vector", context.program.AddVectorExpression(expression));
  }
}

void CompileGetInputBindingNode(CompilerContext &context)
{
  if (!context.OutputFeedsActiveNode("Text")) {
    return;
  }
  const auto constant_int = [&](const int32_t value) {
    LN_IntExpression expression;
    expression.kind = LN_IntExpressionKind::Constant;
    expression.int_value = value;
    return context.program.AddIntExpression(expression);
  };
  LN_StringExpression expression;
  expression.kind = LN_StringExpressionKind::InputBindingText;
  expression.input0 = context.BuildString("Action").value_or(LN_INVALID_INDEX);
  expression.input1 = constant_int(std::clamp(int32_t(context.node.custom1), 0, 2));
  expression.input2 = constant_int(std::clamp(int32_t(context.node.custom2), 0, 3));
  context.CacheStringOutputPin("Text", context.program.AddStringExpression(expression));
}

/** Rebind Input Action and Input Map: one InputMap instruction whose result pulses Done and
 * sets Success, like Save Game. */
static void CompileInputMapInstruction(CompilerContext &context,
                                       const char *node_name,
                                       LN_Instruction instruction)
{
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      std::string(node_name) + " flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  /* One rebind waits at a time: a Loop could only start and cancel it again. */
  if (instruction.int_value == int32_t(LN_InputMapOperation::Rebind) &&
      std::any_of(flow_events->paths.begin(), flow_events->paths.end(), [](const FlowPath &path) {
        return path.loop_frame_index != LN_INVALID_INDEX;
      }))
  {
    context.AddError("Flow", "Rebind Input Action cannot run inside a Loop body");
    return;
  }
  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::PersistenceCompleted;
  const uint32_t completed_expr = context.program.AddBoolExpression(completed);
  LN_BoolExpression succeeded;
  succeeded.kind = LN_BoolExpressionKind::PersistenceSucceeded;
  succeeded.input0 = completed_expr;
  context.CacheBoolOutputPin("Success", context.program.AddBoolExpression(succeeded));

  instruction.opcode = LN_OpCode::InputMap;
  instruction.source_ref_index = context.MakeSourceRef("Flow");
  instruction.bool_expr_index = completed_expr;
  for (const FlowPath &path : flow_events->paths) {
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flow_events, completed_expr));
  }
}

void CompileRebindInputActionNode(CompilerContext &context)
{
  const std::optional<uint32_t> action_expr = context.BuildString("Action");
  const std::optional<uint32_t> user_expr = context.BuildInt("User");
  const std::optional<uint32_t> timeout_expr = context.BuildFloat("Timeout");
  if (!action_expr || !user_expr || !timeout_expr) {
    context.AddError("Action", "Rebind Input Action requires action, user, and timeout inputs");
    return;
  }
  LN_Instruction instruction;
  instruction.int_value = int32_t(LN_InputMapOperation::Rebind);
  instruction.string_expr_index = *action_expr;
  instruction.int_expr_index = *user_expr;
  instruction.float_expr_index = *timeout_expr;
  /* The device class, and the direction in the second byte. */
  instruction.secondary_int_value =
      std::clamp(int32_t(context.node.custom1),
                 int32_t(LN_InputDeviceClass::KeyboardMouse),
                 int32_t(LN_InputDeviceClass::Gamepad)) |
      (std::clamp(int32_t(context.node.custom2), 0, 3) << 8);
  CompileInputMapInstruction(context, "Rebind Input Action", instruction);
}

void CompileInputMapNode(CompilerContext &context)
{
  const std::optional<uint32_t> path_expr = context.BuildString("Path");
  const std::optional<uint32_t> file_expr = context.BuildString("File");
  if (!path_expr || !file_expr) {
    context.AddError("Path", "Input Map requires path and file inputs");
    return;
  }
  LN_Instruction instruction;
  instruction.int_value = std::clamp(int32_t(context.node.custom1),
                                     int32_t(LN_InputMapOperation::Save),
                                     int32_t(LN_InputMapOperation::Reset));
  instruction.string_expr_index = *path_expr;
  instruction.secondary_string_expr_index = *file_expr;
  CompileInputMapInstruction(context, "Input Map", instruction);
}

void CompileOnUIEventNode(CompilerContext &context)
{
  if (!ObjectUIQueryHasConsumedOutput(context)) {
    return;
  }
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::UIElementEvent;
  query.int_value = std::clamp(int(context.node.custom1),
                               int(LN_UIElementEvent::Click),
                               int(LN_UIElementEvent::ItemShown));
  query.string_expr_index = context.BuildString("Action").value_or(LN_INVALID_INDEX);
  query.cache_key = context.node.identifier;
  const uint32_t query_index = context.program.AddQueryExpression(query);

  if (context.OutputFeedsActiveNode("Out")) {
    /* An event root: its routes run in the frame update (a UI-only tree needs no other root). */
    context.has_event_node = true;
    LN_BoolExpression fired;
    fired.kind = LN_BoolExpressionKind::UIElementEventFired;
    fired.input0 = query_index;
    context.CacheBoolOutputPin("Out", context.program.AddBoolExpression(fired));
  }
  auto cache_object = [&](const char *name, const LN_ValueExpressionKind kind) {
    if (context.OutputFeedsActiveNode(name)) {
      LN_ValueExpression object;
      object.kind = kind;
      object.input0 = query_index;
      context.CacheValueOutputPin(name, context.program.AddValueExpression(object));
    }
  };
  cache_object("Object", LN_ValueExpressionKind::UIElementEventObject);
  cache_object("Target", LN_ValueExpressionKind::UIElementEventTarget);
  cache_object("Item", LN_ValueExpressionKind::UIElementEventItem);
  cache_object("Target Item", LN_ValueExpressionKind::UIElementEventTargetItem);
  CacheUIQueryInt(context, query_index, "Index", LN_IntExpressionKind::UIElementEventIndex);
  CacheUIQueryInt(context, query_index, "Amount", LN_IntExpressionKind::UIElementEventAmount);
  CacheUIQueryInt(
      context, query_index, "Target Index", LN_IntExpressionKind::UIElementEventTargetIndex);
  CacheUIQueryInt(context, query_index, "User", LN_IntExpressionKind::UIElementEventUser);
  for (const auto &[name, kind] :
       {std::pair{"Action", LN_StringExpressionKind::UIElementEventAction},
        std::pair{"Target Action", LN_StringExpressionKind::UIElementEventTargetAction},
        std::pair{"Outcome", LN_StringExpressionKind::UIElementEventOutcome}})
  {
    if (context.OutputFeedsActiveNode(name)) {
      LN_StringExpression action;
      action.kind = kind;
      action.input0 = query_index;
      context.CacheStringOutputPin(name, context.program.AddStringExpression(action));
    }
  }
  CacheUIQueryVector(context, query_index, "Point", LN_VectorExpressionKind::PhysicsQueryPoint);
  CacheUIQueryVector(context, query_index, "Normal", LN_VectorExpressionKind::PhysicsQueryNormal);
  CacheUIQueryBool(context, query_index, "Accepted", LN_BoolExpressionKind::UIElementEventAccepted);
}

void CompileOnUISoundNode(CompilerContext &context)
{
  if (!ObjectUIQueryHasConsumedOutput(context)) {
    return;
  }
  /* One query for the frame's UI sounds (one a player); each output runs for its own sound. */
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::UIElementEvent;
  query.int_value = int(LN_UIElementEvent::UISound);
  query.cache_key = context.node.identifier;
  const uint32_t query_index = context.program.AddQueryExpression(query);

  for (const auto &[name, sound] : {std::pair{"Move", LN_UISound::Move},
                                    std::pair{"Accept", LN_UISound::Accept},
                                    std::pair{"Back", LN_UISound::Back},
                                    std::pair{"Denied", LN_UISound::Denied}})
  {
    if (!context.OutputFeedsActiveNode(name)) {
      continue;
    }
    /* An event root, as On UI Event's Out: its route runs in the frame update. */
    context.has_event_node = true;
    LN_BoolExpression fired;
    fired.kind = LN_BoolExpressionKind::UISoundFired;
    fired.input0 = query_index;
    fired.int_value = int32_t(sound);
    context.CacheBoolOutputPin(name, context.program.AddBoolExpression(fired));
  }
  if (context.OutputFeedsActiveNode("Object")) {
    LN_ValueExpression object;
    object.kind = LN_ValueExpressionKind::UIElementEventObject;
    object.input0 = query_index;
    context.CacheValueOutputPin("Object", context.program.AddValueExpression(object));
  }
  CacheUIQueryInt(context, query_index, "User", LN_IntExpressionKind::UIElementEventUser);
  for (const auto &[name, kind] :
       {std::pair{"Action", LN_StringExpressionKind::UIElementEventAction},
        std::pair{"Menu", LN_StringExpressionKind::UIElementEventMenu}})
  {
    if (context.OutputFeedsActiveNode(name)) {
      LN_StringExpression text;
      text.kind = kind;
      text.input0 = query_index;
      context.CacheStringOutputPin(name, context.program.AddStringExpression(text));
    }
  }
}

void CompileSetUIInteractableNode(CompilerContext &context)
{
  const uint32_t target = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  const std::optional<uint32_t> interactable = context.BuildBool("Interactable");
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Set UI Interactable flow must be driven by an event or branch");
  if (!interactable || !flows) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetUIInteractable;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = target;
    instruction.bool_expr_index = *interactable;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                         LN_BoolExpressionKind::InstructionExecuted,
                                                         path.event,
                                                         instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileOpenObjectMenuNode(CompilerContext &context)
{
  const uint32_t camera = context.BuildOptionalObject("Camera").value_or(LN_INVALID_INDEX);
  const std::optional<uint32_t> collection = context.BuildValueExpression("Collection");
  const std::optional<uint32_t> input_context = context.BuildString("Input Context");
  /* No User socket: player 0. */
  const uint32_t user = FindInputSocket(context.node, "User") != nullptr ?
                            context.BuildInt("User").value_or(LN_INVALID_INDEX) :
                            LN_INVALID_INDEX;
  /* No Owner: the opener (LN_UIService::MenuOwnerOfObject). */
  const uint32_t menu_owner = context.BuildOptionalObject("Owner").value_or(LN_INVALID_INDEX);
  const bool in_player_view = (context.node.custom2 & LN_UI_MENU_NODE_PLAYER_VIEW) != 0;
  std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Open Object Menu flow must be driven by an event or branch");
  if (!collection || !input_context || !flows) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Opened", "Menu");
  const LN_UIMenuOpenMode open_mode = LN_UnpackUIMenuNodeOpenMode(context.node.custom2);
  const LN_UIMenuPresentation presentation = LN_UnpackUIMenuNodePresentation(context.node.custom2);
  const LN_UIMenuBackBehavior back_behavior = LN_UnpackUIMenuNodeBackBehavior(context.node.custom2);
  const bool pauses_game = (context.node.custom2 & LN_UI_MENU_NODE_PAUSES_GAME) != 0;
  const bool toggles = (context.node.custom2 & LN_UI_MENU_NODE_TOGGLES) != 0;
  if (pauses_game && toggles) {
    /* The key that opened a pause menu closes it again while its pause holds the game: this
     * node (and what follows it) runs while paused, as menu buttons do. */
    for (FlowPath &path : flows->paths) {
      path.ui_event_route = true;
    }
    /* Logic + Physics steps stop while paused: from there, only Back closes the menu. */
    if (std::any_of(flows->paths.begin(), flows->paths.end(), [](const FlowPath &path) {
          return path.event == LN_Event::OnFixedUpdate;
        }))
    {
      context.AddWarning("Flow",
                         "Pause Game with Toggle: this flow runs on Logic + Physics steps, which "
                         "stop while paused, so only Back closes the menu. Link an Input Action "
                         "(or key) straight into Open Object Menu, with nothing after it, for its "
                         "key to close it too");
    }
  }
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::OpenObjectMenu;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = camera;
    instruction.secondary_value_expr_index = *collection;
    instruction.tertiary_value_expr_index = menu_owner;
    instruction.string_expr_index = *input_context;
    instruction.int_expr_index = user;
    instruction.property_ref_index = completion.completed;
    instruction.int_value = std::clamp(int(context.node.custom1),
                                       int(LN_UIMenuRouting::Modal),
                                       int(LN_UIMenuRouting::PassThrough));
    instruction.secondary_int_value = LN_PackUIMenuOpenSettings(
        open_mode, presentation, back_behavior, pauses_game, toggles, in_player_view);
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Opened")) {
    CachePrecompiledRoutes(context, "Opened", ContinuationPaths(*flows, completion.succeeded));
  }
  /* Toggle: the menu was open, so it closed. */
  if (toggles && context.OutputFeedsActiveNode("Closed")) {
    LN_BoolExpression closed;
    closed.kind = LN_BoolExpressionKind::UICommandClosed;
    closed.input0 = completion.completed;
    CachePrecompiledRoutes(
        context, "Closed", ContinuationPaths(*flows, context.program.AddBoolExpression(closed)));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flows, completion.failed));
  }
}

void CompileCloseObjectMenuNode(CompilerContext &context)
{
  const LN_UIMenuCloseOperation operation = LN_UIMenuCloseOperation(std::clamp(
      int(context.node.custom1),
      int(LN_UIMenuCloseOperation::Top),
      int(LN_UIMenuCloseOperation::All)));
  const bool requires_handle = operation == LN_UIMenuCloseOperation::Specific;
  const std::optional<uint32_t> handle = requires_handle ? context.BuildInt("Menu") :
                                                           std::optional<uint32_t>(LN_INVALID_INDEX);
  const uint32_t user = FindInputSocket(context.node, "User") != nullptr ?
                            context.BuildInt("User").value_or(LN_INVALID_INDEX) :
                            LN_INVALID_INDEX;
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Close Object Menu flow must be driven by an event or branch");
  if (!handle || !flows) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Closed", nullptr);
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::CloseObjectMenu;
    instruction.source_ref_index = context.MakeSourceRef("Menu");
    instruction.int_expr_index = *handle;
    instruction.secondary_int_expr_index = user;
    instruction.int_value = int32_t(operation);
    instruction.property_ref_index = completion.completed;
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Closed")) {
    CachePrecompiledRoutes(context, "Closed", ContinuationPaths(*flows, completion.succeeded));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flows, completion.failed));
  }
}

void CompileSetUIFocusNode(CompilerContext &context)
{
  const std::optional<uint32_t> target = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> user = context.BuildInt("User");
  const std::optional<uint32_t> input_context = context.BuildString("Input Context");
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Set UI Focus flow must be driven by an event or branch");
  if (!user || !input_context || !flows) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Done", nullptr);
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetUIFocus;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = target.value_or(LN_INVALID_INDEX);
    instruction.int_expr_index = *user;
    instruction.string_expr_index = *input_context;
    instruction.property_ref_index = completion.completed;
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flows, completion.succeeded));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flows, completion.failed));
  }
}

void CompileGetUIFocusNode(CompilerContext &context)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::UIFocusObject;
  expression.input0 = context.BuildInt("User").value_or(LN_INVALID_INDEX);
  expression.input1 = context.BuildString("Input Context").value_or(LN_INVALID_INDEX);
  context.CacheValueOutputPin("Object", context.program.AddValueExpression(expression));
}

/** An input's expression, or LN_INVALID_INDEX for its default. False (with an error) when a
 * linked input did not compile: it must not quietly become its default (User 0 would set the
 * active camera). */
static bool InputOrDefault(CompilerContext &context,
                           const char *node_name,
                           const char *input,
                           const std::optional<uint32_t> expr,
                           uint32_t &r_index)
{
  if (!expr && context.InputIsLinked(input)) {
    context.AddError(input, std::string(node_name) + " " + input + " input link did not compile");
    return false;
  }
  r_index = expr.value_or(LN_INVALID_INDEX);
  return true;
}

void CompileScreenAnchorNode(CompilerContext &context)
{
  const std::optional<uint32_t> target = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> camera = context.BuildOptionalObject("Camera");
  const std::optional<uint32_t> offset = context.BuildVector("Offset Pixels");
  const std::optional<uint32_t> margin = context.BuildVector("Safe Margin Pixels");
  const std::optional<uint32_t> depth = context.BuildFloat("Depth");
  const std::optional<uint32_t> reference = context.BuildValueExpression("Reference Resolution");
  const std::optional<uint32_t> scale = context.BuildBool("Scale Offset");
  /* Area: 0 the window, 1 a player's split screen view (User). */
  const int32_t area = std::clamp(int32_t(context.node.custom2), 0, 1);
  uint32_t user = LN_INVALID_INDEX;
  if (area == 1 &&
      !InputOrDefault(context, "Screen Anchor", "User", context.BuildInt("User"), user))
  {
    return;
  }
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Screen Anchor flow must be driven by an event or branch");
  /* An empty Object input anchors the tree's own object. */
  if (!offset || !margin || !depth || !reference || !scale || !flows) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ScreenAnchor;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = target.value_or(LN_INVALID_INDEX);
    instruction.secondary_value_expr_index = camera.value_or(LN_INVALID_INDEX);
    instruction.vector_expr_index = *offset;
    instruction.secondary_vector_expr_index = *margin;
    instruction.float_expr_index = *depth;
    instruction.tertiary_value_expr_index = *reference;
    instruction.bool_expr_index = *scale;
    instruction.int_value = std::clamp(int(context.node.custom1), 0, 8);
    instruction.secondary_int_value = area;
    instruction.int_expr_index = user;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                         LN_BoolExpressionKind::InstructionExecuted,
                                                         path.event,
                                                         instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
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
  tween_expr.input2 = context.BuildBool("Ignore Timescale").value_or(LN_INVALID_INDEX);
  tween_expr.int_value = tween_type;
  tween_expr.int_expr_index = BakeCurveTable(context);
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
  rotation_result.output_type = LN_ValueType::Rotation;
  rotation_result.input0 = tween_index;
  rotation_result.input1 = context.BuildRotation("FromRotation").value_or(LN_INVALID_INDEX);
  rotation_result.input2 = context.BuildRotation("ToRotation").value_or(LN_INVALID_INDEX);
  rotation_result.property_ref_index = tween_type == 3 ? 1u : 0u;
  context.CacheVectorOutputPin("ResultRotation",
                               context.program.AddVectorExpression(rotation_result));
}

void CompileAddPhysicsConstraintNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
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
  auto bool_slot = [](LN_RigidBodyConstraintBoolInput input) { return size_t(input); };
  auto vector_slot = [](LN_RigidBodyConstraintVectorInput input) { return size_t(input); };
  auto float_slot = [](LN_RigidBodyConstraintFloatInput input) { return size_t(input); };
  for (const FlowPath &path : flow_events->paths) {
    LN_RigidBodyConstraintCommandPayload payload;
    payload.constraint_object_value_expr_index =
        context.BuildOptionalObject("Constraint Object").value_or(LN_INVALID_INDEX);
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
      payload.bool_expr_indices[bool_slot(input)] =
          context.BuildBool(socket_name).value_or(LN_INVALID_INDEX);
    };
    auto set_vector = [&](const LN_RigidBodyConstraintVectorInput input, const char *socket_name) {
      payload.vector_expr_indices[vector_slot(input)] =
          context.BuildVector(socket_name).value_or(LN_INVALID_INDEX);
    };
    auto set_float = [&](const LN_RigidBodyConstraintFloatInput input, const char *socket_name) {
      payload.float_expr_indices[float_slot(input)] =
          context.BuildFloat(socket_name).value_or(LN_INVALID_INDEX);
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
    set_vector(LN_RigidBodyConstraintVectorInput::AngularSpringDamping, "Angular Spring Damping");

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
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution()) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }
}

void CompileRemovePhysicsConstraintNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Remove Rigid Body Constraints flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::RemovePhysicsConstraint;
    instruction.source_ref_index = context.MakeSourceRef("Name");
    instruction.bool_guard_expr_index = path.bool_guard_expr_index;
    instruction.value_expr_index = context.BuildOptionalObject("Object").value_or(
        LN_INVALID_INDEX);
    instruction.bool_expr_index = context.BuildBool("Remove All").value_or(LN_INVALID_INDEX);
    instruction.string_expr_index = context.BuildString("Name").value_or(LN_INVALID_INDEX);
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution()) {
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

static void CompileGameLoadEventNode(CompilerContext &context)
{
  context.has_event_node = true;
  CompileExpressionOutputsNode(context);
}

/** On Player Join: a frame event, like On Game Loaded. */
static void CompilePlayerJoinEventNode(CompilerContext &context)
{
  context.has_event_node = true;
  CompileExpressionOutputsNode(context);
}

/** Store Value keeps the Value it had when Flow arrived. The store runs right there, on its Done
 * route, also when Done leads nowhere: reading the stored value later (another tick, another
 * event) must neither miss a pulse nor store again. */
static void CompileStoreValueNode(CompilerContext &context)
{
  CompileExpressionOutputsNode(context);
  if (!InputSocketIdentifierIsLinked(context.node, "Flow", context.input_links)) {
    return;
  }
  if (const blender::bNodeSocket *done = FindOutputSocket(context.node, "Done")) {
    BuildFlowConditionRouteFlowEvents(context.program,
                                      context.tree,
                                      context.node,
                                      *done,
                                      context.definition,
                                      context.node_definitions,
                                      context.input_links,
                                      context.value_cache,
                                      context.float_expression_cache,
                                      context.bool_expression_cache,
                                      context.flow_events_cache,
                                      context.loop_frame_cache);
  }
}

static void CompileConstraintBreakEventNode(CompilerContext &context)
{
  context.has_event_node = true;
  CompileExpressionOutputsNode(context);

  const ResolvedLink *name_link = context.InputLink("Constraint Name");
  if (name_link == nullptr || name_link->fromnode == nullptr) {
    return;
  }
  const auto definition = context.node_definitions.find(name_link->fromnode);
  if (definition != context.node_definitions.end() &&
      definition->second->kind == LN_NodeKind::GetRigidBodyConstraints)
  {
    context.AddWarning(
        "Constraint Name",
        "Get Rigid Body Constraints reads the live constraint registry, but Jolt removes a "
        "broken constraint before this event is dispatched. Enter the name directly, use this "
        "node's Contains mode, or enable Match Any.");
  }
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Raycast flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const blender::bNodeSocket *done_socket = FindOutputSocket(context.node, "Done");
  if (done_socket == nullptr) {
    context.AddError("Done", "Raycast requires a Done output");
    return;
  }

  const std::optional<uint32_t> done_expr = BuildOutputBoolExpression(
      context.program,
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
  const std::optional<uint32_t> frame_index = EnsureLoopFrame(context.program,
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
  if (!frame_index || !context.OutputFeedsActiveNode("Completed") ||
      context.OutputFeedsActiveNode("Loop"))
  {
    return;
  }

  /* A completed-only Loop still needs a body block to start and finish its frame. Emit a tiny
   * Nop body rather than relying on a boolean pulse; the execution-block runtime then marks the
   * completion scope after zero, normal, or broken iterations alike. */
  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Loop Completed must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }
  if (MixesRecurringContinuationDomains(*input_flow)) {
    context.AddError("Flow", kLoopMixedDomainsError);
    return;
  }
  AddLoopStartEvents(context.program, *frame_index, input_flow->paths);
  const LN_LoopFrame &frame = context.program.GetLoopFrames()[*frame_index];
  for (const FlowPath &path : input_flow->paths) {
    FlowPath loop_path = path;
    loop_path.bool_guard_expr_index = frame.loop_active_bool_expr_index;
    loop_path.loop_frame_index = *frame_index;
    loop_path.loop_order_depth = uint32_t(loop_path.execution_order.size());
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::Nop;
    instruction.source_ref_index = context.MakeSourceRef("Loop");
    AppendFlowInstruction(context.program, loop_path, instruction);
    const LN_Event continuation_event = ContinuationEvent(path.event);
    if (continuation_event != path.event) {
      loop_path.event = continuation_event;
      AppendFlowInstruction(context.program, loop_path, instruction);
    }
  }
}

static FlowPath AppendPrecompiledRoute(CompilerContext &context,
                                       const FlowPath &source_path,
                                       const uint32_t condition_expr,
                                       const char *output_name,
                                       const uint32_t semantic_order = 0)
{
  LN_Instruction route;
  route.opcode = LN_OpCode::BranchRoute;
  route.source_ref_index = context.MakeSourceRef(output_name);
  route.bool_expr_index = condition_expr;
  route.bool_value = true;
  FlowPath output_path = source_path;
  if (semantic_order != 0) {
    output_path.execution_order.push_back(semantic_order);
  }
  const uint32_t route_index = AppendFlowInstruction(
      context.program, semantic_order != 0 ? output_path : source_path, route);
  output_path.bool_guard_expr_index = AddInstructionPulseExpression(
      context.program, LN_BoolExpressionKind::InstructionExecuted, source_path.event, route_index);
  if (semantic_order == 0) {
    output_path.execution_order.push_back(route_index + 1u);
  }
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

void CompileAnyFlowNode(CompilerContext &context)
{
  if (!context.OutputFeedsActiveNode("Out")) {
    return;
  }

  if (!context.InputIsLinked("In")) {
    context.AddError("In", "Any Flow requires at least one execution input");
    return;
  }
  const std::optional<FlowEventsResult> input_flow = context.ResolveOptionalFlow("In");
  if (!input_flow) {
    context.AddError("In", "Any Flow inputs must be driven by execution outputs");
    return;
  }

  const uint32_t true_expr = AddConstantBoolExpression(context.program, true);
  std::vector<FlowPath> output_paths;
  output_paths.reserve(input_flow->paths.size());
  for (uint32_t path_index = 0; path_index < input_flow->paths.size(); path_index++) {
    output_paths.push_back(AppendPrecompiledRoute(
        context, input_flow->paths[path_index], true_expr, "Out", path_index + 1u));
  }
  CachePrecompiledRoutes(context, "Out", output_paths);
}

static bool CollectOrderedFlowOutputs(CompilerContext &context,
                                      const char *node_name,
                                      size_t minimum_count,
                                      std::vector<std::pair<uint32_t, std::string>> &r_outputs,
                                      bool has_default_output = false);

void CompileSequenceNode(CompilerContext &context)
{
  std::vector<std::pair<uint32_t, std::string>> outputs;
  if (!CollectOrderedFlowOutputs(context, "Sequence", 2, outputs)) {
    return;
  }
  if (std::none_of(outputs.begin(), outputs.end(), [&](const auto &output) {
        return context.OutputFeedsActiveNode(output.second.c_str());
      }))
  {
    return;
  }

  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Sequence Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }

  const uint32_t true_expr = AddConstantBoolExpression(context.program, true);
  for (const auto &[output_rank, output_name] : outputs) {
    if (!context.OutputFeedsActiveNode(output_name.c_str())) {
      continue;
    }
    std::vector<FlowPath> output_paths;
    output_paths.reserve(input_flow->paths.size());
    for (const FlowPath &path : input_flow->paths) {
      output_paths.push_back(
          AppendPrecompiledRoute(context, path, true_expr, output_name.c_str(), output_rank + 1u));
    }
    CachePrecompiledRoutes(context, output_name.c_str(), output_paths);
  }
}

static std::optional<uint32_t> DynamicSocketRank(const blender::bNodeSocket &socket,
                                                 const std::string_view prefix)
{
  const std::string_view name = socket.identifier[0] != '\0' ? socket.identifier : socket.name;
  if (!name.starts_with(prefix) || name.size() == prefix.size()) {
    return std::nullopt;
  }

  uint32_t rank = 0;
  const char *begin = name.data() + prefix.size();
  const char *end = name.data() + name.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, rank);
  if (parsed.ec != std::errc() || parsed.ptr != end || rank >= LN_MAX_DYNAMIC_SOCKET_COUNT) {
    return std::nullopt;
  }
  return rank;
}

static bool CollectOrderedFlowOutputs(CompilerContext &context,
                                      const char *node_name,
                                      const size_t minimum_count,
                                      std::vector<std::pair<uint32_t, std::string>> &r_outputs,
                                      const bool has_default_output)
{
  r_outputs.clear();
  size_t default_output_count = 0;
  for (const blender::bNodeSocket *socket =
           static_cast<const blender::bNodeSocket *>(context.node.outputs.first);
       socket;
       socket = socket->next)
  {
    if (has_default_output && NamesMatch(socket->name, socket->identifier, "Default")) {
      default_output_count++;
      continue;
    }
    const std::optional<uint32_t> rank = DynamicSocketRank(*socket, "Then ");
    if (!rank) {
      context.AddError(socket->name,
                       std::string(node_name) +
                           " outputs must use unique 'Then <number>' sockets from 0 through 63");
      return false;
    }
    const std::string name = socket->identifier[0] != '\0' ? socket->identifier : socket->name;
    r_outputs.emplace_back(*rank, name);
  }

  if (r_outputs.size() < minimum_count || r_outputs.size() > LN_MAX_DYNAMIC_SOCKET_COUNT) {
    context.AddError("Flow",
                     std::string(node_name) + " requires between " +
                         std::to_string(minimum_count) + " and 64 ordered outputs");
    return false;
  }
  if (has_default_output && default_output_count != 1) {
    context.AddError("Default",
                     std::string(node_name) + " requires exactly one Default execution output");
    return false;
  }
  std::sort(r_outputs.begin(), r_outputs.end());
  for (size_t index = 0; index < r_outputs.size(); index++) {
    if (index > 0 && r_outputs[index - 1].first == r_outputs[index].first) {
      context.AddError(r_outputs[index].second.c_str(),
                       std::string(node_name) + " output numbers must be unique");
      return false;
    }
    if (r_outputs[index].first != index) {
      context.AddError(r_outputs[index].second.c_str(),
                       std::string(node_name) +
                           " outputs must be numbered consecutively from Then 0");
      return false;
    }
  }
  return true;
}

static uint32_t AddValueEqualityExpression(LN_Program &program,
                                           const uint32_t first,
                                           const uint32_t second)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::ValueCompare;
  expression.input0 = first;
  expression.input1 = second;
  expression.float_compare_operation = LN_FloatCompareOperation::Equal;
  return program.AddBoolExpression(expression);
}

static void CompileSwitchFlowRoutes(CompilerContext &context,
                                    const FlowEventsResult &input_flow,
                                    const uint32_t selector_value_expr,
                                    const std::vector<std::pair<uint32_t, std::string>> &outputs,
                                    const std::vector<uint32_t> &case_value_exprs)
{
  std::vector<uint32_t> selected_matches;
  selected_matches.reserve(case_value_exprs.size());
  uint32_t prior_matches = LN_INVALID_INDEX;
  for (const uint32_t case_value_expr : case_value_exprs) {
    const uint32_t matching = AddValueEqualityExpression(
        context.program, selector_value_expr, case_value_expr);
    uint32_t selected = matching;
    if (prior_matches != LN_INVALID_INDEX) {
      selected = AddAndBoolExpression(
          context.program, matching, AddNotBoolExpression(context.program, prior_matches));
      prior_matches = AddOrBoolExpression(context.program, prior_matches, matching);
    }
    else {
      prior_matches = matching;
    }
    selected_matches.push_back(selected);
  }

  for (size_t index = 0; index < outputs.size(); index++) {
    const std::string &output_name = outputs[index].second;
    if (!context.OutputFeedsActiveNode(output_name.c_str())) {
      continue;
    }
    std::vector<FlowPath> output_paths;
    output_paths.reserve(input_flow.paths.size());
    for (const FlowPath &path : input_flow.paths) {
      output_paths.push_back(AppendPrecompiledRoute(
          context, path, selected_matches[index], output_name.c_str(), outputs[index].first + 1u));
    }
    CachePrecompiledRoutes(context, output_name.c_str(), output_paths);
  }

  if (context.OutputFeedsActiveNode("Default")) {
    const uint32_t default_condition = AddNotBoolExpression(context.program, prior_matches);
    std::vector<FlowPath> default_paths;
    default_paths.reserve(input_flow.paths.size());
    for (const FlowPath &path : input_flow.paths) {
      default_paths.push_back(AppendPrecompiledRoute(
          context, path, default_condition, "Default", uint32_t(outputs.size()) + 1u));
    }
    CachePrecompiledRoutes(context, "Default", default_paths);
  }
}

void CompileSwitchFlowIntNode(CompilerContext &context)
{
  std::vector<std::pair<uint32_t, std::string>> outputs;
  if (!CollectOrderedFlowOutputs(context, "Integer Switch Flow", 2, outputs, true)) {
    return;
  }
  if (!context.OutputFeedsActiveNode("Default") &&
      std::none_of(outputs.begin(), outputs.end(), [&](const auto &output) {
        return context.OutputFeedsActiveNode(output.second.c_str());
      }))
  {
    return;
  }

  const std::optional<uint32_t> selector = context.BuildValueExpression("Index");
  if (!selector) {
    context.AddError("Index", "Integer Switch Flow requires an Index input");
    return;
  }
  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Integer Switch Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }

  std::vector<uint32_t> case_values;
  case_values.reserve(outputs.size());
  for (const auto &[rank, _name] : outputs) {
    LN_Value value;
    value.type = LN_ValueType::Int;
    value.exists = true;
    value.int_value = int32_t(rank);
    case_values.push_back(AddConstantValueExpression(context.program, value));
  }
  CompileSwitchFlowRoutes(context, *input_flow, *selector, outputs, case_values);
}

void CompileSwitchFlowStringNode(CompilerContext &context)
{
  std::vector<std::pair<uint32_t, std::string>> outputs;
  if (!CollectOrderedFlowOutputs(context, "String Switch Flow", 2, outputs, true)) {
    return;
  }
  if (!context.OutputFeedsActiveNode("Default") &&
      std::none_of(outputs.begin(), outputs.end(), [&](const auto &output) {
        return context.OutputFeedsActiveNode(output.second.c_str());
      }))
  {
    return;
  }

  std::vector<std::pair<uint32_t, std::string>> cases;
  for (const blender::bNodeSocket *socket =
           static_cast<const blender::bNodeSocket *>(context.node.inputs.first);
       socket;
       socket = socket->next)
  {
    const std::optional<uint32_t> rank = DynamicSocketRank(*socket, "Case ");
    if (!rank) {
      continue;
    }
    const std::string name = socket->identifier[0] != '\0' ? socket->identifier : socket->name;
    cases.emplace_back(*rank, name);
  }
  std::sort(cases.begin(), cases.end());
  if (cases.size() != outputs.size()) {
    context.AddError("Value", "String Switch Flow cases and outputs must have matching counts");
    return;
  }
  for (size_t index = 0; index < cases.size(); index++) {
    if (cases[index].first != outputs[index].first ||
        (index > 0 && cases[index - 1].first == cases[index].first))
    {
      context.AddError(cases[index].second.c_str(),
                       "String Switch Flow cases must use unique 'Case <number>' sockets");
      return;
    }
  }

  /* Compared as text, also a number linked into Value (2 matches the case "2"). */
  const std::optional<uint32_t> selector = BuildTextInputAsValue(context, "Value");
  if (!selector) {
    context.AddError("Value", "String Switch Flow requires a Value input");
    return;
  }
  std::vector<uint32_t> case_values;
  case_values.reserve(cases.size());
  for (const auto &[rank, name] : cases) {
    (void)rank;
    const std::optional<uint32_t> value = BuildTextInputAsValue(context, name.c_str());
    if (!value) {
      context.AddError(name.c_str(), "String Switch Flow case could not be compiled");
      return;
    }
    case_values.push_back(*value);
  }

  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "String Switch Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }
  CompileSwitchFlowRoutes(context, *input_flow, *selector, outputs, case_values);
}

static uint32_t AddFlowRouterRouteExpression(LN_Program &program,
                                             const uint32_t state_index,
                                             const uint32_t output_index,
                                             const bool scoped)
{
  LN_BoolExpression expression;
  expression.kind = scoped ? LN_BoolExpressionKind::FlowRouterSelected :
                             LN_BoolExpressionKind::FlowRouterLastRoute;
  expression.int_value = int32_t(state_index);
  expression.secondary_int_value = int32_t(output_index);
  return program.AddBoolExpression(expression);
}

void CompileFlowRouterNode(CompilerContext &context)
{
  std::vector<std::pair<uint32_t, std::string>> outputs;
  if (!CollectOrderedFlowOutputs(context, "Flow Router", 2, outputs)) {
    return;
  }
  const bool has_active_output = std::any_of(
      outputs.begin(), outputs.end(), [&](const auto &output) {
        return context.OutputFeedsActiveNode(output.second.c_str());
      });
  if (!has_active_output) {
    return;
  }
  const uint32_t state_index = context.program.AddFlowControlState();

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Flow Router Reset must be driven by an execution output");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::ResetFlowRouter;
      instruction.source_ref_index = context.MakeSourceRef("Reset");
      instruction.int_value = int32_t(state_index);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Flow Router Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }

  std::vector<uint32_t> route_conditions;
  route_conditions.reserve(outputs.size());
  for (size_t index = 0; index < outputs.size(); index++) {
    route_conditions.push_back(
        AddFlowRouterRouteExpression(context.program, state_index, uint32_t(index), true));
  }
  std::vector<std::vector<FlowPath>> output_paths(outputs.size());
  for (const FlowPath &path : input_flow->paths) {
    LN_Instruction attempt;
    attempt.opcode = LN_OpCode::TryFlowRouter;
    attempt.source_ref_index = context.MakeSourceRef("Flow");
    attempt.int_value = int32_t(state_index);
    attempt.secondary_int_value = int32_t(outputs.size());
    AppendFlowInstruction(context.program, path, attempt);
    for (size_t index = 0; index < outputs.size(); index++) {
      if (!context.OutputFeedsActiveNode(outputs[index].second.c_str())) {
        continue;
      }
      output_paths[index].push_back(AppendPrecompiledRoute(context,
                                                           path,
                                                           route_conditions[index],
                                                           outputs[index].second.c_str(),
                                                           outputs[index].first + 1u));
    }
  }
  for (size_t index = 0; index < outputs.size(); index++) {
    if (!output_paths[index].empty()) {
      CachePrecompiledRoutes(context, outputs[index].second.c_str(), output_paths[index]);
    }
  }
}

void CompileFlipFlopNode(CompilerContext &context)
{
  const bool route_a = context.OutputFeedsActiveNode("A");
  const bool route_b = context.OutputFeedsActiveNode("B");
  const bool reads_state = context.OutputFeedsActiveNode("Is A");
  if (!route_a && !route_b && !reads_state) {
    return;
  }

  const uint32_t state_index = context.program.AddFlowControlState();
  context.CacheBoolOutputPin("Is A",
                             AddFlowRouterRouteExpression(context.program, state_index, 0, false));

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Flip-Flop Reset must be driven by an execution output");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::ResetFlowRouter;
      instruction.source_ref_index = context.MakeSourceRef("Reset");
      instruction.int_value = int32_t(state_index);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  if (!context.InputIsLinked("Flow")) {
    if (route_a || route_b) {
      context.AddError("Flow", "Flip-Flop A and B outputs require an execution input");
    }
    return;
  }
  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Flip-Flop Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }
  const std::array<const char *, 2> outputs = {{"A", "B"}};
  std::array<uint32_t, 2> route_conditions = {{
      AddFlowRouterRouteExpression(context.program, state_index, 0, true),
      AddFlowRouterRouteExpression(context.program, state_index, 1, true),
  }};
  std::array<std::vector<FlowPath>, 2> output_paths;
  for (const FlowPath &path : input_flow->paths) {
    LN_Instruction attempt;
    attempt.opcode = LN_OpCode::TryFlowRouter;
    attempt.source_ref_index = context.MakeSourceRef("Flow");
    attempt.int_value = int32_t(state_index);
    attempt.secondary_int_value = 2;
    AppendFlowInstruction(context.program, path, attempt);
    for (size_t index = 0; index < outputs.size(); index++) {
      if ((index == 0 && route_a) || (index == 1 && route_b)) {
        output_paths[index].push_back(AppendPrecompiledRoute(
            context, path, route_conditions[index], outputs[index], uint32_t(index) + 1u));
      }
    }
  }
  for (size_t index = 0; index < outputs.size(); index++) {
    if (!output_paths[index].empty()) {
      CachePrecompiledRoutes(context, outputs[index], output_paths[index]);
    }
  }
}

void CompileRepeatLimitNode(CompilerContext &context)
{
  const bool has_accepted_output = context.OutputFeedsActiveNode("Accepted");
  const bool has_blocked_output = context.OutputFeedsActiveNode("Blocked");
  const bool has_data_output = context.OutputFeedsActiveNode("Remaining") ||
                               context.OutputFeedsActiveNode("Is Exhausted");
  if (!has_accepted_output && !has_blocked_output && !has_data_output) {
    return;
  }

  const std::optional<uint32_t> limit = context.BuildInt("Limit");
  if (!limit) {
    context.AddError("Limit", "Repeat Limit requires a Limit input");
    return;
  }
  const uint32_t state_index = context.program.AddFlowControlState();
  auto add_state_bool = [&](const LN_BoolExpressionKind kind) {
    LN_BoolExpression expression;
    expression.kind = kind;
    expression.int_value = int32_t(state_index);
    if (kind == LN_BoolExpressionKind::RepeatLimitExhausted) {
      expression.input0 = *limit;
    }
    return context.program.AddBoolExpression(expression);
  };
  const uint32_t accepted_expr = add_state_bool(LN_BoolExpressionKind::RepeatLimitAccepted);
  const uint32_t blocked_expr = add_state_bool(LN_BoolExpressionKind::RepeatLimitBlocked);
  context.CacheBoolOutputPin("Is Exhausted",
                             add_state_bool(LN_BoolExpressionKind::RepeatLimitExhausted));

  LN_IntExpression remaining;
  remaining.kind = LN_IntExpressionKind::RepeatLimitRemaining;
  remaining.int_value = int32_t(state_index);
  remaining.input0 = *limit;
  context.CacheIntOutputPin("Remaining", context.program.AddIntExpression(remaining));

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Repeat Limit Reset must be driven by an execution output");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::ResetRepeatLimit;
      instruction.source_ref_index = context.MakeSourceRef("Reset");
      instruction.int_value = int32_t(state_index);
      AppendFlowInstruction(context.program, path, instruction);
    }
  }

  if (!context.InputIsLinked("Flow")) {
    if (has_accepted_output || has_blocked_output) {
      context.AddError("Flow",
                       "Repeat Limit Accepted and Blocked outputs require an execution input");
    }
    return;
  }
  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Repeat Limit Flow must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }

  std::vector<FlowPath> accepted_paths;
  std::vector<FlowPath> blocked_paths;
  for (const FlowPath &path : input_flow->paths) {
    LN_Instruction attempt;
    attempt.opcode = LN_OpCode::TryRepeatLimit;
    attempt.source_ref_index = context.MakeSourceRef("Flow");
    attempt.int_value = int32_t(state_index);
    attempt.int_expr_index = *limit;
    AppendFlowInstruction(context.program, path, attempt);
    if (has_accepted_output) {
      accepted_paths.push_back(
          AppendPrecompiledRoute(context, path, accepted_expr, "Accepted", 1));
    }
    if (has_blocked_output) {
      blocked_paths.push_back(AppendPrecompiledRoute(context, path, blocked_expr, "Blocked", 2));
    }
  }
  if (!accepted_paths.empty()) {
    CachePrecompiledRoutes(context, "Accepted", accepted_paths);
  }
  if (!blocked_paths.empty()) {
    CachePrecompiledRoutes(context, "Blocked", blocked_paths);
  }
}

void CompileAverageCenterPositionNode(CompilerContext &context)
{
  const LN_AverageCenterSource source = LN_AverageCenterSource(
      std::clamp<int>(context.node.custom1, 0, 2));
  const char *socket_name = source == LN_AverageCenterSource::VectorList ?
                                "Vectors" :
                            source == LN_AverageCenterSource::ObjectList ?
                                "Objects" :
                                "Collection";
  const std::optional<uint32_t> input = context.BuildValueExpression(socket_name);
  if (!input && context.InputIsLinked(socket_name)) {
    context.AddError(socket_name,
                     "Get Average Center Position input link did not compile");
    return;
  }
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::VectorAverage;
  query.average_center_source = source;
  query.cache_key = context.node.identifier;
  if (input) {
    query.input0 = *input;
  }
  const uint32_t query_index = context.program.AddQueryExpression(query);

  LN_VectorExpression center;
  center.kind = LN_VectorExpressionKind::QueryVectorAverage;
  center.input0 = query_index;
  context.CacheVectorOutputPin("Center", context.program.AddVectorExpression(center));

  LN_IntExpression count;
  count.kind = LN_IntExpressionKind::QueryVectorCount;
  count.input0 = query_index;
  context.CacheIntOutputPin("Count", context.program.AddIntExpression(count));

  LN_BoolExpression valid;
  valid.kind = LN_BoolExpressionKind::QueryValid;
  valid.input0 = query_index;
  context.CacheBoolOutputPin("Valid", context.program.AddBoolExpression(valid));
}

void CompileDistanceBetweenNode(CompilerContext &context)
{
  const LN_DistanceEndpointMode mode_a = context.node.custom1 == 1 ?
                                             LN_DistanceEndpointMode::Position :
                                             LN_DistanceEndpointMode::Object;
  const LN_DistanceEndpointMode mode_b = context.node.custom2 == 1 ?
                                             LN_DistanceEndpointMode::Position :
                                             LN_DistanceEndpointMode::Object;

  std::optional<uint32_t> endpoint_a;
  std::optional<uint32_t> endpoint_b;
  if (mode_a == LN_DistanceEndpointMode::Object) {
    endpoint_a = context.BuildOptionalObject("Object A");
    if (!endpoint_a && context.InputIsLinked("Object A")) {
      context.AddError("Object A", "Distance Between Object A input link did not compile");
      return;
    }
  }
  else {
    endpoint_a = context.BuildVector("Position A");
    if (!endpoint_a) {
      context.AddError("Position A", "Distance Between Position A input did not compile");
      return;
    }
  }
  if (mode_b == LN_DistanceEndpointMode::Object) {
    endpoint_b = context.BuildOptionalObject("Object B");
    if (!endpoint_b && context.InputIsLinked("Object B")) {
      context.AddError("Object B", "Distance Between Object B input link did not compile");
      return;
    }
  }
  else {
    endpoint_b = context.BuildVector("Position B");
    if (!endpoint_b) {
      context.AddError("Position B", "Distance Between Position B input did not compile");
      return;
    }
  }

  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::DistanceBetween;
  query.cache_key = context.node.identifier;
  query.distance_endpoint_a = mode_a;
  query.distance_endpoint_b = mode_b;
  if (endpoint_a) {
    query.input0 = *endpoint_a;
  }
  if (endpoint_b) {
    query.input1 = *endpoint_b;
  }
  const uint32_t query_index = context.program.AddQueryExpression(query);

  LN_FloatExpression distance;
  distance.kind = LN_FloatExpressionKind::QueryDistance;
  distance.input0 = query_index;
  context.CacheFloatOutputPin("Distance", context.program.AddFloatExpression(distance));

  LN_FloatExpression squared_distance;
  squared_distance.kind = LN_FloatExpressionKind::QuerySquaredDistance;
  squared_distance.input0 = query_index;
  context.CacheFloatOutputPin("Squared Distance",
                              context.program.AddFloatExpression(squared_distance));

  LN_BoolExpression valid;
  valid.kind = LN_BoolExpressionKind::QueryValid;
  valid.input0 = query_index;
  context.CacheBoolOutputPin("Valid", context.program.AddBoolExpression(valid));
}

void CompilePredictMotionNode(CompilerContext &context)
{
  const uint32_t state_index = context.program.AddFlowControlState();
  const bool position_source = context.node.custom1 == 1;

  std::optional<uint32_t> source_position;
  std::optional<uint32_t> source_object;
  LN_MotionSourceMode source_mode = LN_MotionSourceMode::Position;
  if (position_source) {
    source_position = context.BuildVector("Position");
    if (!source_position) {
      context.AddError("Position", "Predict Motion requires a Position input in Position mode");
      return;
    }
  }
  else {
    source_object = context.BuildOptionalObject("Object");
    if (!source_object && context.InputIsLinked("Object")) {
      context.AddError("Object", "Predict Motion Object input link did not compile");
      return;
    }
    source_mode = source_object ? LN_MotionSourceMode::Object : LN_MotionSourceMode::OwnerObject;
  }

  const std::optional<uint32_t> prediction_time = context.BuildFloat("Prediction Time");
  const std::optional<uint32_t> smoothing_time = context.BuildFloat("Smoothing Time");
  if (!prediction_time || !smoothing_time) {
    context.AddError("Prediction Time", "Predict Motion time inputs could not be compiled");
    return;
  }

  LN_VectorExpression position;
  position.kind = LN_VectorExpressionKind::MotionPosition;
  position.property_ref_index = state_index;
  context.CacheVectorOutputPin("Position", context.program.AddVectorExpression(position));

  LN_VectorExpression velocity = position;
  velocity.kind = LN_VectorExpressionKind::MotionVelocity;
  context.CacheVectorOutputPin("Velocity", context.program.AddVectorExpression(velocity));

  LN_VectorExpression direction = position;
  direction.kind = LN_VectorExpressionKind::MotionDirection;
  context.CacheVectorOutputPin("Direction", context.program.AddVectorExpression(direction));

  LN_VectorExpression predicted_position = position;
  predicted_position.kind = LN_VectorExpressionKind::MotionPredictedPosition;
  context.CacheVectorOutputPin("Predicted Position",
                               context.program.AddVectorExpression(predicted_position));

  LN_FloatExpression speed;
  speed.kind = LN_FloatExpressionKind::MotionSpeed;
  speed.int_value = int32_t(state_index);
  context.CacheFloatOutputPin("Speed", context.program.AddFloatExpression(speed));

  LN_BoolExpression valid;
  valid.kind = LN_BoolExpressionKind::MotionValid;
  valid.int_value = int32_t(state_index);
  context.CacheBoolOutputPin("Valid", context.program.AddBoolExpression(valid));

  const std::optional<FlowEventsResult> reset_flow = context.ResolveOptionalFlow("Reset");
  if (context.InputIsLinked("Reset") && !reset_flow) {
    context.AddError("Reset", "Predict Motion Reset must be driven by an execution output");
    return;
  }
  if (reset_flow) {
    for (const FlowPath &path : reset_flow->paths) {
      LN_Instruction reset;
      reset.opcode = LN_OpCode::ResetMotion;
      reset.source_ref_index = context.MakeSourceRef("Reset");
      reset.int_value = int32_t(state_index);
      AppendFlowInstruction(context.program, path, reset);
    }
  }

  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Predict Motion must be driven by an event or another execution output");
  if (!input_flow) {
    return;
  }

  std::vector<FlowPath> updated_paths;
  updated_paths.reserve(input_flow->paths.size());
  for (const FlowPath &path : input_flow->paths) {
    LN_Instruction sample;
    sample.opcode = LN_OpCode::SampleMotion;
    sample.source_ref_index = context.MakeSourceRef("Flow");
    sample.int_value = int32_t(state_index);
    sample.secondary_int_value = int32_t(source_mode);
    sample.float_expr_index = *prediction_time;
    sample.secondary_float_expr_index = *smoothing_time;
    if (source_position) {
      sample.vector_expr_index = *source_position;
    }
    if (source_object) {
      sample.value_expr_index = *source_object;
    }
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, sample);
    FlowPath updated_path = path;
    updated_path.bool_guard_expr_index = AddInstructionPulseExpression(
        context.program, LN_BoolExpressionKind::InstructionExecuted, path.event, instruction_index);
    updated_path.execution_order.push_back(instruction_index + 1u);
    updated_paths.push_back(std::move(updated_path));
  }
  CachePrecompiledRoutes(context, "Updated", updated_paths);
}

void CompileBreakLoopNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> input_flow = context.ResolveRequiredPrimaryFlow(
      "Break Loop must be driven by a Loop body execution output");
  if (!input_flow) {
    return;
  }
  if (std::any_of(input_flow->paths.begin(), input_flow->paths.end(), [](const FlowPath &path) {
        return path.loop_frame_index == LN_INVALID_INDEX;
      }))
  {
    context.AddError("Flow", "Break Loop can only run inside a Loop body");
    return;
  }
  for (const FlowPath &path : input_flow->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::BreakLoop;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = int32_t(path.loop_frame_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileFlowGateNode(CompilerContext &context)
{
  const std::optional<uint32_t> start_open = context.BuildBool("Start Open");
  if (!start_open) {
    context.AddError("Start Open", "Flow Gate requires a Start Open boolean");
    return;
  }

  LN_BoolExpression open_expression;
  open_expression.kind = LN_BoolExpressionKind::FlowGateOpen;
  open_expression.input0 = *start_open;
  const uint32_t open_expr = context.program.AddBoolExpression(open_expression);
  context.CacheBoolOutputPin("Is Open", open_expr);

  struct GateInput {
    const char *name;
    LN_FlowGateControl control;
    uint8_t fallback_rank;
    bool is_enter;
  };
  struct PendingGatePulse {
    FlowPath path;
    GateInput input;
  };
  /* Explicit upstream execution order wins. If several pins receive the exact same source pulse,
   * use a stable fallback of Toggle, Open, Close, Enter, giving Close final control precedence and
   * sampling Enter afterward. Repeated pulses remain observable (two toggles cancel). */
  constexpr std::array<GateInput, 3> controls = {{
      {"Toggle", LN_FlowGateControl::Toggle, 0, false},
      {"Open", LN_FlowGateControl::Open, 1, false},
      {"Close", LN_FlowGateControl::Close, 2, false},
  }};
  std::vector<PendingGatePulse> pending_pulses;
  for (const GateInput &control : controls) {
    if (!context.InputIsLinked(control.name)) {
      continue;
    }
    const std::optional<FlowEventsResult> control_flow = context.ResolveOptionalFlow(control.name);
    if (!control_flow) {
      context.AddError(control.name, "Flow Gate controls must be driven by execution outputs");
      return;
    }
    for (const FlowPath &path : control_flow->paths) {
      pending_pulses.push_back({path, control});
    }
  }

  std::optional<FlowEventsResult> enter_flow;
  if (context.InputIsLinked("Enter")) {
    enter_flow = context.ResolveOptionalFlow("Enter");
    if (!enter_flow) {
      context.AddError("Enter", "Flow Gate Enter must be driven by an execution output");
      return;
    }
  }
  else if (context.OutputFeedsActiveNode("Exit")) {
    context.AddError("Enter", "Flow Gate Exit requires an Enter execution input");
    return;
  }
  if (enter_flow && context.OutputFeedsActiveNode("Exit")) {
    const GateInput enter = {"Enter", LN_FlowGateControl::Toggle, 3, true};
    for (const FlowPath &path : enter_flow->paths) {
      pending_pulses.push_back({path, enter});
    }
  }

  std::stable_sort(pending_pulses.begin(),
                   pending_pulses.end(),
                   [](const PendingGatePulse &a, const PendingGatePulse &b) {
                     if (a.path.event != b.path.event) {
                       return int(a.path.event) < int(b.path.event);
                     }
                     if (a.path.execution_order != b.path.execution_order) {
                       return std::lexicographical_compare(a.path.execution_order.begin(),
                                                           a.path.execution_order.end(),
                                                           b.path.execution_order.begin(),
                                                           b.path.execution_order.end());
                     }
                     return a.input.fallback_rank < b.input.fallback_rank;
                   });

  std::vector<FlowPath> output_paths;
  output_paths.reserve(enter_flow ? enter_flow->paths.size() : 0u);
  for (const PendingGatePulse &pending : pending_pulses) {
    if (!pending.input.is_enter) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::SetFlowGate;
      instruction.source_ref_index = context.MakeSourceRef(pending.input.name);
      instruction.int_value = int32_t(open_expr);
      instruction.secondary_int_value = int32_t(pending.input.control);
      AppendFlowInstruction(context.program, pending.path, instruction);
      continue;
    }

    LN_Instruction attempt;
    attempt.opcode = LN_OpCode::TryFlowGate;
    attempt.source_ref_index = context.MakeSourceRef("Enter");
    attempt.int_value = int32_t(open_expr);
    const uint32_t attempt_index = AppendFlowInstruction(context.program, pending.path, attempt);

    FlowPath output_path = pending.path;
    output_path.bool_guard_expr_index = AddInstructionPulseExpression(
        context.program,
        LN_BoolExpressionKind::InstructionExecuted,
        pending.path.event,
        attempt_index);
    output_paths.push_back(output_path);
  }
  CachePrecompiledRoutes(context, "Exit", output_paths);
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

void CompileOnNextFrameNode(CompilerContext &context)
{
  const uint32_t state_index = context.program.AddTimeFlowState();

  LN_BoolExpression done;
  done.kind = LN_BoolExpressionKind::NextFrameDone;
  done.int_value = int32_t(state_index);
  context.CacheBoolOutputPin("Completed", context.program.AddBoolExpression(done));

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Wait for Next Frame flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::ArmNextFrame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = int32_t(state_index);
    AppendFlowInstruction(context.program, path, instruction);
  }
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
  if (MixesRecurringContinuationDomains(*flow_events)) {
    context.AddError("Flow",
                     "Cooldown cannot mix Frame Update and Logic + Physics trigger paths because "
                     "one cooldown has one clock domain");
    return;
  }
  if (std::any_of(flow_events->paths.begin(), flow_events->paths.end(), [](const FlowPath &path) {
        return path.loop_frame_index != LN_INVALID_INDEX;
      }))
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
      accepted_paths.push_back(AppendPrecompiledRoute(context, path, accepted_expr, "Accepted"));
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
    CachePrecompiledRoutes(context, "Completed", ContinuationPaths(*flow_events, completed_expr));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Delay flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (MixesRecurringContinuationDomains(*flow_events)) {
    context.AddError("Flow",
                     "Delay cannot mix Frame Update and Logic + Physics trigger paths because one "
                     "delay has one clock domain");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Timer Set Timer must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (MixesRecurringContinuationDomains(*flow_events)) {
    context.AddError("Set Timer",
                     "Timer cannot mix Frame Update and Logic + Physics trigger paths because one "
                     "timer has one clock domain");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Pulsify flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (MixesRecurringContinuationDomains(*flow_events)) {
    context.AddError("Flow",
                     "Pulsify cannot mix Frame Update and Logic + Physics trigger paths because "
                     "one pulse generator has one clock domain");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Barrier flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (MixesRecurringContinuationDomains(*flow_events)) {
    context.AddError("Flow",
                     "Barrier cannot mix Frame Update and Logic + Physics trigger paths because "
                     "one barrier has one clock domain");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Print flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Quit Game flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Restart Game flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Load Blender File flow must be driven by an event or branch");
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
  const std::optional<uint32_t> target_expr = use_target ? context.BuildOptionalObject("Target") :
                                                           std::nullopt;
  if (use_target && !target_expr) {
    context.AddWarning("Target",
                       "Send Event target mode has no Target object and will not send events; "
                       "turn Use Target off for broadcast");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Send Event flow must be driven by an event or branch");
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
    context.AddError("Property",
                     "Set Global Property requires category, property, and value inputs");
    return;
  }

  const std::optional<uint32_t> done_expr = context.BuildPrimaryExecution();
  if (done_expr) {
    context.CacheBoolOutputPin("Done", *done_expr);
  }

  const std::optional<uint32_t> persistent_expr = context.BuildBool("Persistent");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Global Property flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Save Variable flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Save Variable Dict flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Translate flow must be driven by an event or branch");
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
  const std::optional<uint32_t> rotating_object_expr = context.BuildOptionalObject(
      "Rotating Object");
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
    context.AddError("Destination",
                     "Move To with Navmesh requires destination, speed, and threshold inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Move To with Navmesh flow must be driven by an event or branch");
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
    done_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionExecuted,
                                      path.event,
                                      instruction_index));
    reached_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionReached,
                                      path.event,
                                      instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
  context.CacheBoolOutputPin("When Reached",
                             CombineBoolExpressionsWithOr(context.program, reached_outputs));
  if (first_instruction_index != LN_INVALID_INDEX) {
    LN_VectorExpression next_point_expression;
    next_point_expression.kind = LN_VectorExpressionKind::InstructionNextPoint;
    next_point_expression.input0 = first_instruction_index;
    const uint32_t next_point_vector = context.program.AddVectorExpression(next_point_expression);
    if (g_active_instruction_schedule != nullptr) {
      g_active_instruction_schedule->RecordInstructionIndexVectorExpression(
          flow_events->paths.front().event, next_point_vector);
    }
    context.CacheVectorOutputPin("Next Point", next_point_vector);

    LN_ValueExpression next_point_value;
    next_point_value.kind = LN_ValueExpressionKind::FromVector;
    next_point_value.input0 = next_point_vector;
    context.CacheValueOutputPin("Next Point",
                                context.program.AddValueExpression(next_point_value));
  }
}

void CompileFollowPathNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Moving Object");
  const std::optional<uint32_t> rotating_object_expr = context.BuildOptionalObject(
      "Rotating Object");
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
    context.AddError("Path Points",
                     "Follow Path requires path points, speed, and threshold inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Follow Path flow must be driven by an event or branch");
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
    done_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionExecuted,
                                      path.event,
                                      instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileSetCollectionVisibilityNode(CompilerContext &context)
{
  if (!context.InputIsLinked("Collection")) {
    const std::optional<LN_Value> collection = context.ReadValue(
        "Collection", LN_ValueType::CollectionRef);
    if (!collection || !collection->exists || collection->type != LN_ValueType::CollectionRef) {
      context.AddError("Collection", "Set Collection Render Visibility requires a collection");
      return;
    }
  }
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  const std::optional<uint32_t> visible_expr = context.BuildBool("Visible");
  const std::optional<uint32_t> recursive_expr = context.BuildBool("Include Child Collections");
  if (!collection_expr || !visible_expr || !recursive_expr) {
    context.AddError("Collection",
                     "Set Collection Render Visibility requires collection and visibility inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Collection Render Visibility flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCollectionVisibility;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *collection_expr;
    instruction.bool_expr_index = *visible_expr;
    instruction.secondary_bool_expr_index = *recursive_expr;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionExecuted,
                                      path.event,
                                      instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileSetCollectionMemberNode(CompilerContext &context)
{
  const char *name = "Set Collection Member";
  if (!context.InputIsLinked("Collection")) {
    const std::optional<LN_Value> collection = context.ReadValue(
        "Collection", LN_ValueType::CollectionRef);
    if (!collection || !collection->exists || collection->type != LN_ValueType::CollectionRef) {
      context.AddError("Collection", "Set Collection Member requires a collection");
      return;
    }
  }
  const std::optional<uint32_t> collection_expr = context.BuildValueExpression("Collection");
  if (!collection_expr) {
    context.AddError("Collection", "Set Collection Member requires a collection input");
    return;
  }
  uint32_t member, include_children, object_expr;
  if (!InputOrDefault(context, name, "Member", context.BuildBool("Member"), member) ||
      !InputOrDefault(
          context, name, "Include Children", context.BuildBool("Include Children"), include_children) ||
      /* Empty Object: the tree's own object. A link that did not compile is an error. */
      !InputOrDefault(context, name, "Object", context.BuildOptionalObject("Object"), object_expr))
  {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Collection Member flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCollectionMember;
    instruction.source_ref_index = context.MakeSourceRef("Collection");
    instruction.value_expr_index = *collection_expr;
    instruction.secondary_value_expr_index = object_expr;
    instruction.bool_expr_index = member;
    instruction.secondary_bool_expr_index = include_children;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionExecuted,
                                      path.event,
                                      instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompilePlayActionNode(CompilerContext &context)
{
  const blender::ID *action_id = context.node.id;
  std::string action_name;
  if (action_id == nullptr) {
    context.AddWarning("action", "Play Action will pulse Failed: no Action is selected");
  }
  else {
    /* ID codes live in the first two bytes of #ID.name (see #GS in DNA_ID.h). Do not read
     * `sizeof(ID_Type)` from `name`; the enum is wider than the on-disk/runtime prefix. */
    const int16_t id_code = int16_t(*reinterpret_cast<const uint16_t *>(action_id->name));
    if (id_code != int16_t(blender::ID_AC)) {
      context.AddWarning("action",
                         "Play Action will pulse Failed: selected data-block is not an Action");
    }
    else {
      action_name.assign(action_id->name + 2);
    }
  }
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

  const int play_mode = int(
      std::clamp(int(context.node.custom1), 0, int(BL_Action::ACT_MODE_MAX) - 1));
  const int blend_mode = int(
      std::clamp(int(context.node.custom2), 0, int(BL_Action::ACT_BLEND_MAX) - 1));
  const uint32_t pack = uint32_t(play_mode & 0xFF) | (uint32_t(blend_mode & 0xFF) << 8u);

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Play Action flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const ActionCompletionOutputs completion = AddActionCompletionOutputs(context);
  if (play_mode != BL_Action::ACT_MODE_PLAY &&
      (completion.output_mask & LN_ACTION_OUTPUT_COMPLETED) != 0u)
  {
    context.AddWarning("Completed",
                       "Loop and Ping Pong playback do not complete naturally; use Interrupted "
                       "to react when that exact playback is stopped or replaced");
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
    instruction.secondary_int_value = int32_t(completion.output_mask);
    instruction.property_ref_index = completion.lifecycle;
    instruction.secondary_vector_value = MT_Vector3(-1.0f, 1.0f, 0.0f);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }

  const struct {
    const char *name;
    uint32_t expression;
  } routes[] = {{"Started", completion.started},
                {"Completed", completion.completed},
                {"Interrupted", completion.interrupted},
                {"Failed", completion.failed}};
  for (const auto &route : routes) {
    if (route.expression != LN_INVALID_INDEX) {
      CachePrecompiledRoutes(context,
                             route.name,
                             ContinuationPaths(*flow_events, route.expression));
    }
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Stop Action flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Action Frame flow must be driven by an event or branch");
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

void CompileSetShapeKeyValueNode(CompilerContext &context)
{
  const std::optional<uint32_t> shape_key_expr = context.BuildString("Shape Key");
  const std::optional<uint32_t> value_expr = context.BuildFloat("Value");
  if (!shape_key_expr || !value_expr) {
    context.AddError("Shape Key", "Set Shape Key Value requires shape key and value inputs");
    return;
  }
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Shape Key Value flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetShapeKeyValue;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.string_expr_index = *shape_key_expr;
    instruction.float_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileStopAllSoundsNode(CompilerContext &context)
{
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Stop All Sounds flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Play Sound flow must be driven by an event or branch");
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
    instruction.secondary_int_value = int32_t(LN_SoundGroupFromInt(context.node.custom2, false));
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetVolumeNode(CompilerContext &context)
{
  const std::optional<uint32_t> volume_expr = context.BuildFloat("Volume");
  const std::optional<uint32_t> fade_expr = context.BuildFloat("Fade");
  if (!volume_expr || !fade_expr) {
    context.AddError("Volume", "Set Volume requires volume and fade inputs");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Volume flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetSoundVolume;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_value = int32_t(LN_SoundGroupFromInt(context.node.custom1, true));
    instruction.float_expr_index = *volume_expr;
    instruction.secondary_float_expr_index = *fade_expr;
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Stop Sound flow must be driven by an event or branch");
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
  const int32_t listener = std::clamp(int32_t(context.node.custom1),
                                      int32_t(LN_SoundListener::NearestPlayer),
                                      int32_t(LN_SoundListener::ActiveCamera));
  uint32_t user_expr = LN_INVALID_INDEX;
  if (listener == int32_t(LN_SoundListener::Player) &&
      !InputOrDefault(context, "Play Sound 3D", "User", context.BuildInt("User"), user_expr))
  {
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Play Sound 3D flow must be driven by an event or branch");
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
    instruction.int_value = listener;
    instruction.secondary_int_expr_index = user_expr;
    instruction.secondary_int_value = int32_t(LN_SoundGroupFromInt(context.node.custom2, false));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Start Speaker flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Pause Sound flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Resume Sound flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Pose Rotation flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Constraint Influence flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
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
    context.AddError("Target Position",
                     "Move To requires target, speed, and stop distance inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Move To flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  /* Reached: on the pulse whose move ends within Stop Distance of the target (or the object is
   * already there). It needs where the object is, read only when Reached leads somewhere. */
  std::optional<uint32_t> position_expr;
  if (context.OutputFeedsActiveNode("Reached")) {
    LN_ValueExpression attribute;
    attribute.kind = LN_ValueExpressionKind::ObjectAttribute;
    if (object_expr) {
      attribute.input0 = *object_expr;
    }
    attribute.input1 = AddConstantStringExpression(context.program, "worldPosition");
    LN_VectorExpression position;
    position.kind = LN_VectorExpressionKind::FromGenericValue;
    position.input0 = context.program.AddValueExpression(attribute);
    position_expr = context.program.AddVectorExpression(position);
  }
  std::vector<uint32_t> reached_outputs;
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
    if (position_expr) {
      instruction.secondary_vector_expr_index = *position_expr;
    }
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    reached_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                            LN_BoolExpressionKind::InstructionReached,
                                                            path.event,
                                                            instruction_index));
  }
  context.CacheBoolOutputPin("Reached",
                             CombineBoolExpressionsWithOr(context.program, reached_outputs));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Slow Follow flow must be driven by an event or branch");
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
  if (!context.InputIsLinked("Scene")) {
    const std::optional<LN_Value> scene = context.ReadValue("Scene", LN_ValueType::String);
    if (!scene || scene->type != LN_ValueType::String || scene->string_value.empty()) {
      context.AddError("Scene", "Load Scene requires a non-empty scene name");
      return;
    }
  }
  const std::optional<uint32_t> scene_expr = context.BuildString("Scene");
  if (!scene_expr) {
    context.AddError("Scene", "Load Scene requires a scene name input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Load Scene flow must be driven by an event or branch");
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
  if (!context.InputIsLinked("Scene")) {
    const std::optional<LN_Value> scene = context.ReadValue("Scene", LN_ValueType::String);
    if (!scene || scene->type != LN_ValueType::String || scene->string_value.empty()) {
      context.AddError("Scene", "Set Scene requires a non-empty scene name");
      return;
    }
  }
  const std::optional<uint32_t> scene_expr = context.BuildString("Scene");
  if (!scene_expr) {
    context.AddError("Scene", "Set Scene requires a scene name input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Scene flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Save Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (std::any_of(flow_events->paths.begin(), flow_events->paths.end(), [](const FlowPath &path) {
        return path.loop_frame_index != LN_INVALID_INDEX;
      }))
  {
    context.AddError("Flow", "Save Game cannot run inside a Loop body");
    return;
  }

  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::PersistenceCompleted;
  const uint32_t completed_expr = context.program.AddBoolExpression(completed);
  LN_BoolExpression succeeded;
  succeeded.kind = LN_BoolExpressionKind::PersistenceSucceeded;
  succeeded.input0 = completed_expr;
  const uint32_t succeeded_expr = context.program.AddBoolExpression(succeeded);
  context.CacheBoolOutputPin("Success", succeeded_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SaveGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *slot_expr;
    instruction.bool_expr_index = completed_expr;
    if (path_expr) {
      instruction.string_expr_index = *path_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flow_events, completed_expr));
  }
}

void CompileSetSaveExistenceNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<uint32_t> enabled_expr = context.BuildBool("Enabled");
  if (!object_expr && IsInputSocketLinked(context.node, "Object", context.input_links)) {
    context.AddError("Object", "Set Save Object State object input did not compile");
    return;
  }
  if (!enabled_expr) {
    context.AddError("Enabled", "Set Save Object State requires an Enabled input");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Save Object State flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::PersistenceCompleted;
  const uint32_t completed_expr = context.program.AddBoolExpression(completed);
  LN_BoolExpression succeeded;
  succeeded.kind = LN_BoolExpressionKind::PersistenceSucceeded;
  succeeded.input0 = completed_expr;
  const uint32_t succeeded_expr = context.program.AddBoolExpression(succeeded);
  context.CacheBoolOutputPin("Success", succeeded_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetSaveExistence;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.bool_expr_index = *enabled_expr;
    instruction.secondary_bool_expr_index = completed_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flow_events, completed_expr));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Load Game flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  if (std::any_of(flow_events->paths.begin(), flow_events->paths.end(), [](const FlowPath &path) {
        return path.loop_frame_index != LN_INVALID_INDEX;
      }))
  {
    context.AddError("Flow", "Load Game cannot run inside a Loop body");
    return;
  }

  LN_BoolExpression completed;
  completed.kind = LN_BoolExpressionKind::PersistenceCompleted;
  const uint32_t completed_expr = context.program.AddBoolExpression(completed);
  LN_BoolExpression succeeded;
  succeeded.kind = LN_BoolExpressionKind::PersistenceSucceeded;
  succeeded.input0 = completed_expr;
  const uint32_t succeeded_expr = context.program.AddBoolExpression(succeeded);
  context.CacheBoolOutputPin("Success", succeeded_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::LoadGame;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.int_expr_index = *slot_expr;
    instruction.bool_expr_index = completed_expr;
    if (path_expr) {
      instruction.string_expr_index = *path_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flow_events, completed_expr));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Align Axis to Vector flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Replace Mesh flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Copy Property flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Pose Location flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Pose Transform flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Pose Scale flow must be driven by an event or branch");
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
    context.AddError("Value",
                     "Set Bone Attribute requires a value input for the selected attribute");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Bone Attribute flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Target flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Attribute flow must be driven by an event or branch");
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
  const std::optional<uint32_t> socket_expr = direct_node_socket ? context.BuildString("Socket") :
                                                                   std::nullopt;
  const char *value_socket = EditorNodeValueSocketIdentifier(context.node.custom1);
  const std::optional<uint32_t> value_expr = BuildEditorNodeValueExpression(context);
  if (!modifier_expr || !node_or_input_expr || (direct_node_socket && !socket_expr) || !value_expr)
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
    context.AddError(value_socket,
                     "Set Editor Node Value requires node, socket, and value inputs");
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
      context.AddWarning("Flow", "Make Node Tree Unique skipped: no compositor Scene selected");
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
        target_tree = reinterpret_cast<const blender::Scene *>(target_id)->compositing_node_group;
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

  const int32_t target = context.node.custom1 == target_name          ? 0 :
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
  if (operation == operation_replace &&
      (replace_target < target_name || replace_target > target_persistent_id))
  {
    context.AddError("Modifier", "Assign Geometry Nodes Modifier has an unsupported target");
    return;
  }

  const blender::ID *tree_id = context.node.id;
  using blender::ID_Type;
  if (tree_id == nullptr || GS(tree_id->name) != blender::ID_NT) {
    context.AddWarning("Modifier",
                       "Assign Geometry Nodes Modifier skipped: no node group selected");
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
  if ((needs_string && !string_expr) || (needs_index && !index_expr)) {
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Draw Line flow must be driven by an event or branch");
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
    context.AddError("Width",
                     cube ? "Draw Cube requires color, origin, and width" :
                            "Draw Box requires color, origin, width, length, and height");
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      cube ? "Draw Cube flow must be driven by an event or branch" :
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
    const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
        "Draw flow must be driven by an event or branch");
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
    const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
        "Draw flow must be driven by an event or branch");
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
    const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
        "Draw flow must be driven by an event or branch");
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
    const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
        "Draw flow must be driven by an event or branch");
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
  context.AddError(
      "Node",
      "This native node is registered but does not have a runtime command implementation yet");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Rotate To flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Attribute flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  const std::optional<uint32_t> teleport_expr = BuildSetObjectAttributeTeleport(context);

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
      AppendSetObjectAttributeVectorInstruction(context,
                                                path,
                                                object_expr,
                                                position_opcode,
                                                "Position",
                                                *position_expr,
                                                position_fallback);
      AppendSetObjectAttributeVectorInstruction(context,
                                                path,
                                                object_expr,
                                                orientation_opcode,
                                                "Rotation",
                                                *rotation_expr,
                                                rotation_fallback);
      AppendSetObjectAttributeVectorInstruction(
          context, path, object_expr, scale_opcode, "Scale", *scale_expr, scale_fallback);
      if (teleport_expr) {
        AppendSetObjectAttributeTeleport(context, path, object_expr, *teleport_expr);
      }
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
    if (teleport_expr) {
      AppendSetObjectAttributeTeleport(context, path, object_expr, *teleport_expr);
    }
  }
}

void CompileSetRigidBodyAttributeNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
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
      instruction.vector_value = MT_Vector3(
          FloatExpressionConstantFallback(context.program, *value_expr), 0.0f, 0.0f);
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
      instruction.vector_value = MT_Vector3(
          FloatExpressionConstantFallback(context.program, *linear_expr),
          FloatExpressionConstantFallback(context.program, *angular_expr),
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
      instruction.secondary_int_value = BoolExpressionConstantFallback(context.program,
                                                                       *wake_expr) ?
                                            1 :
                                            0;
      break;
    }
    case LN_RigidBodyAttribute::AxisLocks: {
      const std::optional<uint32_t> translation_expr = context.BuildVector("Lock Translation");
      const std::optional<uint32_t> rotation_expr = context.BuildVector("Lock Rotation");
      if (!translation_expr || !rotation_expr) {
        context.AddError(
            "Lock Translation",
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Start Logic Tree flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Stop Logic Tree flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Run Logic Tree flow must be driven by an event or branch");
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
  if (const std::optional<LN_Value> init_val = context.ReadValue("Initialize", LN_ValueType::Bool))
  {
    if (init_val->type == LN_ValueType::Bool) {
      default_init = init_val->bool_value;
    }
  }
  const std::optional<uint32_t> init_expr = context.BuildBool("Initialize");

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Install Logic Tree flow must be driven by an event or branch");
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
  /* An empty Camera is no camera: User 0 keeps the active camera (as before), Users 1 to 7 lose
   * their split screen view. */
  const std::optional<uint32_t> camera_expr = context.BuildOptionalObject("Camera");
  if (!camera_expr && context.InputIsLinked("Camera")) {
    context.AddError("Camera", "Set Camera requires a camera input");
    return;
  }
  uint32_t user_expr;
  if (!InputOrDefault(context, "Set Camera", "User", context.BuildInt("User"), user_expr)) {
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Camera flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetActiveCamera;
    instruction.source_ref_index = context.MakeSourceRef("Camera");
    instruction.value_expr_index = camera_expr.value_or(LN_INVALID_INDEX);
    instruction.int_expr_index = user_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetSplitScreenNode(CompilerContext &context)
{
  uint32_t enabled_expr;
  uint32_t divider_expr;
  uint32_t keep_mouse_expr;
  if (!InputOrDefault(
          context, "Set Split Screen", "Enabled", context.BuildBool("Enabled"), enabled_expr) ||
      !InputOrDefault(
          context, "Set Split Screen", "Divider", context.BuildInt("Divider"), divider_expr) ||
      !InputOrDefault(context,
                      "Set Split Screen",
                      "Keep Mouse in View",
                      context.BuildBool("Keep Mouse in View"),
                      keep_mouse_expr))
  {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Split Screen flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const int32_t choices = (int32_t(context.node.custom1) & 0x7) |
                          (int32_t(context.node.custom2) << LN_SPLIT_SCREEN_FIELD_OF_VIEW_SHIFT);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetSplitScreen;
    instruction.source_ref_index = context.MakeSourceRef("Enabled");
    instruction.bool_expr_index = enabled_expr;
    instruction.int_expr_index = divider_expr;
    instruction.secondary_bool_expr_index = keep_mouse_expr;
    instruction.int_value = choices;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCameraViewportNode(CompilerContext &context)
{
  const std::optional<uint32_t> camera_expr = context.BuildCameraOrActive("Camera");
  if (!camera_expr) {
    context.AddError("Camera", "Set Camera Viewport requires a camera input");
    return;
  }
  const char *name = "Set Camera Viewport";
  uint32_t enabled, left, top, width, height, on_top;
  /* Area: 0 the window, 1 a player's split screen view (User). */
  const int32_t area = std::clamp(int32_t(context.node.custom1), 0, 1);
  uint32_t user = LN_INVALID_INDEX;
  if ((area == 1 && !InputOrDefault(context, name, "User", context.BuildInt("User"), user)) ||
      !InputOrDefault(context, name, "Enabled", context.BuildBool("Enabled"), enabled) ||
      !InputOrDefault(context, name, "Left", context.BuildFloat("Left"), left) ||
      !InputOrDefault(context, name, "Top", context.BuildFloat("Top"), top) ||
      !InputOrDefault(context, name, "Width", context.BuildFloat("Width"), width) ||
      !InputOrDefault(context, name, "Height", context.BuildFloat("Height"), height) ||
      !InputOrDefault(context, name, "On Top", context.BuildBool("On Top"), on_top))
  {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Camera Viewport flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCameraViewport;
    instruction.source_ref_index = context.MakeSourceRef("Camera");
    instruction.value_expr_index = *camera_expr;
    instruction.bool_expr_index = enabled;
    instruction.float_expr_index = left;
    instruction.secondary_float_expr_index = top;
    instruction.tertiary_float_expr_index = width;
    instruction.quaternary_float_expr_index = height;
    instruction.secondary_bool_expr_index = on_top;
    instruction.secondary_int_value = area;
    instruction.int_expr_index = user;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetCameraVisibilityNode(CompilerContext &context)
{
  const std::optional<uint32_t> camera_expr = context.BuildCameraOrActive("Camera");
  if (!camera_expr) {
    context.AddError("Camera", "Set Camera Visibility requires a camera input");
    return;
  }
  const char *name = "Set Camera Visibility";
  uint32_t visible, keep_shadow, include_children;
  if (!InputOrDefault(context, name, "Visible", context.BuildBool("Visible"), visible) ||
      !InputOrDefault(context, name, "Keep Shadow", context.BuildBool("Keep Shadow"), keep_shadow) ||
      !InputOrDefault(
          context, name, "Include Children", context.BuildBool("Include Children"), include_children))
  {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Camera Visibility flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  const uint32_t object_expr = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetCameraVisibility;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = *camera_expr;
    instruction.secondary_value_expr_index = object_expr;
    instruction.bool_expr_index = visible;
    instruction.secondary_bool_expr_index = keep_shadow;
    instruction.tertiary_bool_expr_index = include_children;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetPlayerPointerNode(CompilerContext &context)
{
  const char *name = "Set Player Pointer";
  uint32_t user, enabled, press_action;
  if (!InputOrDefault(context, name, "User", context.BuildInt("User"), user) ||
      !InputOrDefault(context, name, "Enabled", context.BuildBool("Enabled"), enabled) ||
      !InputOrDefault(
          context, name, "Press Action", context.BuildString("Press Action"), press_action))
  {
    return;
  }
  /* View Center: the middle of the player's view points, as far as Reach. Surface: a cursor on
   * a screen. Each mode reads only its own inputs (the others are hidden). */
  const bool aim = context.node.custom1 == 1;
  uint32_t move_action = LN_INVALID_INDEX;
  uint32_t speed = LN_INVALID_INDEX;
  uint32_t reach = LN_INVALID_INDEX;
  uint32_t surface = LN_INVALID_INDEX;
  uint32_t cursor = LN_INVALID_INDEX;
  if (aim) {
    if (!InputOrDefault(context, name, "Reach", context.BuildFloat("Reach"), reach)) {
      return;
    }
  }
  else {
    if (!InputOrDefault(
            context, name, "Move Action", context.BuildString("Move Action"), move_action) ||
        !InputOrDefault(context, name, "Speed", context.BuildFloat("Speed"), speed))
    {
      return;
    }
    /* Empty Surface: the tree's own object; empty Cursor: none. A link that did not compile is
     * an error, not those. */
    if (!InputOrDefault(
            context, name, "Surface", context.BuildOptionalObject("Surface"), surface) ||
        !InputOrDefault(context, name, "Cursor", context.BuildOptionalObject("Cursor"), cursor))
    {
      return;
    }
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Player Pointer flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetPlayerPointer;
    instruction.source_ref_index = context.MakeSourceRef(aim ? "Reach" : "Surface");
    instruction.value_expr_index = surface;
    instruction.secondary_value_expr_index = cursor;
    instruction.int_expr_index = user;
    instruction.bool_expr_index = enabled;
    instruction.string_expr_index = move_action;
    instruction.float_expr_index = speed;
    instruction.secondary_float_expr_index = reach;
    instruction.secondary_string_expr_index = press_action;
    instruction.bool_value = aim;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileTakeUIControlNode(CompilerContext &context)
{
  const char *name = "Take UI Control";
  uint32_t user, take, target;
  if (!InputOrDefault(context, name, "User", context.BuildInt("User"), user) ||
      !InputOrDefault(context, name, "Take", context.BuildBool("Take"), take) ||
      !InputOrDefault(context, name, "Object", context.BuildOptionalObject("Object"), target))
  {
    return;
  }
  const std::optional<FlowEventsResult> flows = context.ResolveRequiredPrimaryFlow(
      "Take UI Control flow must be driven by an event or branch");
  if (!flows) {
    return;
  }
  const UICommandCompletionOutputs completion = AddUICommandCompletionOutputs(
      context, "Done", nullptr);
  for (const FlowPath &path : flows->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::TakeUIControl;
    instruction.source_ref_index = context.MakeSourceRef("Object");
    instruction.value_expr_index = target;
    instruction.int_expr_index = user;
    instruction.bool_expr_index = take;
    instruction.property_ref_index = completion.completed;
    AppendFlowInstruction(context.program, path, instruction);
  }
  if (context.OutputFeedsActiveNode("Done")) {
    CachePrecompiledRoutes(context, "Done", ContinuationPaths(*flows, completion.succeeded));
  }
  if (context.OutputFeedsActiveNode("Failed")) {
    CachePrecompiledRoutes(context, "Failed", ContinuationPaths(*flows, completion.failed));
  }
}

void CompileSetPlayerGamepadNode(CompilerContext &context)
{
  uint32_t user_expr;
  uint32_t gamepad_expr;
  if (!InputOrDefault(context, "Set Player Gamepad", "User", context.BuildInt("User"), user_expr) ||
      !InputOrDefault(
          context, "Set Player Gamepad", "Gamepad", context.BuildInt("Gamepad"), gamepad_expr))
  {
    return;
  }
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Player Gamepad flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetPlayerGamepad;
    instruction.source_ref_index = context.MakeSourceRef("User");
    instruction.int_expr_index = user_expr;
    instruction.secondary_int_expr_index = gamepad_expr;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

/** Both outputs read the open menu that the tree's object belongs to (main thread only). */
void CompileGetMenuPlayerNode(CompilerContext &context)
{
  if (context.OutputFeedsActiveNode("User")) {
    LN_IntExpression expression;
    expression.kind = LN_IntExpressionKind::MenuPlayerUser;
    context.CacheIntOutputPin("User", context.program.AddIntExpression(expression));
  }
  if (context.OutputFeedsActiveNode("Owner")) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::MenuPlayerOwner;
    context.CacheValueOutputPin("Owner", context.program.AddValueExpression(expression));
  }
}

void CompileGetPlayerGamepadNode(CompilerContext &context)
{
  uint32_t user_expr;
  if (!InputOrDefault(context, "Get Player Gamepad", "User", context.BuildInt("User"), user_expr))
  {
    return;
  }
  if (context.OutputFeedsActiveNode("Gamepad")) {
    LN_IntExpression expression;
    expression.kind = LN_IntExpressionKind::PlayerGamepad;
    expression.input0 = user_expr;
    context.CacheIntOutputPin("Gamepad", context.program.AddIntExpression(expression));
  }
  const std::pair<const char *, LN_BoolExpressionKind> bool_outputs[] = {
      {"Connected", LN_BoolExpressionKind::PlayerGamepadConnected},
      {"Joined", LN_BoolExpressionKind::PlayerGamepadJoined},
  };
  for (const auto &[name, kind] : bool_outputs) {
    if (context.OutputFeedsActiveNode(name)) {
      LN_BoolExpression expression;
      expression.kind = kind;
      expression.input0 = user_expr;
      context.CacheBoolOutputPin(name, context.program.AddBoolExpression(expression));
    }
  }
}

/** Every output reads the scene's player views when the tree runs (main thread only). */
void CompileGetPlayerViewNode(CompilerContext &context)
{
  uint32_t user_expr;
  if (!InputOrDefault(context, "Get Player View", "User", context.BuildInt("User"), user_expr)) {
    return;
  }
  if (context.OutputFeedsActiveNode("Camera")) {
    LN_ValueExpression expression;
    expression.kind = LN_ValueExpressionKind::PlayerViewCamera;
    expression.input0 = user_expr;
    context.CacheValueOutputPin("Camera", context.program.AddValueExpression(expression));
  }
  if (context.OutputFeedsActiveNode("Shown")) {
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::PlayerViewShown;
    expression.input0 = user_expr;
    context.CacheBoolOutputPin("Shown", context.program.AddBoolExpression(expression));
  }
  const char *const area_outputs[] = {"Left", "Top", "Width", "Height"};
  for (uint8_t component = 0; component < 4; component++) {
    if (context.OutputFeedsActiveNode(area_outputs[component])) {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::PlayerViewArea;
      expression.input0 = user_expr;
      expression.component_index = component;
      context.CacheFloatOutputPin(area_outputs[component],
                                  context.program.AddFloatExpression(expression));
    }
  }
  if (context.OutputFeedsActiveNode("Views")) {
    LN_IntExpression expression;
    expression.kind = LN_IntExpressionKind::PlayerViewCount;
    context.CacheIntOutputPin("Views", context.program.AddIntExpression(expression));
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
  CompileCameraFloatCommand(context,
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Light Power flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Resolution flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set VSync flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t mode_expr = AddConstantIntExpression(context.program,
                                                      int32_t(context.node.custom1));
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
      context, "Property", "Name", "Toggle Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Toggle Property flow must be driven by an event or branch");
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
    instruction.source_ref_index = context.MakeSourceRef("Property");
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::Bool;
    /* Negates the live value when the command flushes (see LN_PropertyModifyOp). */
    instruction.int_value = int32_t(LN_PropertyModifyOp::Toggle);
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

static std::optional<LN_PropertyModifyOp> ModifyPropertyOp(CompilerContext &context)
{
  switch (context.node.custom1) {
    case blender::NODE_MATH_ADD:
      return LN_PropertyModifyOp::Add;
    case blender::NODE_MATH_SUBTRACT:
      return LN_PropertyModifyOp::Subtract;
    case blender::NODE_MATH_MULTIPLY:
      return LN_PropertyModifyOp::Multiply;
    case blender::NODE_MATH_DIVIDE:
      return LN_PropertyModifyOp::Divide;
    case blender::NODE_MATH_POWER:
      return LN_PropertyModifyOp::Power;
    case blender::NODE_MATH_MODULO:
      return LN_PropertyModifyOp::Modulo;
    case blender::NODE_MATH_FLOOR:
      return LN_PropertyModifyOp::FloorDivide;
    default:
      context.AddError("Operation", "Modify Property uses an unsupported operation");
      return std::nullopt;
  }
}

void CompileModifyPropertyNode(CompilerContext &context)
{
  const char *node_name = "Modify Property";
  const bool clamped = (context.node.custom2 & modify_property_clamp) != 0;

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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      std::string(node_name) + " flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const std::optional<LN_PropertyModifyOp> op = ModifyPropertyOp(context);
  if (!op) {
    return;
  }
  std::optional<uint32_t> min_expr;
  std::optional<uint32_t> max_expr;
  if (clamped) {
    min_expr = context.BuildFloat("Min");
    max_expr = context.BuildFloat("Max");
    if (!min_expr || !max_expr) {
      context.AddError("Min", "Modify Property with Clamp requires min and max inputs");
      return;
    }
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
    /* The operand; the command applies the operation to the live value when it flushes. */
    instruction.float_expr_index = *delta_expr;
    instruction.int_value = int32_t(*op);
    if (clamped) {
      instruction.secondary_float_expr_index = *min_expr;
      instruction.tertiary_float_expr_index = *max_expr;
    }
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileInventoryNode(CompilerContext &context)
{
  static const char *socket_names[LN_INVENTORY_INPUT_COUNT] = {"Item",
                                                               "Amount",
                                                               "Max Stack",
                                                               "Stack",
                                                               "Capacity",
                                                               "Slot",
                                                               "Other Slot",
                                                               "By Count",
                                                               "Descending",
                                                               "Recipes",
                                                               "Recipe",
                                                               "Library",
                                                               "Swap",
                                                               "To Object",
                                                               "To Property",
                                                               "Keep Places",
                                                               "Override"};
  const std::optional<uint32_t> property_expr = context.BuildString("Property");
  if (!property_expr) {
    context.AddError("Property", "Inventory needs the name of the inventory game property");
    return;
  }
  const uint32_t object_expr = context.BuildOptionalObject("Object").value_or(LN_INVALID_INDEX);
  /* The operation and its inputs; the instruction runs it on the property's list. */
  LN_ValueExpression edit;
  edit.kind = LN_ValueExpressionKind::InventoryEdit;
  edit.value = MakeDefaultValue(LN_ValueType::Int);
  edit.value.int_value = std::clamp<int32_t>(
      context.node.custom1, 0, int32_t(LN_InventoryOperation::Craft));
  const LN_InventoryOperation operation = LN_InventoryOperation(edit.value.int_value);
  const bool uses_library = ELEM(operation,
                                 LN_InventoryOperation::AddItem,
                                 LN_InventoryOperation::MoveToSlot,
                                 LN_InventoryOperation::Craft);
  const blender::bNodeSocket *library = FindInputSocket(context.node, "Library");
  const bool has_library = uses_library &&
                           (context.InputIsLinked("Library") ||
                            (library != nullptr && library->type == blender::SOCK_COLLECTION &&
                             library->default_value != nullptr &&
                             static_cast<const blender::bNodeSocketValueCollection *>(
                                 library->default_value)
                                     ->value != nullptr));
  for (uint32_t input = 0; input < LN_INVENTORY_INPUT_COUNT; ++input) {
    const std::optional<uint32_t> input_expr =
        input == LN_INVENTORY_INPUT_LIBRARY && !has_library ?
            std::nullopt :
            context.BuildValueExpression(socket_names[input]);
    edit.input_indices.push_back(input_expr.value_or(LN_INVALID_INDEX));
  }
  /* An empty Library, or one that finds no collection, uses the scene's Item Library. */
  if (uses_library) {
    edit.input1 = LN_INVENTORY_SCENE_LIBRARY;
  }
  const uint32_t edit_expr = context.program.AddValueExpression(edit);
  const uint32_t result_ref = AddTreePropertyRef(context.program,
                                                 InventoryResultPropertyName(context.node),
                                                 LN_ValueType::Dict,
                                                 MakeDefaultValue(LN_ValueType::Dict));
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Inventory must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  /* The outputs read what the instruction keeps in the tree property. */
  const auto cache_output = [&](const char *name, const LN_ValueExpressionKind kind) {
    LN_ValueExpression expression;
    expression.kind = kind;
    expression.property_ref_index = result_ref;
    context.CacheValueOutputPin(name, context.program.AddValueExpression(expression));
  };
  cache_output("Inventory", LN_ValueExpressionKind::InventoryEdit);
  cache_output("Leftover", LN_ValueExpressionKind::InventoryAmount);
  cache_output("Removed", LN_ValueExpressionKind::InventoryChanged);
  cache_output("Made", LN_ValueExpressionKind::InventoryMade);
  cache_output("Outcome", LN_ValueExpressionKind::InventoryOutcome);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::InventoryEdit;
    instruction.source_ref_index = context.MakeSourceRef("Property");
    instruction.value_expr_index = object_expr;
    instruction.string_expr_index = *property_expr;
    instruction.secondary_value_expr_index = edit_expr;
    instruction.property_ref_index = result_ref;
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileDropItemNode(CompilerContext &context)
{
  const std::optional<uint32_t> item_expr = context.BuildValueExpression("Item");
  const std::optional<uint32_t> amount_expr = context.BuildInt("Amount");
  const std::optional<uint32_t> position_expr = context.BuildVector("Position");
  if (!item_expr || !amount_expr || !position_expr) {
    context.AddError("Item", "Drop Item needs an item, an amount and a position");
    return;
  }
  /* An empty Library uses the scene's Item Library at runtime. */
  const blender::bNodeSocket *library = FindInputSocket(context.node, "Library");
  const bool has_library = context.InputIsLinked("Library") ||
                           (library != nullptr && library->type == blender::SOCK_COLLECTION &&
                            library->default_value != nullptr &&
                            static_cast<const blender::bNodeSocketValueCollection *>(
                                library->default_value)
                                    ->value != nullptr);
  const uint32_t library_expr = has_library ?
                                    context.BuildValueExpression("Library").value_or(
                                        LN_INVALID_INDEX) :
                                    LN_INVALID_INDEX;
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Drop Item must be driven by an event or branch");
  if (!flow_events) {
    return;
  }
  /* Done runs only when the drop is made or queued. */
  std::vector<uint32_t> done_outputs;
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::DropItem;
    instruction.source_ref_index = context.MakeSourceRef("Item");
    instruction.secondary_value_expr_index = *item_expr;
    instruction.int_expr_index = *amount_expr;
    instruction.vector_expr_index = *position_expr;
    instruction.value_expr_index = library_expr;
    const uint32_t instruction_index = AppendFlowInstruction(context.program, path, instruction);
    done_outputs.push_back(AddInstructionPulseExpression(context.program,
                                                         LN_BoolExpressionKind::InstructionExecuted,
                                                         path.event,
                                                         instruction_index));
  }
  context.CacheBoolOutputPin("Done", CombineBoolExpressionsWithOr(context.program, done_outputs));
}

void CompileAddObjectNode(CompilerContext &context)
{
  const std::optional<uint32_t> source_object_expr = context.BuildValueExpression("Object to Add");
  const std::optional<uint32_t> copy_transform_expr = context.BuildOptionalObject(
      "Copy Transform");
  const std::optional<uint32_t> life_expr = context.BuildFloat("Life");
  const std::optional<uint32_t> full_copy_expr = context.BuildBool("Full Copy");
  const std::optional<uint32_t> own_mesh_expr = context.BuildBool("Own Mesh");
  if (!source_object_expr || !life_expr || !full_copy_expr || !own_mesh_expr) {
    context.AddError("Object to Add",
                     "Add Object requires object, life, full copy and own mesh inputs");
    return;
  }

  uint32_t result_property_ref_index = LN_INVALID_INDEX;
  if (context.OutputIsLinked("Added Object") || context.OutputIsLinked("Object")) {
    result_property_ref_index = AddTreePropertyRef(context.program,
                                                   AddObjectResultPropertyName(context.node),
                                                   LN_ValueType::ObjectRef,
                                                   MakeDefaultValue(LN_ValueType::ObjectRef));
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Add Object flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::AddObject;
    instruction.source_ref_index = context.MakeSourceRef("Object to Add");
    instruction.float_expr_index = *life_expr;
    instruction.bool_expr_index = *full_copy_expr;
    instruction.secondary_bool_expr_index = *own_mesh_expr;
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
  constexpr int settings_initialized = 1 << 14;
  const bool modern = (context.node.custom2 & settings_initialized) != 0;

  if (modern) {
    const int parent_type = context.node.custom1;
    if (!parent_expr || parent_type < int(KX_ParentType::Object) ||
        parent_type > int(KX_ParentType::Bone))
    {
      context.AddError("Parent Object", "Set Parent has invalid modern parenting settings");
      return;
    }

    std::optional<uint32_t> compound_expr;
    std::optional<uint32_t> bone_name_expr;
    if (parent_type == int(KX_ParentType::Object)) {
      compound_expr = context.BuildBool("Compound");
      if (!compound_expr) {
        context.AddError("Compound", "Object parenting requires a Compound input");
        return;
      }
    }
    else {
      bone_name_expr = context.BuildString("Bone Name");
      if (!bone_name_expr) {
        context.AddError("Bone Name", "Bone parenting requires a bone name input");
        return;
      }
    }

    const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
        "Set Parent flow must be driven by an event or branch");
    if (!flow_events) {
      return;
    }
    for (const FlowPath &path : flow_events->paths) {
      LN_Instruction instruction;
      instruction.opcode = LN_OpCode::SetParent;
      instruction.source_ref_index = context.MakeSourceRef("Parent Object");
      instruction.bool_value = true; /* Modern request marker; legacy uses dynamic bool inputs. */
      instruction.int_value = parent_type;
      instruction.secondary_value_expr_index = *parent_expr;
      if (compound_expr) {
        instruction.bool_expr_index = *compound_expr;
      }
      if (child_expr) {
        instruction.value_expr_index = *child_expr;
      }
      if (bone_name_expr) {
        instruction.string_expr_index = *bone_name_expr;
      }
      AppendFlowInstruction(context.program, path, instruction);
    }
    return;
  }

  const std::optional<uint32_t> compound_expr = context.BuildBool("Compound");
  const std::optional<uint32_t> ghost_expr = context.BuildBool("Ghost");
  if (!parent_expr || !compound_expr || !ghost_expr) {
    context.AddError("Parent Object", "Set Parent requires parent, compound, and ghost inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Parent flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Apply Impulse flow must be driven by an event or branch");
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
    context.AddError(
        "Target Position",
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

    done_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionExecuted,
                                      path.event,
                                      instruction_index));
    reached_outputs.push_back(
        AddInstructionPulseExpression(context.program,
                                      LN_BoolExpressionKind::InstructionReached,
                                      path.event,
                                      instruction_index));
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
      context, "Property", "Name", "Set Game Property Int requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildInt("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Int requires an integer value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Game Property Int must be driven by an event or branch");
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
      context, "Property", "Name", "Set Game Property Float requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildFloat("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Float requires a float value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Game Property Float must be driven by an event or branch");
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
      context, "Property", "Name", "Set Game Property Bool requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildBool("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Game Property Bool requires a boolean value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Game Property Bool must be driven by an event or branch");
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

/** Sets the string game property \a name of the "Object" input (or the owner) to \a socket. */
static void CompileSetStringProperty(CompilerContext &context,
                                     const std::string &name,
                                     const char *socket,
                                     const char *node_name)
{
  std::optional<uint32_t> value_expr = context.BuildString(socket);
  if (!value_expr) {
    if (const std::optional<uint32_t> generic_expr = context.BuildValueExpression(socket)) {
      LN_StringExpression string_expr;
      string_expr.kind = LN_StringExpressionKind::FromGenericValue;
      string_expr.value_expr_index = *generic_expr;
      value_expr = context.program.AddStringExpression(string_expr);
    }
  }
  if (!value_expr) {
    context.AddError(socket, std::string(node_name) + " requires a string value input");
    return;
  }

  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      std::string(node_name) + " must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_Value default_value;
  default_value.type = LN_ValueType::String;
  const uint32_t property_ref_index = AddGamePropertyRef(
      context.program, name, LN_ValueType::String, default_value);
  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::SetGameProperty;
    instruction.source_ref_index = context.MakeSourceRef(socket);
    instruction.property_ref_index = property_ref_index;
    instruction.property_value_type = LN_ValueType::String;
    instruction.string_expr_index = *value_expr;
    if (object_expr) {
      instruction.value_expr_index = *object_expr;
    }
    AppendFlowInstruction(context.program, path, instruction);
  }
}

void CompileSetGamePropertyStringNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context, "Property", "Name", "Set Game Property String requires a constant property name");
  if (name) {
    CompileSetStringProperty(context, *name, "Value", "Set Game Property String");
  }
}

/** Text objects show their "Text" game property. */
void CompileSetTextNode(CompilerContext &context)
{
  CompileSetStringProperty(context, "Text", "Text", "Set Text");
}

void CompileSetTreePropertyNode(CompilerContext &context)
{
  const std::optional<std::string> name = ReadRequiredConstantStringWithFallback(
      context, "Property", "Name", "Set Tree Property requires a constant property name");
  if (!name) {
    return;
  }

  const std::optional<uint32_t> value_expr = context.BuildValueExpression("Value");
  if (!value_expr) {
    context.AddError("Value", "Set Tree Property requires a valid value input");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Tree Property flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t property_ref_index = AddTreePropertyRef(
      context.program, *name, LN_ValueType::Generic, MakeDefaultValue(LN_ValueType::Generic));
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Cursor Position flow must be driven by an event or branch");
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Gamepad Vibration flow must be driven by an event or branch");
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
  if (!body_expr || !head_expr || !invert_expr || !index_expr || !sensitivity_expr ||
      !exponent_expr || !use_cap_x_expr || !cap_x_expr || !use_cap_y_expr || !cap_y_expr ||
      !threshold_expr)
  {
    context.AddError("Index", "Gamepad Look requires object and numeric inputs");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Gamepad Look flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression stick_expr;
  stick_expr.kind = LN_VectorExpressionKind::GamepadStick;
  stick_expr.input0 = *index_expr;
  stick_expr.input1 = *threshold_expr;
  stick_expr.input2 = uint32_t(context.node.custom1 & 1);
  /* The stick as it is (x right, y up): the Inverted input flips it. Left unset, every invert
   * flag was on and up looked down. */
  stick_expr.property_ref_index = 0;
  stick_expr.float_expr_index = AddConstantFloatExpression(context.program, 1.0f);
  stick_expr.float_value = 1.0f;
  const uint32_t stick_expr_index = context.program.AddVectorExpression(stick_expr);

  const MT_Vector3 invert_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *invert_expr);
  const MT_Vector3 cap_x_fallback = VectorExpressionConstantFallback(context.program, *cap_x_expr);
  const MT_Vector3 cap_y_fallback = VectorExpressionConstantFallback(context.program, *cap_y_expr);
  const float sensitivity_fallback = FloatExpressionConstantFallback(context.program,
                                                                     *sensitivity_expr);
  const float exponent_fallback = FloatExpressionConstantFallback(context.program, *exponent_expr);
  const bool use_cap_x_fallback = BoolExpressionConstantFallback(context.program, *use_cap_x_expr);
  const bool use_cap_y_fallback = BoolExpressionConstantFallback(context.program, *use_cap_y_expr);

  for (const FlowPath &path : flow_events->paths) {
    LN_Instruction instruction;
    instruction.opcode = LN_OpCode::GamepadLook;
    instruction.source_ref_index = context.MakeSourceRef("Flow");
    instruction.value_expr_index = *body_expr;
    instruction.property_ref_index = *head_expr;
    instruction.vector_expr_index = stick_expr_index;
    /* Its gamepad's player: the view waits while their menu uses the gamepad. */
    instruction.secondary_int_expr_index = *index_expr;
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Mouse Look flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const MT_Vector3 invert_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *invert_expr);
  const MT_Vector3 cap_x_fallback = VectorExpressionConstantFallback(context.program, *cap_x_expr);
  const MT_Vector3 cap_y_fallback = VectorExpressionConstantFallback(context.program, *cap_y_expr);
  const float sensitivity_fallback = FloatExpressionConstantFallback(context.program,
                                                                     *sensitivity_expr);
  const float smoothing_fallback = FloatExpressionConstantFallback(context.program,
                                                                   *smoothing_expr);
  const bool use_cap_x_fallback = BoolExpressionConstantFallback(context.program, *use_cap_x_expr);
  const bool use_cap_y_fallback = BoolExpressionConstantFallback(context.program, *use_cap_y_expr);

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
                                           AddAndBoolExpression(
                                               program, path.bool_guard_expr_index, guard_expr);
  return guarded_path;
}

void CompileCharacterJumpNode(CompilerContext &context)
{
  const std::optional<uint32_t> object_expr = context.BuildOptionalObject("Object");
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Jump flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Gravity flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Jump Force flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Max Jumps flow must be driven by an event or branch");
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Walk flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t local_expr = AddConstantBoolExpression(context.program,
                                                        context.node.custom1 != 0);
  const MT_Vector3 vector_fallback = VectorExpressionConstantFallback(context.program,
                                                                      *vector_expr);
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
  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Velocity flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  const uint32_t local_expr = AddConstantBoolExpression(context.program,
                                                        context.node.custom1 != 0);
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      flow_error);
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

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Vehicle Control flow must be driven by an event or branch");
  if (!flow_events) {
    return;
  }

  LN_VectorExpression control_expr;
  control_expr.kind = LN_VectorExpressionKind::Combine;
  control_expr.input0 = *throttle_expr;
  control_expr.input1 = *brake_expr;
  control_expr.input2 = *handbrake_expr;
  control_expr.vector_value = MT_Vector3(
      FloatExpressionConstantFallback(context.program, *throttle_expr),
      FloatExpressionConstantFallback(context.program, *brake_expr),
      FloatExpressionConstantFallback(context.program, *handbrake_expr));
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
  if (!wheels_expr || !suspension_enabled_expr || !suspension_value_expr ||
      !stiffness_enabled_expr || !stiffness_value_expr || !damping_enabled_expr ||
      !damping_value_expr || !friction_enabled_expr || !friction_value_expr)
  {
    context.AddError("Vehicle", "Set Vehicle Attributes requires wheels, toggles, and values");
    return;
  }

  const std::optional<FlowEventsResult> flow_events = context.ResolveRequiredPrimaryFlow(
      "Set Vehicle Attributes flow must be driven by an event or branch");
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
      AppendFlowInstruction(
          context.program, GuardFlowPath(context.program, path, spec.enabled_expr), instruction);
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
  FlowPath event_path;
  event_path.event = event;
  if (context.definition.kind == LN_NodeKind::WhilePaused) {
    LN_BoolExpression paused_expression;
    paused_expression.kind = LN_BoolExpressionKind::GamePaused;
    event_path.bool_guard_expr_index = context.program.AddBoolExpression(paused_expression);

    LN_PauseProcessingRequest request;
    request.scope = LN_PauseProcessingScope(
        std::clamp(int(context.node.custom1),
                   int(LN_PauseProcessingScope::Owner),
                   int(LN_PauseProcessingScope::Scene)));
    request.source_ref_index = source_ref_index;

    const char *target_socket = nullptr;
    if (request.scope == LN_PauseProcessingScope::ObjectHierarchy) {
      target_socket = "Object";
    }
    else if (request.scope == LN_PauseProcessingScope::Collection) {
      target_socket = "Collection";
    }

    if (target_socket != nullptr) {
      if (context.InputIsLinked(target_socket)) {
        context.AddError(
            target_socket,
            "While Paused scope targets must be static so they can be resolved before paused "
            "trees are scheduled");
      }
      else if (const std::optional<uint32_t> target_expr =
                   context.BuildValueExpression(target_socket))
      {
        request.target_value_expr_index = *target_expr;
      }
      else {
        context.AddError(target_socket, "While Paused scope target could not be compiled");
      }

      if (request.scope == LN_PauseProcessingScope::Collection) {
        const std::optional<LN_Value> collection = context.ReadValue(
            "Collection", LN_ValueType::CollectionRef);
        if (!collection || !collection->exists ||
            collection->type != LN_ValueType::CollectionRef)
        {
          context.AddError("Collection", "While Paused collection scope requires a collection");
        }
      }
    }
    context.program.AddPauseProcessingRequest(request);
  }
  AppendFlowInstruction(context.program, event_path, instruction);
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

  const std::optional<LN_Value> value = EvaluateOutputValue(
      context.node, *socket, context.node_definitions, context.input_links, context.value_cache);
  if (!value) {
    return;
  }

  LN_Constant constant;
  constant.source_ref_index = context.MakeSourceRef(context.definition.outputs[0].name.c_str());
  constant.value = *value;
  context.program.AddConstant(constant);
}

#define EVENT_HANDLER(kind, id, event) \
  {{LN_NodeKind::kind, {CompileHandlerKind::EventSource, false, false, true}, id}, \
   CompileActionKind::EmitEventSource, \
   FlowResolverKind::EventSource, \
   event, \
   nullptr}

#define CONST_HANDLER(kind, handler_kind, id, flow_kind) \
  {{LN_NodeKind::kind, {CompileHandlerKind::handler_kind, false, false, true}, id}, \
   CompileActionKind::EmitConstantOutput, \
   flow_kind, \
   LN_Event::OnInit, \
   nullptr}

#define EXPRESSION_HANDLER(kind, handler_kind, id, flow_kind) \
  {{LN_NodeKind::kind, {CompileHandlerKind::handler_kind, false, false, true}, id}, \
   CompileActionKind::Custom, \
   flow_kind, \
   LN_Event::OnInit, \
   CompileExpressionOutputsNode}

#define CHECKED_EXPRESSION_HANDLER(kind, handler_kind, id, flow_kind, fn) \
  {{LN_NodeKind::kind, {CompileHandlerKind::handler_kind, false, false, true}, id}, \
   CompileActionKind::Custom, \
   flow_kind, \
   LN_Event::OnInit, \
   fn}

#define COMMAND_HANDLER(kind, id, fn) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Command, true, true, true}, id}, \
   CompileActionKind::Custom, \
   FlowResolverKind::Invalid, \
   LN_Event::OnInit, \
   fn}

#define UNSUPPORTED_COMMAND_HANDLER(kind, id) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Command, true, true, false}, id}, \
   CompileActionKind::Custom, \
   FlowResolverKind::Invalid, \
   LN_Event::OnInit, \
   CompileUnsupportedCommandNode}

#define DATA_CONTAINER_HANDLER(kind, id) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id}, \
   CompileActionKind::Custom, \
   FlowResolverKind::Invalid, \
   LN_Event::OnInit, \
   CompileDataContainerNode}

#define DATA_ACTION_HANDLER(kind, id) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id}, \
   CompileActionKind::Custom, \
   FlowResolverKind::FlowConditionRoute, \
   LN_Event::OnInit, \
   CompileDataContainerNode}

#define CUSTOM_EXPR_HANDLER(kind, id, flow_kind, fn) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id}, \
   CompileActionKind::Custom, \
   flow_kind, \
   LN_Event::OnInit, \
   fn}

#define SNAPSHOT_TRANSFORM_HANDLER(kind, id) \
  {{LN_NodeKind::kind, {CompileHandlerKind::Expression, false, false, true}, id}, \
   CompileActionKind::Custom, \
   FlowResolverKind::Invalid, \
   LN_Event::OnInit, \
   CompileSnapshotTransformNode}

static constexpr InternalCompileHandler internal_compile_handlers[] = {
    EVENT_HANDLER(EventOnInit, "event.init", LN_Event::OnInit),
    EVENT_HANDLER(EventOnUpdate, "event.update", LN_Event::OnUpdate),
    EVENT_HANDLER(EventOnFixedUpdate, "event.fixed_update", LN_Event::OnFixedUpdate),
    EVENT_HANDLER(WhilePaused, "event.while_paused", LN_Event::OnUpdate),
    CUSTOM_EXPR_HANDLER(OnGameLoaded,
                        "event.game_loaded",
                        FlowResolverKind::PollingEventRoute,
                        CompileGameLoadEventNode),
    CUSTOM_EXPR_HANDLER(OnPlayerJoin,
                        "event.player_join",
                        FlowResolverKind::PollingEventRoute,
                        CompilePlayerJoinEventNode),
    CONST_HANDLER(ValueBool, ValueExpression, "value.bool", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueInt, ValueExpression, "value.int", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueFloat, ValueExpression, "value.float", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueString, ValueExpression, "value.string", FlowResolverKind::Invalid),
    CONST_HANDLER(StringOperation, ValueExpression, "string.operation", FlowResolverKind::Invalid),
    CONST_HANDLER(FormattedString, ValueExpression, "string.format", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueColor, ValueExpression, "value.color", FlowResolverKind::Invalid),
    CONST_HANDLER(ColorRGB, ValueExpression, "color.rgb", FlowResolverKind::Invalid),
    CONST_HANDLER(ColorRGBA, ValueExpression, "color.rgba", FlowResolverKind::Invalid),
    CONST_HANDLER(ValueVector, ValueExpression, "value.vector", FlowResolverKind::Invalid),
    CONST_HANDLER(Euler, ValueExpression, "vector.euler", FlowResolverKind::Invalid),
    CONST_HANDLER(
        SeparateEuler, ValueExpression, "rotation.separate_euler", FlowResolverKind::Invalid),
    CONST_HANDLER(CombineXY, ValueExpression, "vector.xy", FlowResolverKind::Invalid),
    CONST_HANDLER(CombineXYZ, ValueExpression, "vector.xyz", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(
        CombineXYZW, "vector.xyzw", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        ResizeVector, "vector.resize", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        XYZToMatrix, "matrix.from_xyz", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        MatrixToXYZ, "matrix.to_xyz", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        VectorRotate, "vector.rotate", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(VectorToRotation,
                        "vector.to_rotation",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(BoneRotationToObjectRotation,
                        "rotation.bone_to_object",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        FilePath, "path.file", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        GetSound, "sound.get", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        GetImage, "image.get", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        GetObjectID, "object.id", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetAxisVector,
                        "object.axis_vector",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        JoinPath, "path.join", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetMasterFolder,
                        "path.master_folder",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CONST_HANDLER(SeparateXY, ValueExpression, "vector.separate_xy", FlowResolverKind::Invalid),
    CONST_HANDLER(SeparateXYZ, ValueExpression, "vector.separate_xyz", FlowResolverKind::Invalid),
    CONST_HANDLER(InvertValue, ValueExpression, "value.invert", FlowResolverKind::Invalid),
    CONST_HANDLER(ClampValue, ValueExpression, "value.clamp", FlowResolverKind::Invalid),
    CONST_HANDLER(MapRange, ValueExpression, "value.map", FlowResolverKind::Invalid),
    CONST_HANDLER(Threshold, ValueExpression, "value.threshold", FlowResolverKind::Invalid),
    CONST_HANDLER(
        RangedThreshold, ValueExpression, "value.ranged_threshold", FlowResolverKind::Invalid),
    CONST_HANDLER(WithinRange, ValueExpression, "value.within_range", FlowResolverKind::Invalid),
    CONST_HANDLER(Math, ValueExpression, "math.float", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(OnNextFrame,
                        "flow.next_frame",
                        FlowResolverKind::PollingEventRoute,
                        CompileOnNextFrameNode),
    CUSTOM_EXPR_HANDLER(Once, "flow.once", FlowResolverKind::PrecompiledRoute, CompileOnceNode),
    CUSTOM_EXPR_HANDLER(BooleanEdge,
                        "flow.boolean_edge",
                        FlowResolverKind::PollingEventRoute,
                        CompileBooleanEdgeNode),
    EXPRESSION_HANDLER(Branch, Expression, "flow.branch", FlowResolverKind::Branch),
    CUSTOM_EXPR_HANDLER(
        FlowGate, "flow.gate", FlowResolverKind::PrecompiledRoute, CompileFlowGateNode),
    CUSTOM_EXPR_HANDLER(
        AnyFlow, "flow.any", FlowResolverKind::PrecompiledRoute, CompileAnyFlowNode),
    CUSTOM_EXPR_HANDLER(
        Sequence, "flow.sequence", FlowResolverKind::PrecompiledRoute, CompileSequenceNode),
    CUSTOM_EXPR_HANDLER(SwitchFlowInt,
                        "flow.switch_int",
                        FlowResolverKind::PrecompiledRoute,
                        CompileSwitchFlowIntNode),
    CUSTOM_EXPR_HANDLER(SwitchFlowString,
                        "flow.switch_string",
                        FlowResolverKind::PrecompiledRoute,
                        CompileSwitchFlowStringNode),
    CUSTOM_EXPR_HANDLER(
        FlowRouter, "flow.router", FlowResolverKind::PrecompiledRoute, CompileFlowRouterNode),
    CUSTOM_EXPR_HANDLER(
        FlipFlop, "flow.flip_flop", FlowResolverKind::PrecompiledRoute, CompileFlipFlopNode),
    CUSTOM_EXPR_HANDLER(RepeatLimit,
                        "flow.repeat_limit",
                        FlowResolverKind::PrecompiledRoute,
                        CompileRepeatLimitNode),
    CUSTOM_EXPR_HANDLER(
        BreakLoop, "flow.break_loop", FlowResolverKind::Invalid, CompileBreakLoopNode),
    EXPRESSION_HANDLER(Gate, Expression, "logic.boolean_gate", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        ValueChanged, Expression, "flow.value_changed", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(
        ValueChangedTo, Expression, "flow.value_changed_to", FlowResolverKind::PollingEventRoute),
    CUSTOM_EXPR_HANDLER(
        Delay, "flow.delay", FlowResolverKind::LatentCompletionRoute, CompileDelayNode),
    CUSTOM_EXPR_HANDLER(
        Timer, "flow.timer", FlowResolverKind::LatentCompletionRoute, CompileTimerNode),
    CUSTOM_EXPR_HANDLER(
        Pulsify, "flow.pulsify", FlowResolverKind::LatentCompletionRoute, CompilePulsifyNode),
    CUSTOM_EXPR_HANDLER(
        Barrier, "flow.barrier", FlowResolverKind::LatentCompletionRoute, CompileBarrierNode),
    CUSTOM_EXPR_HANDLER(
        Cooldown, "flow.cooldown", FlowResolverKind::PrecompiledRoute, CompileCooldownNode),
    {{LN_NodeKind::Loop, {CompileHandlerKind::Expression, false, false, true}, "flow.loop"},
     CompileActionKind::Custom,
     FlowResolverKind::Loop,
     LN_Event::OnInit,
     CompileLoopNode},
    {{LN_NodeKind::LoopFromList,
      {CompileHandlerKind::Expression, false, false, true},
      "flow.loop_from_list"},
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
    DATA_ACTION_HANDLER(DictAddValue, "data.dict_add_value"),
    COMMAND_HANDLER(Inventory, "data.inventory", CompileInventoryNode),
    COMMAND_HANDLER(DropItem, "data.drop_item", CompileDropItemNode),
    DATA_CONTAINER_HANDLER(ItemCount, "data.item_count"),
    DATA_CONTAINER_HANDLER(GetItemInfo, "data.get_item_info"),
    DATA_CONTAINER_HANDLER(GetRecipeInfo, "data.get_recipe_info"),
    DATA_CONTAINER_HANDLER(FindRecipe, "data.find_recipe"),
    DATA_CONTAINER_HANDLER(ListSort, "data.list_sort"),
    DATA_CONTAINER_HANDLER(ListFindIndex, "data.list_find_index"),
    COMMAND_HANDLER(SetText, "ui.set_text", CompileSetTextNode),
    DATA_ACTION_HANDLER(DictRemoveKey, "data.dict_remove_key"),
    DATA_CONTAINER_HANDLER(ListFromItems, "data.list_from_items"),
    EXPRESSION_HANDLER(
        GetBoneTailWorld, Expression, "animation.bone_tail_world", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GetBoneLength, Expression, "animation.bone_length", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GetBoneCenterWorld, Expression, "animation.bone_center_world", FlowResolverKind::Invalid),
    DATA_CONTAINER_HANDLER(ListContains, "data.list_contains"),
    EXPRESSION_HANDLER(LimitRange, Expression, "value.limit", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(RandomValue, Expression, "value.random", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(
        Typecast, "value.typecast", FlowResolverKind::Invalid, CompileTypecastNode),
    CUSTOM_EXPR_HANDLER(
        ValueValid, "value.valid", FlowResolverKind::Invalid, CompileValueValidNode),
    CUSTOM_EXPR_HANDLER(MixValue, "value.mix", FlowResolverKind::Invalid, CompileMixValueNode),
    CUSTOM_EXPR_HANDLER(
        FloatCurve, "math.float_curve", FlowResolverKind::Invalid, CompileFloatCurveNode),
    EXPRESSION_HANDLER(ValueSwitch, Expression, "value.switch", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(ValueSwitchList,
                        "value.switch_list",
                        FlowResolverKind::Invalid,
                        CompileValueSwitchListNode),
    CUSTOM_EXPR_HANDLER(ValueSwitchListCompare,
                        "value.switch_list_compare",
                        FlowResolverKind::Invalid,
                        CompileValueSwitchListCompareNode),
    CHECKED_EXPRESSION_HANDLER(StoreValue,
                               Expression,
                               "value.store",
                               FlowResolverKind::FlowConditionRoute,
                               CompileStoreValueNode),
    CUSTOM_EXPR_HANDLER(
        HasProperty, "property.exists", FlowResolverKind::Invalid, CompileGamePropertyReadNode),
    EXPRESSION_HANDLER(IsNone, Expression, "value.is_none", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(NotNone, Expression, "value.not_none", FlowResolverKind::Invalid),
    CHECKED_EXPRESSION_HANDLER(KeyboardKey,
                               Expression,
                               "input.key",
                               FlowResolverKind::PollingEventRoute,
                               CompileKeyboardKeyNode),
    EXPRESSION_HANDLER(
        KeyboardActive, Expression, "input.keyboard_active", FlowResolverKind::Invalid),
    CHECKED_EXPRESSION_HANDLER(
        KeyCode, ValueExpression, "input.key_code", FlowResolverKind::Invalid, CompileKeyCodeNode),
    EXPRESSION_HANDLER(KeyLogger, Expression, "input.key_logger", FlowResolverKind::Invalid),
    CHECKED_EXPRESSION_HANDLER(MouseButton,
                               Expression,
                               "input.mouse",
                               FlowResolverKind::PollingEventRoute,
                               CompileMouseButtonNode),
    EXPRESSION_HANDLER(
        MouseMoved, Expression, "input.mouse_moved", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(
        MouseWheel, Expression, "input.mouse_wheel", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(
        MouseOver, Expression, "input.mouse_over", FlowResolverKind::PollingEventRoute),
    CUSTOM_EXPR_HANDLER(ObjectScrollView,
                        "input.object_scroll_view",
                        FlowResolverKind::PollingEventRoute,
                        CompileObjectScrollViewNode),
    CUSTOM_EXPR_HANDLER(ObjectGridView,
                        "input.object_grid_view",
                        FlowResolverKind::PollingEventRoute,
                        CompileObjectGridViewNode),
    CUSTOM_EXPR_HANDLER(OnUIEvent,
                        "input.on_ui_event",
                        FlowResolverKind::PollingEventRoute,
                        CompileOnUIEventNode),
    CUSTOM_EXPR_HANDLER(OnUISound,
                        "input.on_ui_sound",
                        FlowResolverKind::PollingEventRoute,
                        CompileOnUISoundNode),
    CUSTOM_EXPR_HANDLER(InputAction,
                        "input.input_action",
                        FlowResolverKind::PollingEventRoute,
                        CompileInputActionNode),
    CUSTOM_EXPR_HANDLER(GetInputBinding,
                        "input.input_binding",
                        FlowResolverKind::Invalid,
                        CompileGetInputBindingNode),
    COMMAND_HANDLER(RebindInputAction, "input.rebind_input_action", CompileRebindInputActionNode),
    COMMAND_HANDLER(InputMap, "input.input_map", CompileInputMapNode),
    CUSTOM_EXPR_HANDLER(
        GetUIFocus, "input.ui_focus", FlowResolverKind::Invalid, CompileGetUIFocusNode),
    EXPRESSION_HANDLER(
        CursorPosition, ValueExpression, "input.cursor_position", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        CursorMovement, ValueExpression, "input.cursor_movement", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GamepadActive, Expression, "input.gamepad_active", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GamepadButton, Expression, "input.gamepad_button", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(
        GamepadStick, ValueExpression, "input.gamepad_stick", FlowResolverKind::Invalid),
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
    EXPRESSION_HANDLER(
        GetActiveCamera, ValueExpression, "scene.active_camera", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        WorldToScreen, Expression, "scene.world_to_screen", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        ScreenToWorld, Expression, "scene.screen_to_world", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetFullscreen, Expression, "render.fullscreen", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetResolution, Expression, "render.resolution", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(GetVSync, Expression, "render.vsync", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GetCollisionGroup, Expression, "physics.collision_group", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        GetCharacterInfo, Expression, "physics.character_info", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(GetGamePropertyInt,
                        "property.get_int",
                        FlowResolverKind::Invalid,
                        CompileGamePropertyReadNode),
    CUSTOM_EXPR_HANDLER(GetGamePropertyFloat,
                        "property.get_float",
                        FlowResolverKind::Invalid,
                        CompileGamePropertyReadNode),
    CUSTOM_EXPR_HANDLER(GetGamePropertyBool,
                        "property.get_bool",
                        FlowResolverKind::Invalid,
                        CompileGamePropertyReadNode),
    CUSTOM_EXPR_HANDLER(GetGamePropertyString,
                        "property.get_string",
                        FlowResolverKind::Invalid,
                        CompileGamePropertyReadNode),
    CHECKED_EXPRESSION_HANDLER(GetTreeProperty,
                               ValueExpression,
                               "property.get_tree",
                               FlowResolverKind::Invalid,
                               CompileGetTreePropertyNode),
    EXPRESSION_HANDLER(
        GetObjectAttribute, ValueExpression, "object.get_attribute", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(GetRigidBodyAttribute,
                        "physics.get_rigid_body_attribute",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetDistance,
                        "object.distance_between",
                        FlowResolverKind::Invalid,
                        CompileDistanceBetweenNode),
    CUSTOM_EXPR_HANDLER(GetAverageCenterPosition,
                        "object.get_average_center_position",
                        FlowResolverKind::Invalid,
                        CompileAverageCenterPositionNode),
    CUSTOM_EXPR_HANDLER(PredictMotion,
                        "object.predict_motion",
                        FlowResolverKind::PrecompiledRoute,
                        CompilePredictMotionNode),
    CUSTOM_EXPR_HANDLER(EvaluateProperty,
                        "object.evaluate_property",
                        FlowResolverKind::Invalid,
                        CompileEvaluatePropertyNode),
    CUSTOM_EXPR_HANDLER(GetGameProperty,
                        "property.get_any",
                        FlowResolverKind::Invalid,
                        CompileGetGamePropertyNode),
    EXPRESSION_HANDLER(
        GetBoneHeadWorld, Expression, "animation.bone_head_world", FlowResolverKind::Invalid),
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
    COMMAND_HANDLER(SetTreeProperty, "property.set_tree", CompileSetTreePropertyNode),
    COMMAND_HANDLER(ToggleTreeProperty, "property.toggle_tree", CompileToggleTreePropertyNode),
    COMMAND_HANDLER(RemoveObject, "object.remove", CompileRemoveObjectNode),
    COMMAND_HANDLER(AddObject, "object.add", CompileAddObjectNode),
    COMMAND_HANDLER(SetParent, "object.set_parent", CompileSetParentNode),
    COMMAND_HANDLER(RemoveParent, "object.remove_parent", CompileRemoveParentNode),
    COMMAND_HANDLER(SetGravity, "scene.set_gravity", CompileSetGravityNode),
    COMMAND_HANDLER(SetTimescale, "scene.set_timescale", CompileSetTimescaleNode),
    COMMAND_HANDLER(SetGamePaused, "game.set_paused", CompileSetGamePausedNode),
    COMMAND_HANDLER(SetCamera, "scene.set_camera", CompileSetCameraNode),
    COMMAND_HANDLER(SetSplitScreen, "scene.set_split_screen", CompileSetSplitScreenNode),
    COMMAND_HANDLER(SetPlayerGamepad, "input.set_player_gamepad", CompileSetPlayerGamepadNode),
    CUSTOM_EXPR_HANDLER(GetPlayerGamepad,
                        "input.player_gamepad",
                        FlowResolverKind::Invalid,
                        CompileGetPlayerGamepadNode),
    CUSTOM_EXPR_HANDLER(GetMenuPlayer,
                        "ui.menu_player",
                        FlowResolverKind::Invalid,
                        CompileGetMenuPlayerNode),
    COMMAND_HANDLER(
        SetCameraViewport, "scene.set_camera_viewport", CompileSetCameraViewportNode),
    COMMAND_HANDLER(
        SetCameraVisibility, "scene.set_camera_visibility", CompileSetCameraVisibilityNode),
    CUSTOM_EXPR_HANDLER(GetPlayerView,
                        "scene.player_view",
                        FlowResolverKind::Invalid,
                        CompileGetPlayerViewNode),
    COMMAND_HANDLER(SetCameraFov, "scene.set_camera_fov", CompileSetCameraFovNode),
    COMMAND_HANDLER(
        SetCameraOrthoScale, "scene.set_camera_ortho_scale", CompileSetCameraOrthoScaleNode),
    COMMAND_HANDLER(SetFullscreen, "render.set_fullscreen", CompileSetFullscreenNode),
    COMMAND_HANDLER(SetResolution, "render.set_resolution", CompileSetResolutionNode),
    COMMAND_HANDLER(SetVSync, "render.set_vsync", CompileSetVSyncNode),
    COMMAND_HANDLER(ShowFramerate, "render.show_framerate", CompileShowFramerateNode),
    COMMAND_HANDLER(ShowProfile, "render.show_profile", CompileShowProfileNode),
    COMMAND_HANDLER(
        SetCollisionGroup, "physics.set_collision_group", CompileSetCollisionGroupNode),
    COMMAND_HANDLER(SetPhysics, "physics.set", CompileSetPhysicsNode),
    COMMAND_HANDLER(SetPhysicsPaused, "physics.set_paused", CompileSetPhysicsPausedNode),
    COMMAND_HANDLER(SetDynamics, "physics.set_dynamics", CompileSetDynamicsNode),
    COMMAND_HANDLER(RebuildCollisionShape,
                    "physics.rebuild_collision_shape",
                    CompileRebuildCollisionShapeNode),
    COMMAND_HANDLER(SetRigidBodyAttribute,
                    "physics.set_rigid_body_attribute",
                    CompileSetRigidBodyAttributeNode),
    COMMAND_HANDLER(CharacterJump, "physics.character_jump", CompileCharacterJumpNode),
    COMMAND_HANDLER(
        SetCharacterGravity, "physics.character_set_gravity", CompileSetCharacterGravityNode),
    COMMAND_HANDLER(SetCharacterJumpSpeed,
                    "physics.character_set_jump_speed",
                    CompileSetCharacterJumpSpeedNode),
    COMMAND_HANDLER(
        SetCharacterMaxJumps, "physics.character_set_max_jumps", CompileSetCharacterMaxJumpsNode),
    COMMAND_HANDLER(SetCharacterWalkDirection,
                    "physics.character_set_walk_direction",
                    CompileSetCharacterWalkDirectionNode),
    COMMAND_HANDLER(
        SetCharacterVelocity, "physics.character_set_velocity", CompileSetCharacterVelocityNode),
    COMMAND_HANDLER(VehicleControl, "physics.vehicle_control", CompileVehicleControlNode),
    COMMAND_HANDLER(VehicleAccelerate, "physics.vehicle_accelerate", CompileVehicleAccelerateNode),
    COMMAND_HANDLER(VehicleBrake, "physics.vehicle_brake", CompileVehicleBrakeNode),
    COMMAND_HANDLER(VehicleSteer, "physics.vehicle_steer", CompileVehicleSteerNode),
    COMMAND_HANDLER(
        VehicleSetAttributes, "physics.vehicle_set_attributes", CompileVehicleSetAttributesNode),
    COMMAND_HANDLER(
        SetCursorVisibility, "input.set_cursor_visibility", CompileSetCursorVisibilityNode),
    COMMAND_HANDLER(SetCursorPosition, "input.set_cursor_position", CompileSetCursorPositionNode),
    COMMAND_HANDLER(GamepadVibration, "input.gamepad_vibration", CompileGamepadVibrationNode),
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
    COMMAND_HANDLER(
        ApplyForceToTarget, "physics.apply_force_to_target", CompileApplyForceToTargetNode),
    COMMAND_HANDLER(ApplyTorque, "physics.apply_torque", CompileApplyTorqueNode),
    COMMAND_HANDLER(SetGamePropertyInt, "property.set_int", CompileSetGamePropertyIntNode),
    COMMAND_HANDLER(SetGamePropertyFloat, "property.set_float", CompileSetGamePropertyFloatNode),
    COMMAND_HANDLER(SetGamePropertyBool, "property.set_bool", CompileSetGamePropertyBoolNode),
    COMMAND_HANDLER(
        SetGamePropertyString, "property.set_string", CompileSetGamePropertyStringNode),
    COMMAND_HANDLER(SetGameProperty, "property.set_any", CompileSetGamePropertyNode),
    COMMAND_HANDLER(Print, "debug.print", CompilePrintNode),
    COMMAND_HANDLER(QuitGame, "game.quit", CompileQuitGameNode),
    COMMAND_HANDLER(RestartGame, "game.restart", CompileRestartGameNode),
    COMMAND_HANDLER(LoadBlendFile, "game.load_blend_file", CompileLoadBlendFileNode),
    COMMAND_HANDLER(PlayAction, "animation.play_action", CompilePlayActionNode),
    COMMAND_HANDLER(StopAction, "animation.stop_action", CompileStopActionNode),
    COMMAND_HANDLER(SetActionFrame, "animation.set_action_frame", CompileSetActionFrameNode),
    COMMAND_HANDLER(
        SetShapeKeyValue, "animation.set_shape_key_value", CompileSetShapeKeyValueNode),
    COMMAND_HANDLER(StopAllSounds, "audio.stop_all_sounds", CompileStopAllSoundsNode),
    COMMAND_HANDLER(PlaySound, "audio.play_sound", CompilePlaySoundNode),
    COMMAND_HANDLER(PlaySound3D, "audio.play_sound_3d", CompilePlaySound3DNode),
    COMMAND_HANDLER(PauseSound, "audio.pause_sound", CompilePauseSoundNode),
    COMMAND_HANDLER(ResumeSound, "audio.resume_sound", CompileResumeSoundNode),
    COMMAND_HANDLER(StopSound, "audio.stop_sound", CompileStopSoundNode),
    COMMAND_HANDLER(SetVolume, "audio.set_volume", CompileSetVolumeNode),
    COMMAND_HANDLER(StartLogicTree, "logic.tree.start", CompileStartLogicTreeNode),
    COMMAND_HANDLER(StopLogicTree, "logic.tree.stop", CompileStopLogicTreeNode),
    COMMAND_HANDLER(RunLogicTree, "logic.tree.run_once", CompileRunLogicTreeNode),
    COMMAND_HANDLER(InstallLogicTree, "logic.tree.install", CompileInstallLogicTreeNode),
    COMMAND_HANDLER(SendEvent, "events.send_event", CompileSendEventNode),
    COMMAND_HANDLER(SetGlobalProperty, "properties.global_set", CompileSetGlobalPropertyNode),
    COMMAND_HANDLER(SaveVariable, "properties.variable_save", CompileSaveVariableNode),
    COMMAND_HANDLER(
        SaveVariableDict, "properties.variable_dict_save", CompileSaveVariableDictNode),
    COMMAND_HANDLER(ClearVariables, "properties.variable_clear", CompileClearVariablesNode),
    COMMAND_HANDLER(RemoveVariable, "properties.variable_remove", CompileRemoveVariableNode),
    COMMAND_HANDLER(Translate, "transform.translate", CompileTranslateNode),
    COMMAND_HANDLER(Navigate, "transform.navigate", CompileNavigateNode),
    COMMAND_HANDLER(FollowPath, "transform.follow_path", CompileFollowPathNode),
    COMMAND_HANDLER(SetCollectionVisibility,
                    "scene.set_collection_visibility",
                    CompileSetCollectionVisibilityNode),
    COMMAND_HANDLER(SetCollectionMember,
                    "scene.set_collection_member",
                    CompileSetCollectionMemberNode),
    COMMAND_HANDLER(
        SetOverlayCollection, "scene.set_overlay_collection", CompileSetOverlayCollectionNode),
    COMMAND_HANDLER(RemoveOverlayCollection,
                    "scene.remove_overlay_collection",
                    CompileRemoveOverlayCollectionNode),
    COMMAND_HANDLER(OpenObjectMenu, "ui.open_object_menu", CompileOpenObjectMenuNode),
    COMMAND_HANDLER(CloseObjectMenu, "ui.close_object_menu", CompileCloseObjectMenuNode),
    COMMAND_HANDLER(SetUIFocus, "ui.set_focus", CompileSetUIFocusNode),
    COMMAND_HANDLER(SetPlayerPointer, "ui.set_player_pointer", CompileSetPlayerPointerNode),
    COMMAND_HANDLER(TakeUIControl, "ui.take_control", CompileTakeUIControlNode),
    COMMAND_HANDLER(ScreenAnchor, "ui.screen_anchor", CompileScreenAnchorNode),
    COMMAND_HANDLER(SetUIInteractable, "ui.set_interactable", CompileSetUIInteractableNode),
    COMMAND_HANDLER(ScrollObjectView, "ui.scroll_object_view", CompileScrollObjectViewNode),
    COMMAND_HANDLER(ShowMessage, "ui.show_message", CompileShowMessageNode),
    COMMAND_HANDLER(MoveToward, "transform.move_to", CompileMoveTowardNode),
    COMMAND_HANDLER(SlowFollow, "transform.slow_follow", CompileSlowFollowNode),
    COMMAND_HANDLER(LoadScene, "scene.load_scene", CompileLoadSceneNode),
    COMMAND_HANDLER(SetScene, "scene.set_scene", CompileSetSceneNode),
    COMMAND_HANDLER(SaveGame, "game.save_game", CompileSaveGameNode),
    COMMAND_HANDLER(SetSaveExistence, "game.set_save_existence", CompileSetSaveExistenceNode),
    COMMAND_HANDLER(LoadGame, "game.load_game", CompileLoadGameNode),
    COMMAND_HANDLER(AlignAxisToVector, "transform.align_axis", CompileAlignAxisToVectorNode),
    COMMAND_HANDLER(RotateToward, "transform.rotate_to", CompileRotateTowardNode),
    COMMAND_HANDLER(SetObjectAttribute, "object.set_attribute", CompileSetObjectAttributeNode),
    COMMAND_HANDLER(ReplaceMesh, "object.replace_mesh", CompileReplaceMeshNode),
    COMMAND_HANDLER(CopyProperty, "property.copy_property", CompileCopyPropertyNode),
    COMMAND_HANDLER(
        SetBonePoseLocation, "animation.set_bone_pose_location", CompileSetBonePoseLocationNode),
    COMMAND_HANDLER(
        SetBonePoseRotation, "animation.set_bone_pose_rotation", CompileSetBonePoseRotationNode),
    COMMAND_HANDLER(
        SetBonePoseScale, "animation.set_bone_pose_scale", CompileSetBonePoseScaleNode),
    COMMAND_HANDLER(SetBonePoseTransform,
                    "animation.set_bone_pose_transform",
                    CompileSetBonePoseTransformNode),
    COMMAND_HANDLER(SetBoneAttribute, "animation.set_bone_attribute", CompileSetBoneAttributeNode),
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
    COMMAND_HANDLER(
        SetMaterialParameter, "material.set_parameter", CompileSetMaterialParameterNode),
    CUSTOM_EXPR_HANDLER(GetMaterialFromSlot,
                        "material.get_from_slot",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetMaterialSlotCount,
                        "material.slot_count",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(
        GetMaterialName, "material.name", FlowResolverKind::Invalid, CompileExpressionOutputsNode),
    CUSTOM_EXPR_HANDLER(GetMaterialParameter,
                        "material.get_parameter",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    COMMAND_HANDLER(
        SetGeometryNodesInput, "geometry.set_modifier_input", CompileSetGeometryNodesInputNode),
    CUSTOM_EXPR_HANDLER(GetEditorNodeValue,
                        "nodes.get_editor_node_value",
                        FlowResolverKind::Invalid,
                        CompileExpressionOutputsNode),
    COMMAND_HANDLER(
        SetEditorNodeValue, "nodes.set_editor_node_value", CompileSetEditorNodeValueNode),
    COMMAND_HANDLER(MakeNodeTreeUnique, "nodes.make_tree_unique", CompileMakeNodeTreeUniqueNode),
    COMMAND_HANDLER(SetNodeMute, "nodes.set_mute", CompileSetNodeMuteNode),
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
    UNSUPPORTED_COMMAND_HANDLER(SetNodeGroupSocketValue, "node_group.set_socket"),
    UNSUPPORTED_COMMAND_HANDLER(PlayMaterialSequence, "material.play_sequence"),
    COMMAND_HANDLER(StartSpeaker, "audio.start_speaker", CompileStartSpeakerNode),
    COMMAND_HANDLER(DrawLine, "debug.draw_line", CompileDrawLineNode),
    COMMAND_HANDLER(DrawCube, "debug.draw_cube", CompileDrawCubeNode),
    COMMAND_HANDLER(DrawBox, "debug.draw_box", CompileDrawBoxNode),
    COMMAND_HANDLER(Draw, "debug.draw", CompileDrawNode),
    UNSUPPORTED_COMMAND_HANDLER(LoadFileContent, "file.load_content"),
    EXPRESSION_HANDLER(
        ReceiveEvent, Expression, "events.receive_event", FlowResolverKind::PollingEventRoute),
    EXPRESSION_HANDLER(
        LogicTreeStatus, Expression, "logic.tree_status", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(ActionDone, Expression, "animation.action_done", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        ObjectsColliding, Expression, "physics.objects_colliding", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(Collision, Expression, "physics.is_colliding", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(
        OnCollision, Expression, "physics.on_collision", FlowResolverKind::PollingEventRoute),
    CUSTOM_EXPR_HANDLER(OnConstraintBreak,
                        "physics.on_constraint_break",
                        FlowResolverKind::PollingEventRoute,
                        CompileConstraintBreakEventNode),
    EXPRESSION_HANDLER(
        AnimationStatus, Expression, "animation.animation_status", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(
        Raycast, "physics.raycast", FlowResolverKind::FlowConditionRoute, CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(RaycastAll,
                        "physics.raycast_all",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(RaycastFromMesh,
                        "physics.raycast_from_mesh",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(
        ShapeCast, "physics.shape_cast", FlowResolverKind::FlowConditionRoute, CompileRaycastNode),
    CUSTOM_EXPR_HANDLER(ShapeCastAll,
                        "physics.shape_cast_all",
                        FlowResolverKind::FlowConditionRoute,
                        CompileRaycastNode),
    EXPRESSION_HANDLER(MouseRay, Expression, "physics.mouse_ray", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(CameraRay, Expression, "physics.camera_ray", FlowResolverKind::Invalid),
    EXPRESSION_HANDLER(EvaluateCurve, Expression, "curve.evaluate", FlowResolverKind::Invalid),
    CUSTOM_EXPR_HANDLER(
        RandomFloat, "math.random_float", FlowResolverKind::Invalid, CompileRandomFloatNode),
    CUSTOM_EXPR_HANDLER(
        RandomInt, "math.random_int", FlowResolverKind::Invalid, CompileRandomIntNode),
    CUSTOM_EXPR_HANDLER(
        RandomVector, "math.random_vector", FlowResolverKind::Invalid, CompileRandomVectorNode),
    CUSTOM_EXPR_HANDLER(Formula, "math.formula", FlowResolverKind::Invalid, CompileFormulaNode),
    CUSTOM_EXPR_HANDLER(TweenValue,
                        "time.tween_value",
                        FlowResolverKind::PollingEventRoute,
                        CompileTweenValueNode),
    COMMAND_HANDLER(
        AddPhysicsConstraint, "physics.add_constraint", CompileAddPhysicsConstraintNode),
    CUSTOM_EXPR_HANDLER(GetRigidBodyConstraints,
                        "physics.get_constraints",
                        FlowResolverKind::Invalid,
                        CompileGetRigidBodyConstraintsNode),
    COMMAND_HANDLER(
        RemovePhysicsConstraint, "physics.remove_constraint", CompileRemovePhysicsConstraintNode),
    CUSTOM_EXPR_HANDLER(ProjectileRay,
                        "physics.projectile_ray",
                        FlowResolverKind::Invalid,
                        CompileProjectileRayNode),
};

#undef EVENT_HANDLER
#undef CONST_HANDLER
#undef EXPRESSION_HANDLER
#undef CHECKED_EXPRESSION_HANDLER
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
    const CompileHandlerDescriptor *descriptors, const size_t count, const LN_NodeKind kind)
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
        add_compile_inventory_error(
            errors, descriptor, "command handler does not require flow input");
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
      add_compile_inventory_error(
          errors, descriptor, "only command handlers may be marked unsupported at runtime");
    }
  }

  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    const CompileHandlerDescriptor *descriptor = find_descriptor_in_span(
        descriptors, count, definition.kind);
    if (descriptor == nullptr) {
      errors.push_back(definition.idname + ": registry node has no compiler handler descriptor");
      continue;
    }
    if (descriptor->info.emits_commands != definition.has_side_effects) {
      add_compile_inventory_error(errors,
                                  *descriptor,
                                  definition.idname +
                                      ": command emission does not match side effects");
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
  const bool descriptors_valid = validate_compile_handler_descriptors(
      descriptors.data(), descriptors.size(), errors);

  for (const InternalCompileHandler &handler : internal_compile_handlers) {
    const CompileHandlerDescriptor &descriptor = handler.descriptor;
    const LN_TreeCompiler::CompileHandlerInfo &info = descriptor.info;
    switch (handler.action_kind) {
      case CompileActionKind::EmitEventSource:
        if (info.kind != CompileHandlerKind::EventSource) {
          add_compile_inventory_error(errors, descriptor, "event action lacks event handler kind");
        }
        if (handler.flow_kind != FlowResolverKind::EventSource) {
          add_compile_inventory_error(
              errors, descriptor, "event action lacks event flow resolver");
        }
        if (handler.compile_fn != nullptr) {
          add_compile_inventory_error(
              errors, descriptor, "event action must not have compile function");
        }
        break;
      case CompileActionKind::EmitConstantOutput:
        if (info.kind != CompileHandlerKind::ValueExpression &&
            info.kind != CompileHandlerKind::Expression)
        {
          add_compile_inventory_error(
              errors, descriptor, "constant-output action lacks expression handler kind");
        }
        if (handler.compile_fn != nullptr) {
          add_compile_inventory_error(
              errors, descriptor, "constant-output action must not have compile function");
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

    if (info.kind == CompileHandlerKind::Command &&
        handler.action_kind != CompileActionKind::Custom)
    {
      add_compile_inventory_error(errors, descriptor, "command handler is not custom compiled");
    }
    if (info.kind == CompileHandlerKind::Command && handler.flow_kind != FlowResolverKind::Invalid)
    {
      add_compile_inventory_error(
          errors, descriptor, "command handler must use explicit command flow resolution");
    }
    if (info.kind != CompileHandlerKind::Command && info.requires_flow_input) {
      add_compile_inventory_error(
          errors, descriptor, "non-command handler requires command flow input");
    }
  }

  return descriptors_valid && errors.empty();
}

}  // namespace ln_compiler

LN_TreeCompiler::CompileHandlerInfo LN_TreeCompiler::GetCompileHandlerInfo(const LN_NodeKind kind)
{
  if (const ln_compiler::InternalCompileHandler *handler = ln_compiler::FindInternalCompileHandler(
          kind))
  {
    return handler->descriptor.info;
  }

  return {CompileHandlerKind::Unsupported, false, false, false};
}

LN_TreeCompiler::CompileDispatch LN_TreeCompiler::GetCompileDispatch(const LN_NodeKind kind)
{
  const ln_compiler::InternalCompileHandler *handler = ln_compiler::FindInternalCompileHandler(
      kind);
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
  if (const ln_compiler::InternalCompileHandler *handler = ln_compiler::FindInternalCompileHandler(
          kind))
  {
    return &handler->descriptor;
  }
  return nullptr;
}

const LN_TreeCompiler::CompileHandlerDescriptor *LN_TreeCompiler::GetCompileHandlerDescriptors(
    size_t &r_count)
{
  static const auto descriptors = [] {
    std::array<CompileHandlerDescriptor, std::size(ln_compiler::internal_compile_handlers)>
        values{};
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
  const bool valid = ln_compiler::validate_compile_handler_descriptors(descriptors, count, errors);
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
  for (const blender::bNode *node = static_cast<const blender::bNode *>(tree.nodes.first); node;
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
                          visited_targets,
                          link->multi_input_sort_id);
    for (const ResolvedEndpoint &target : targets) {
      if (target.node == nullptr || target.socket == nullptr) {
        continue;
      }

      ResolvedLink resolved_link;
      resolved_link.fromnode = source->node;
      resolved_link.fromsock = source->socket;
      resolved_link.tonode = target.node;
      resolved_link.tosock = target.socket;
      resolved_link.multi_input_sort_id = target.multi_input_sort_id;
      resolved_links.push_back(resolved_link);
      input_links[target.socket].push_back(resolved_link);
      edges[source->node].push_back(target.node);
    }
  }

  for (auto &item : input_links) {
    if ((item.first->flag & blender::SOCK_MULTI_INPUT) == 0) {
      continue;
    }
    std::stable_sort(
        item.second.begin(), item.second.end(), [](const ResolvedLink &a, const ResolvedLink &b) {
          /* Blender draws larger multi-input sort ids first. Match the visible top-to-bottom
           * order. */
          return a.multi_input_sort_id > b.multi_input_sort_id;
        });
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

    if (node_is_active && definition_iter->second->kind == LN_NodeKind::Gate &&
        (node->custom1 < 0 || node->custom1 > 6))
    {
      AddNodeIssue(*program,
                   tree,
                   *node,
                   "Conditions",
                   LN_CompileSeverity::Error,
                   "Boolean Combine operation is invalid");
    }

    for (const LN_PinDefinition &input : definition_iter->second->inputs) {
      std::vector<const blender::bNodeSocket *> sockets;
      if (input.is_variadic) {
        for (const blender::bNodeSocket *candidate =
                 static_cast<const blender::bNodeSocket *>(node->inputs.first);
             candidate;
             candidate = candidate->next)
        {
          const size_t prefix_size = input.name.size();
          if (std::strncmp(candidate->name, input.name.c_str(), prefix_size) == 0 ||
              std::strncmp(candidate->identifier, input.name.c_str(), prefix_size) == 0)
          {
            sockets.push_back(candidate);
          }
        }
      }
      else if (const blender::bNodeSocket *socket = FindInputSocket(*node, input.name)) {
        sockets.push_back(socket);
      }

      if (sockets.empty()) {
        const bool optional_missing_socket = input.value_type == LN_ValueType::ObjectRef ||
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

      for (const blender::bNodeSocket *socket : sockets) {
        const LN_PinDefinition *matched_input = FindPinDefinition(definition_iter->second->inputs,
                                                                  *socket);
        if (matched_input != &input) {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       socket->name,
                       node_is_active ? LN_CompileSeverity::Error : LN_CompileSeverity::Warning,
                       SocketTypeMismatchDiagnostic(input, *socket));
          continue;
        }
        const auto links_iter = input_links.find(socket);
        const int link_count = (links_iter == input_links.end()) ? 0 :
                                                                   int(links_iter->second.size());
        if (node_is_active && link_count > 1 && !matched_input->allows_multiple_links) {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       socket->name,
                       LN_CompileSeverity::Error,
                       "Multiple links to a single input are not supported in V1");
        }
        if (node_is_active && matched_input->allows_multiple_links &&
            link_count > int(LN_MAX_DYNAMIC_SOCKET_COUNT))
        {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       socket->name,
                       LN_CompileSeverity::Error,
                       "Dynamic inputs are limited to 64 connections");
        }
        if (node_is_active && definition_iter->second->kind == LN_NodeKind::Gate &&
            node->custom1 == 3 && link_count > 1)
        {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       socket->name,
                       LN_CompileSeverity::Error,
                       "Boolean Combine Not accepts exactly one condition");
        }
        if (node_is_active && input.requires_link && link_count == 0) {
          AddNodeIssue(*program,
                       tree,
                       *node,
                       socket->name,
                       LN_CompileSeverity::Error,
                       RequiredInputLinkMissingDiagnostic(input));
        }
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

  /* VisitNode emits post-order and the compile order is reversed below. Feed Sequence children to
   * it in descending socket order so their complete subgraphs compile as Then 0, Then 1, ... .
   * This makes deferred command ordering independent of link creation order. */
  for (const auto &definition_item : node_definitions) {
    const blender::bNode *sequence_node = definition_item.first;
    if (!IsNodeActive(active_nodes, sequence_node) ||
        definition_item.second->kind != LN_NodeKind::Sequence)
    {
      continue;
    }

    struct RankedTarget {
      int rank;
      const blender::bNode *node;
    };
    std::vector<RankedTarget> ranked_targets;
    for (const ResolvedLink &link : resolved_links) {
      if (link.fromnode != sequence_node || !IsNodeActive(active_nodes, link.tonode) ||
          !IsExecutionFlowLink(link, node_definitions))
      {
        continue;
      }
      const std::optional<uint32_t> rank = DynamicSocketRank(*link.fromsock, "Then ");
      if (rank) {
        ranked_targets.push_back({int(*rank), link.tonode});
      }
    }
    std::stable_sort(ranked_targets.begin(),
                     ranked_targets.end(),
                     [](const RankedTarget &a, const RankedTarget &b) {
                       if (a.rank != b.rank) {
                         return a.rank > b.rank;
                       }
                       return a.node->identifier > b.node->identifier;
                     });

    std::vector<const blender::bNode *> ordered_targets;
    ordered_targets.reserve(ranked_targets.size());
    for (const RankedTarget &target : ranked_targets) {
      ordered_targets.push_back(target.node);
    }
    active_edges[sequence_node] = ordered_targets;
    active_flow_edges[sequence_node] = std::move(ordered_targets);
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
  InstructionSchedule instruction_schedule;
  ValueCache value_cache;
  BoolExpressionCache bool_expression_cache;
  FlowEventsCache flow_events_cache;
  IntExpressionCache int_expression_cache;
  FloatExpressionCache float_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;
  ScopedCompilerScratchState compiler_scratch_state(loop_frame_cache,
                                                    instruction_schedule,
                                                    {&bool_expression_cache,
                                                     &int_expression_cache,
                                                     &float_expression_cache,
                                                     &string_expression_cache,
                                                     &vector_expression_cache,
                                                     &color_expression_cache,
                                                     &value_expression_cache});
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
    CacheLinkedGenericOutputConversions(context);
  }

  if (program->m_compileReport.HasErrors()) {
    program->m_compileReport.SetDisabledReason("Logic Nodes compilation failed");
    return program;
  }

  program->ReorderInstructionsForCompiler(
      instruction_schedule.execution_orders,
      instruction_schedule.loop_order_depths,
      instruction_schedule.instruction_pulse_expressions,
      instruction_schedule.instruction_index_vector_expressions);

  /* A tree that runs nothing (an empty tree waiting for nodes) needs no root either. */
  const bool runs_anything = !program->GetInstructionHeaders(LN_Event::OnInit).empty() ||
                             !program->GetInstructionHeaders(LN_Event::OnUpdate).empty() ||
                             !program->GetInstructionHeaders(LN_Event::OnFixedUpdate).empty();
  if (!has_event_node && runs_anything) {
    program->m_compileReport.AddIssue(
        LN_CompileSeverity::Warning,
        "Logic tree has no On Init / On Frame Update event node (flow still runs on the Logic + "
        "Physics step when driven by sensors, branches, or conditions; add On Frame Update if you "
        "want an explicit frame root or to silence this hint)");
  }

  return program;
}
