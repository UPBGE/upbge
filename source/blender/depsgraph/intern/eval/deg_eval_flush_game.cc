/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * UPBGE: incremental flush and evaluation for the game engine, see deg_eval_flush_game.hh.
 */

#include "intern/eval/deg_eval_flush_game.hh"

#include "DEG_depsgraph.hh"

#include "intern/depsgraph.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender::deg {

/* OperationNode::custom_flags of the operations the current flush listed (the builders use the
 * field during a build only). */
constexpr int OPERATION_FLUSH_LISTED = 0x464C;

static bool game_lists_in_use(const Depsgraph *graph)
{
  return graph->use_game_incremental_flush && graph->game_flushed_lists_valid;
}

static void game_list_operation(Depsgraph *graph, OperationNode *op)
{
  if (op->custom_flags != OPERATION_FLUSH_LISTED) {
    op->custom_flags = OPERATION_FLUSH_LISTED;
    graph->game_flushed_operations.append(op);
  }
}

void game_flush_lists_invalidate(Depsgraph *graph)
{
  graph->game_flushed_operations.clear();
  graph->game_flushed_id_nodes.clear();
  graph->game_flushed_lists_valid = false;
}

bool game_flush_prepare_partial(Depsgraph *graph,
                                const int id_state_none,
                                const int component_state_none)
{
  if (!game_lists_in_use(graph)) {
    return false;
  }
  /* Only the nodes of the previous update carry a state: the operations it tagged (the
   * evaluation only schedules tagged operations) and the ID nodes it reached. */
  for (OperationNode *node : graph->game_flushed_operations) {
    node->scheduled = false;
    node->custom_flags = 0;
  }
  for (IDNode *id_node : graph->game_flushed_id_nodes) {
    id_node->custom_flags = id_state_none;
    for (ComponentNode *comp_node : id_node->components.values()) {
      comp_node->custom_flags = component_state_none;
    }
  }
  graph->game_flushed_operations.clear();
  graph->game_flushed_id_nodes.clear();
  return true;
}

void game_flush_prepare_after_full_reset(Depsgraph *graph)
{
  if (!graph->use_game_incremental_flush) {
    return;
  }
  graph->game_flushed_operations.clear();
  graph->game_flushed_id_nodes.clear();
  graph->game_flushed_lists_valid = true;
  /* After a (re)build the builders tagged operations without a flush: list them. The other
   * operations get the custom flags cleared, the builders leave them set. */
  for (OperationNode *node : graph->operations) {
    if (node->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
      node->custom_flags = OPERATION_FLUSH_LISTED;
      graph->game_flushed_operations.append(node);
    }
    else {
      node->custom_flags = 0;
    }
  }
}

void game_flush_list_nodes(Depsgraph *graph,
                           OperationNode *op_node,
                           IDNode *id_node,
                           const int id_state_before,
                           ComponentNode *comp_node,
                           const int comp_state_before)
{
  if (!game_lists_in_use(graph)) {
    return;
  }
  /* Listed even when already tagged: an operation an earlier update left tagged (invisible, so
   * not evaluated) has to be evaluated once a flush reaches it again. */
  game_list_operation(graph, op_node);
  if (id_node->custom_flags != id_state_before) {
    graph->game_flushed_id_nodes.append(id_node);
  }
  if (comp_node->custom_flags != comp_state_before) {
    /* The flush handled the component now and tagged its operations. */
    for (OperationNode *op : comp_node->operations) {
      if (op->flag & DEPSOP_FLAG_NEEDS_UPDATE) {
        game_list_operation(graph, op);
      }
    }
  }
}

Span<IDNode *> game_flush_id_nodes_to_walk(const Depsgraph *graph)
{
  if (game_lists_in_use(graph)) {
    return graph->game_flushed_id_nodes;
  }
  return graph->id_nodes;
}

Span<OperationNode *> game_eval_operations_to_walk(const Depsgraph *graph)
{
  /* Every visible operation tagged for update is in the list: an operation left tagged by an
   * earlier update is invisible, and becomes visible only through a tag which flushes it again. */
  if (game_lists_in_use(graph) && !graph->has_animated_visibility &&
      !graph->need_update_nodes_visibility)
  {
    return graph->game_flushed_operations;
  }
  return graph->operations;
}

}  // namespace blender::deg

namespace blender {

void DEG_game_set_incremental_flush(Depsgraph *depsgraph, const bool enable)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  if (deg_graph->use_game_incremental_flush == enable) {
    return;
  }
  /* A flush of the other mode leaves node states the lists do not know about. */
  deg::game_flush_lists_invalidate(deg_graph);
  deg_graph->use_game_incremental_flush = enable;
}

}  // namespace blender
