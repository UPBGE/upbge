/* SPDX-FileCopyrightText: 2015 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "intern/builder/deg_builder_cycle.h"

// TOO(sergey): Use some wrappers over those?
#include <cstdio>
#include <cstdlib>

#include "BLI_stack.hh"

#include "CLG_log.h"

#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_operation.hh"

#include "intern/depsgraph.hh"
#include "intern/depsgraph_relation.hh"

static CLG_LogRef LOG = {"depsgraph"};

namespace blender::deg {

namespace {

enum eCyclicCheckVisitedState {
  /* Not is not visited at all during traversal. */
  NODE_NOT_VISITED = 0,
  /* Node has been visited during traversal and not in current stack. */
  NODE_VISITED = 1,
  /* Node has been visited during traversal and is in current stack. */
  NODE_IN_STACK = 2,
};

struct StackEntry {
  OperationNode *node;
  StackEntry *from;
  Relation *via_relation;
};

struct CyclesSolverState {
  CyclesSolverState(Depsgraph *graph) : graph(graph) {}
  ~CyclesSolverState()
  {
    if (num_cycles != 0) {
      CLOG_WARN(&LOG, "Detected %d dependency cycles", num_cycles);
    }
  }
  Depsgraph *graph;
  Stack<StackEntry> traversal_stack;
  int num_cycles = 0;
};

inline void set_node_visited_state(Node *node, eCyclicCheckVisitedState state)
{
  node->custom_flags = (node->custom_flags & ~0x3) | int(state);
}

inline eCyclicCheckVisitedState get_node_visited_state(Node *node)
{
  return (eCyclicCheckVisitedState)(node->custom_flags & 0x3);
}

inline void set_node_num_visited_children(Node *node, int num_children)
{
  node->custom_flags = (node->custom_flags & 0x3) | (num_children << 2);
}

inline int get_node_num_visited_children(Node *node)
{
  return node->custom_flags >> 2;
}

void schedule_node_to_stack(CyclesSolverState *state, OperationNode *node)
{
  StackEntry entry;
  entry.node = node;
  entry.from = nullptr;
  entry.via_relation = nullptr;
  state->traversal_stack.push(entry);
  set_node_visited_state(node, NODE_IN_STACK);
}

/* Schedule leaf nodes (node without input links) for traversal. */
void schedule_leaf_nodes(CyclesSolverState *state)
{
  for (OperationNode *node : state->graph->operations) {
    bool has_inlinks = false;
    for (Relation *rel : node->inlinks) {
      if (rel->from->type == NodeType::OPERATION) {
        has_inlinks = true;
      }
    }
    node->custom_flags = 0;
    if (has_inlinks == false) {
      schedule_node_to_stack(state, node);
    }
    else {
      set_node_visited_state(node, NODE_NOT_VISITED);
    }
  }
}

/* Schedule node which was not checked yet for being belong to
 * any of dependency cycle.
 */
bool schedule_non_checked_node(CyclesSolverState *state)
{
  for (OperationNode *node : state->graph->operations) {
    if (get_node_visited_state(node) == NODE_NOT_VISITED) {
      schedule_node_to_stack(state, node);
      return true;
    }
  }
  return false;
}

bool check_relation_can_murder(Relation *relation)
{
  if (relation->flag & RELATION_FLAG_GODMODE) {
    return false;
  }
  return true;
}

Relation *select_relation_to_murder(Relation *relation, StackEntry *cycle_start_entry)
{
  /* More or less Russian roulette solver, which will make sure only
   * specially marked relations are kept alive.
   *
   * TODO(sergey): There might be better strategies here. */
  if (check_relation_can_murder(relation)) {
    return relation;
  }
  StackEntry *current = cycle_start_entry;
  OperationNode *to_node = (OperationNode *)relation->to;
  while (current->node != to_node) {
    if (check_relation_can_murder(current->via_relation)) {
      return current->via_relation;
    }
    current = current->from;
  }
  return relation;
}

/* Solve cycles with all nodes which are scheduled for traversal. */
void solve_cycles(CyclesSolverState *state)
{
  Stack<StackEntry> &traversal_stack = state->traversal_stack;
  while (!traversal_stack.is_empty()) {
    StackEntry *entry = &traversal_stack.peek();
    OperationNode *node = entry->node;
    bool all_child_traversed = true;
    const int num_visited = get_node_num_visited_children(node);
    for (int i = num_visited; i < node->outlinks.size(); i++) {
      Relation *rel = node->outlinks[i];
      if (rel->to->type == NodeType::OPERATION) {
        OperationNode *to = (OperationNode *)rel->to;
        eCyclicCheckVisitedState to_state = get_node_visited_state(to);
        if (to_state == NODE_IN_STACK) {
          std::string cycle_str = "  " + to->full_identifier() + " depends on\n  " +
                                  node->full_identifier() + " via '" + rel->name + "'\n";
          StackEntry *current = entry;
          while (current->node != to) {
            BLI_assert(current != nullptr);
            cycle_str += "  " + current->from->node->full_identifier() + " via '" +
                         current->via_relation->name + "'\n";
            current = current->from;
          }
          CLOG_WARN(&LOG, "Dependency cycle detected:\n%s", cycle_str.c_str());
          Relation *sacrificial_relation = select_relation_to_murder(rel, entry);
          sacrificial_relation->flag |= RELATION_FLAG_CYCLIC;
          ++state->num_cycles;
        }
        else if (to_state == NODE_NOT_VISITED) {
          StackEntry new_entry;
          new_entry.node = to;
          new_entry.from = entry;
          new_entry.via_relation = rel;
          traversal_stack.push(new_entry);
          set_node_visited_state(node, NODE_IN_STACK);
          all_child_traversed = false;
          set_node_num_visited_children(node, i);
          break;
        }
      }
    }
    if (all_child_traversed) {
      set_node_visited_state(node, NODE_VISITED);
      traversal_stack.pop();
    }
  }
}

}  // namespace

void deg_graph_detect_cycles(Depsgraph *graph)
{
  CyclesSolverState state(graph);
  /* First we solve cycles which are reachable from leaf nodes. */
  schedule_leaf_nodes(&state);
  solve_cycles(&state);
  /* We are not done yet. It is possible to have closed loop cycle,
   * for example A -> B -> C -> A. These nodes were not scheduled
   * yet (since they all have inlinks), and were not traversed since
   * nobody else points to them. */
  while (schedule_non_checked_node(&state)) {
    solve_cycles(&state);
  }
}

}  // namespace blender::deg
