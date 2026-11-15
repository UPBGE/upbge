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
/** The input nodes' "Also When UI Uses It" checkbox (RNA `use_ui_input`) as the node's last row,
 * below its inputs (Keyboard Key, Mouse Button, Gamepad Button, Input Action; 2026-11-13): the
 * node uses a custom socket order and declares it last. */
void add_use_ui_input_layout(NodeDeclarationBuilder &b);
/** The bone nodes' name fields (node_logic_bone_search.cc, 2026-11-13): a Bone Name input drawn
 * as Blender's bone fields, its dropdown listing the bones of the armature the input
 * `armature_socket` names (empty: with `empty_is_owner` the armatures this tree is applied to;
 * linked: the scene's), any typed name kept; red when that armature surely has no such bone. */
CustomSocketDrawFn bone_name_field(StringRefNull armature_socket, bool empty_is_owner = true);
/** A Constraint Name input: the constraints of the bone the input `bone_socket` names, on those
 * armatures; red when that bone surely has no such constraint. */
CustomSocketDrawFn bone_constraint_field(StringRefNull armature_socket,
                                         StringRefNull bone_socket);
/** A Vertex Group input drawn as Blender's modifier field (node_logic_vertex_group_search.cc):
 * its dropdown lists the vertex groups and number attributes of the mesh of the object the input
 * `object_socket` names (a string literal; empty: the objects this tree is applied to), any typed
 * name kept; red only when no such object has the name. */
CustomSocketDrawFn vertex_group_field(StringRefNull object_socket);
/** The property name fields (node_logic_property_search.cc, 2026-11-15): Blender's search field,
 * its dropdown listing the names there are with each one's type or use beside it, any typed name
 * kept (properties can be made while the game runs); red only when the name is surely a mistake.
 * The socket names are string literals of the node's declaration.
 *
 * A game property of the object the input `object_socket` names: that object's properties; empty:
 * the object `fallback_socket` names (when given), else the objects this tree is applied to;
 * linked: the file's. Red when those objects are known and none has it, or not known and no
 * object has it, and nothing else in the file names it (another node's property field, live
 * text). `empty_is_owner` false: an empty input may mean another object (Object Grid View's
 * menu player), never sure. */
CustomSocketDrawFn game_property_field(const char *object_socket,
                                       const char *fallback_socket = nullptr,
                                       bool empty_is_owner = true);
/** A Property filter (only objects with it count): every object's game properties; red when no
 * object has it and nothing else names it. */
CustomSocketDrawFn game_property_filter_field();
/** A tree property: the names this tree's Get / Set / Toggle Tree Property nodes use. `reads`
 * (Get Tree Property): red when no Set or Toggle Tree Property of the tree writes it. */
CustomSocketDrawFn tree_property_field(bool reads);
/** A global property: the names the file's global property nodes use, of the Category the input
 * `category_socket` names when typed in; a Category: the categories. Never red. */
CustomSocketDrawFn global_property_field(const char *category_socket);
CustomSocketDrawFn global_category_field();
/** What a game property field's red means, ending its description. */
inline constexpr const char *game_property_pick_note =
    "Pick one of the object's game properties, or type any name. Red when the object surely has "
    "none of that name: it lacks it, and no other node or live text in the file uses the name";
inline constexpr const char *game_property_filter_note =
    "Pick one of the game properties objects have, or type any name. Red when no object has it "
    "and no node or live text in the file uses the name";
/** The descriptions those inputs share. */
inline constexpr const char *bone_name_description =
    "The bone: pick one of the armature's bones, or type any name. Red when the armature has no "
    "bone of that name";
inline constexpr const char *bone_constraint_description =
    "The constraint on that bone: pick one of its constraints, or type any name. Red when the "
    "bone has no constraint of that name";
/** The ray family's folded panels (2026-11-11; Raycast, Shape Cast, Mouse Ray, Camera Ray,
 * Projectile Ray, Raycast from Mesh, Sweep Object): "Filters" with X-Ray, Include Sensors and Hit
 * Backfaces (Line of Sight: without X-Ray), "Visualize" whose header checkbox is the Visualize
 * input, with Duration, Hit Color and Miss Color. Declared after the plain inputs. */
void add_ray_filters_panel(NodeDeclarationBuilder &b);
void add_ray_filters_panel_without_xray(NodeDeclarationBuilder &b);
void add_visualize_panel(NodeDeclarationBuilder &b);
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
