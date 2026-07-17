/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Reachability analysis: only nodes on an event/sensor execution chain (plus data
 * dependencies) are validated and compiled. Unused canvas nodes emit warnings only.
 */

#include "LN_TreeCompiler_internal.hh"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace ln_compiler {

namespace {

bool IsExecutionFlowInputPin(const LN_PinDefinition &pin)
{
  return pin.kind == LN_PinKind::Execution;
}

bool IsExecutionFlowOutputPin(const LN_PinDefinition &pin)
{
  return pin.kind == LN_PinKind::Execution;
}

bool NodeHasExecutionOutput(const LN_NodeDefinition &definition)
{
  for (const LN_PinDefinition &pin : definition.outputs) {
    if (pin.kind == LN_PinKind::Execution) {
      return true;
    }
  }
  return false;
}

bool IsFlowEntryNode(const LN_NodeDefinition &definition, const InternalCompileHandler *handler)
{
  if (handler == nullptr) {
    return false;
  }
  if (handler->action_kind == CompileActionKind::EmitEventSource) {
    return true;
  }
  if (handler->flow_kind != FlowResolverKind::PollingEventRoute) {
    return false;
  }
  if (!NodeHasExecutionOutput(definition)) {
    return false;
  }
  return handler->descriptor.info.kind == LN_TreeCompiler::CompileHandlerKind::Expression ||
         handler->descriptor.info.kind == LN_TreeCompiler::CompileHandlerKind::ValueExpression;
}

bool NodeHasSideEffectsOrCommands(const LN_NodeDefinition &definition,
                                  const InternalCompileHandler *handler)
{
  if (definition.has_side_effects) {
    return true;
  }
  if (handler != nullptr && handler->descriptor.info.emits_commands) {
    return true;
  }
  return false;
}

}  // namespace

bool IsExecutionFlowLink(const ResolvedLink &link, const NodeDefinitionMap &node_definitions)
{
  const auto from_def = node_definitions.find(link.fromnode);
  const auto to_def = node_definitions.find(link.tonode);
  if (from_def == node_definitions.end() || to_def == node_definitions.end()) {
    return false;
  }

  const LN_PinDefinition *from_pin = FindPinDefinition(from_def->second->outputs, *link.fromsock);
  const LN_PinDefinition *to_pin = FindPinDefinition(to_def->second->inputs, *link.tosock);
  if (from_pin == nullptr || to_pin == nullptr) {
    return false;
  }

  return IsExecutionFlowOutputPin(*from_pin) && IsExecutionFlowInputPin(*to_pin);
}

ActiveNodeSet ComputeActiveNodes(
    const std::vector<const blender::bNode *> &nodes,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    const std::vector<ResolvedLink> &resolved_links)
{
  ActiveNodeSet result;

  std::unordered_map<const blender::bNodeSocket *, std::vector<const ResolvedLink *>>
      links_by_output;
  links_by_output.reserve(resolved_links.size());
  for (const ResolvedLink &link : resolved_links) {
    links_by_output[link.fromsock].push_back(&link);
  }

  std::unordered_set<const blender::bNode *> has_incoming_flow;
  for (const ResolvedLink &link : resolved_links) {
    if (IsExecutionFlowLink(link, node_definitions)) {
      has_incoming_flow.insert(link.tonode);
    }
  }

  std::deque<const blender::bNode *> queue;
  for (const blender::bNode *node : nodes) {
    const auto def_iter = node_definitions.find(node);
    if (def_iter == node_definitions.end()) {
      continue;
    }

    const LN_NodeDefinition &definition = *def_iter->second;
    const InternalCompileHandler *handler = FindInternalCompileHandler(definition.kind);
    if (!IsFlowEntryNode(definition, handler)) {
      continue;
    }

    if (handler->action_kind != CompileActionKind::EmitEventSource &&
        has_incoming_flow.find(node) != has_incoming_flow.end())
    {
      continue;
    }

    if (result.flow_reachable.insert(node).second) {
      queue.push_back(node);
    }
  }

  while (!queue.empty()) {
    const blender::bNode *node = queue.front();
    queue.pop_front();

    const auto def_iter = node_definitions.find(node);
    if (def_iter == node_definitions.end()) {
      continue;
    }

    const LN_NodeDefinition &definition = *def_iter->second;
    for (const LN_PinDefinition &out_pin : definition.outputs) {
      if (!IsExecutionFlowOutputPin(out_pin)) {
        continue;
      }

      const blender::bNodeSocket *out_socket = FindOutputSocket(*node, out_pin.name);
      if (out_socket == nullptr) {
        continue;
      }

      const auto out_links_iter = links_by_output.find(out_socket);
      if (out_links_iter == links_by_output.end()) {
        continue;
      }

      for (const ResolvedLink *link : out_links_iter->second) {
        if (!IsExecutionFlowLink(*link, node_definitions)) {
          continue;
        }
        if (result.flow_reachable.insert(link->tonode).second) {
          queue.push_back(link->tonode);
        }
      }
    }
  }

  result.active = result.flow_reachable;
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (const blender::bNode *node : result.active) {
      for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
               node->inputs.first);
           socket;
           socket = socket->next)
      {
        const auto links_iter = input_links.find(socket);
        if (links_iter == input_links.end()) {
          continue;
        }
        for (const ResolvedLink &link : links_iter->second) {
          if (link.fromnode != nullptr && result.active.insert(link.fromnode).second) {
            expanded = true;
          }
        }
      }
    }
  }

  return result;
}

bool IsNodeActive(const ActiveNodeSet &active_nodes, const blender::bNode *node)
{
  return active_nodes.active.find(node) != active_nodes.active.end();
}

void WarnInactiveNodes(LN_Program &program,
                       const blender::bNodeTree &tree,
                       const std::vector<const blender::bNode *> &nodes,
                       const NodeDefinitionMap &node_definitions,
                       const ActiveNodeSet &active_nodes)
{
  for (const blender::bNode *node : nodes) {
    if (IsNodeActive(active_nodes, node)) {
      continue;
    }

    const auto def_iter = node_definitions.find(node);
    if (def_iter == node_definitions.end()) {
      continue;
    }

    const InternalCompileHandler *handler = FindInternalCompileHandler(def_iter->second->kind);
    if (!NodeHasSideEffectsOrCommands(*def_iter->second, handler)) {
      continue;
    }

    AddNodeIssue(program,
                 tree,
                 *node,
                 nullptr,
                 LN_CompileSeverity::Warning,
                 "Node is not connected to an event chain and will not run");
  }
}

}  // namespace ln_compiler
