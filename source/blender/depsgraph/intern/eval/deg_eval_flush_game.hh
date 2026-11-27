/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * UPBGE: incremental flush and evaluation for the game engine.
 *
 * The flush of Blender resets every operation and ID node, and the evaluation walks all the
 * operations several times (pending parents, scheduling), whatever the update. In a large game
 * scene that costs the whole graph every frame for a few operations evaluated.
 *
 * When the game engine enables it on its depsgraph (#DEG_game_set_incremental_flush), the flush
 * lists the operations it tags and the ID nodes it reaches; the next flush resets those only and
 * the evaluation walks the list. A (re)build or a change of the setting invalidates the lists
 * (full reset once, operations tagged by the builders listed), and animated or changing
 * visibility keeps the full passes.
 *
 * Every function here does nothing (or returns the full graph) when the setting is off, so the
 * Blender code paths only call them through a short hook.
 */

#pragma once

#include "BLI_span.hh"

namespace blender::deg {

struct ComponentNode;
struct Depsgraph;
struct IDNode;
struct OperationNode;

/** Forget the lists: the next flush resets the whole graph. Nodes are (re)built or freed. */
void game_flush_lists_invalidate(Depsgraph *graph);

/**
 * Reset of the nodes the previous flush listed, instead of the whole graph.
 * \return false when the full reset of Blender has to run (setting off or lists invalid).
 */
bool game_flush_prepare_partial(Depsgraph *graph, int id_state_none, int component_state_none);

/** After the full reset of Blender: lists the operations the builders tagged without a flush. */
void game_flush_prepare_after_full_reset(Depsgraph *graph);

/**
 * After the flush handled an operation of its queue: lists the operation, its ID node when the
 * flush reached it now and the tagged operations of its component when the flush handled it now
 * (states before the handling given).
 */
void game_flush_list_nodes(Depsgraph *graph,
                           OperationNode *op_node,
                           IDNode *id_node,
                           int id_state_before,
                           ComponentNode *comp_node,
                           int comp_state_before);

/** ID nodes the flush can have marked modified: the listed ones, else all of them. */
Span<IDNode *> game_flush_id_nodes_to_walk(const Depsgraph *graph);

/** Operations which can need evaluation: the listed ones, else all of them. */
Span<OperationNode *> game_eval_operations_to_walk(const Depsgraph *graph);

}  // namespace blender::deg
