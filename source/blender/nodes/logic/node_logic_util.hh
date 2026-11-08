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
/** An option of a merged node (NOD_logic_merged.hh, 2026-11-08): the node it was before, set up
 * like a logic node type but never registered as one (`logic_option_register` keeps it for the
 * merged node, which forwards its declaration, buttons, init and update to it). */
void logic_option_type_base(bke::bNodeType *ntype, UString idname);
void logic_option_register(bke::bNodeType &ntype);

namespace nodes::logic {

void add_input_button_status_outputs(NodeDeclarationBuilder &b);
/** An Input Map action name: a search in the scene's Game Input Map that also takes other
 * names. */
void draw_input_action_socket(CustomSocketDrawParams &params);

/** The list and dictionary action nodes (Append, Remove Index, Remove Value, Set List Index, Set
 * Dictionary Key, Add to Dictionary Value, Remove Dictionary Key; 2026-11-08). custom1 (RNA
 * "store") picks what they change: 1 Game Property, every new node: the list or dictionary a
 * game property holds, at the end of the step (Object and Property shown, only Done out); 0
 * Output Only, every node saved before: the changed copy at the output (the List or Dictionary
 * input and the outputs shown), as before. The engine reads it as LN_COLLECTION_STORE_*. Each
 * node declares its Object and Property inputs itself (the socket audit reads declarations from
 * the node's source), with these descriptions. */
std::string collection_store_object_description();
std::string collection_store_property_description(const char *collection);
void collection_store_init(bNodeTree *tree, bNode *node);
void collection_store_update(bNodeTree *tree, bNode *node);
void collection_store_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr);

/** Shared, validated storage lifecycle for Logic Nodes that expose Blender's curve widget. */
CurveMapping *curve_mapping_create_default();
CurveMapping *curve_mapping_ensure(bNode &node);
void curve_mapping_blend_write(const bNodeTree &tree,
                               const bNode &node,
                               BlendWriter &writer);
void curve_mapping_blend_read(bNodeTree &tree, bNode &node, BlendDataReader &reader);

}  // namespace nodes::logic

}  // namespace blender
