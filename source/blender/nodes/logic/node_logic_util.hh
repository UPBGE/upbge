/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "NOD_logic.hh"
#include "NOD_register.hh"             // IWYU pragma: export
#include "NOD_socket_declarations.hh"  // IWYU pragma: export

#include "node_logic_socket_declarations.hh"  // IWYU pragma: export

#include "RNA_access.hh"  // IWYU pragma: export

namespace blender {

struct BlendDataReader;
struct BlendWriter;
struct CurveMapping;

bool logic_node_poll_default(const bke::bNodeType *ntype,
                             const bNodeTree *ntree,
                             const char **r_disabled_hint);
void logic_node_type_base(bke::bNodeType *ntype,
                          UString idname);

namespace nodes::logic {

void add_input_button_status_outputs(NodeDeclarationBuilder &b);
/** An Input Map action name: a search in the scene's Game Input Map that also takes other
 * names. */
void draw_input_action_socket(CustomSocketDrawParams &params);

/** Shared, validated storage lifecycle for Logic Nodes that expose Blender's curve widget. */
CurveMapping *curve_mapping_create_default();
CurveMapping *curve_mapping_ensure(bNode &node);
void curve_mapping_blend_write(const bNodeTree &tree,
                               const bNode &node,
                               BlendWriter &writer);
void curve_mapping_blend_read(bNodeTree &tree, bNode &node, BlendDataReader &reader);

}  // namespace nodes::logic

}  // namespace blender
