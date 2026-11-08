/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_colortools.hh"
#include "BKE_context.hh"

#include "BLI_map.hh"

#include "DNA_scene_types.h"

#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

#include "NOD_logic_merged.hh"
#include "NOD_socket_search_link.hh"

#include "node_util.hh"

namespace blender {

namespace nodes::logic {

namespace {

bool curve_mapping_has_valid_point_counts(const CurveMapping *mapping)
{
  if (mapping == nullptr) {
    return false;
  }
  for (int curve_index = 0; curve_index < CM_TOT; curve_index++) {
    if (mapping->cm[curve_index].totpoint < 0) {
      return false;
    }
  }
  return true;
}

void clear_curve_mapping_data(CurveMapping &mapping)
{
  for (int curve_index = 0; curve_index < CM_TOT; curve_index++) {
    mapping.cm[curve_index].totpoint = 0;
    mapping.cm[curve_index].curve = nullptr;
    mapping.cm[curve_index].table = nullptr;
    mapping.cm[curve_index].premultable = nullptr;
  }
}

bool curve_mapping_is_valid(const CurveMapping *mapping)
{
  if (!curve_mapping_has_valid_point_counts(mapping) || mapping->cur < 0 ||
      mapping->cur >= CM_TOT || mapping->cm[0].totpoint < 2 ||
      mapping->cm[0].curve == nullptr)
  {
    return false;
  }

  for (int curve_index = 0; curve_index < CM_TOT; curve_index++) {
    if (mapping->cm[curve_index].totpoint > 0 && mapping->cm[curve_index].curve == nullptr) {
      return false;
    }
  }
  return true;
}

}  // namespace

Span<NodeMetadata> node_metadata()
{
  return logic_node_metadata();
}

const NodeMetadata *node_metadata(StringRef idname)
{
  return find_logic_node_metadata(idname);
}

void add_input_button_status_outputs(NodeDeclarationBuilder &b)
{
  namespace decl = blender::nodes::logic::decl;
  b.add_output<decl::Execution>("Triggered"_ustr, "Out"_ustr)
      .is_default_link_socket()
      .description(
          "Logic + Physics step pulse selected by the Pressed, While Held, or Released mode");
  b.add_output<decl::Condition>("Active"_ustr).description(
      "True while held; fixed flow reads the fixed-step input snapshot");
  b.add_output<decl::Condition>("Pressed"_ustr).description(
      "True once when pressed; frame and fixed flow consume independent accumulated edges");
  b.add_output<decl::Condition>("Released"_ustr).description(
      "True once when released; frame and fixed flow consume independent accumulated edges");
}

void draw_input_action_socket(CustomSocketDrawParams &params)
{
  Scene *scene = CTX_data_scene(&params.C);
  if (params.socket.is_directly_linked() || scene == nullptr) {
    params.draw_standard(params.layout);
    return;
  }
  PointerRNA game_ptr = RNA_pointer_create_discrete(&scene->id, RNA_SceneGameData, &scene->gm);
  params.layout.prop_search(&params.socket_ptr,
                            RNA_struct_find_property(&params.socket_ptr, "default_value"),
                            &game_ptr,
                            RNA_struct_find_property(&game_ptr, "input_actions"),
                            nullptr,
                            params.label,
                            ICON_NONE,
                            true);
}

/* The engine's LN_COLLECTION_STORE_OUTPUT_ONLY (0) / LN_COLLECTION_STORE_GAME_PROPERTY (1). */
static constexpr int16_t collection_store_game_property = 1;

std::string collection_store_object_description()
{
  return "Object with the game property; empty uses this tree's object";
}

std::string collection_store_property_description(const char *collection)
{
  return std::string("The game property holding the ") + collection +
         ": changed at the end of the step, after this step's earlier writes of it (a missing "
         "one starts empty)";
}

void collection_store_init(bNodeTree *tree, bNode *node)
{
  node->custom1 = collection_store_game_property;
  collection_store_update(tree, node);
}

void collection_store_update(bNodeTree *tree, bNode *node)
{
  const bool game_property = node->custom1 == collection_store_game_property;
  /* The lookups below read the socket maps (a new node's are made here). */
  tree->ensure_topology_cache();
  const auto show = [&](bNodeSocket *socket, const bool available) {
    if (socket != nullptr) {
      bke::node_set_socket_availability(*tree, *socket, available);
    }
  };
  show(node->input_by_identifier("Object"_ustr), game_property);
  show(node->input_by_identifier("Property"_ustr), game_property);
  show(node->input_by_identifier("List"_ustr), !game_property);
  show(node->input_by_identifier("Dictionary"_ustr), !game_property);
  /* Game Property mode changes the property at the end of the step: its outputs would show the
   * list as the tree reads it now, without this step's earlier changes. Read it in the next step
   * (Get Game Property) instead. */
  show(node->output_by_identifier("List"_ustr), !game_property);
  show(node->output_by_identifier("Dictionary"_ustr), !game_property);
  show(node->output_by_identifier("Value"_ustr), !game_property);
}

void collection_store_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "store", UI_ITEM_NONE, "", ICON_NONE);
}

CurveMapping *curve_mapping_create_default()
{
  CurveMapping *mapping = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  BKE_curvemapping_init(mapping);
  return mapping;
}

CurveMapping *curve_mapping_ensure(bNode &node)
{
  CurveMapping *mapping = static_cast<CurveMapping *>(node.storage);
  if (!curve_mapping_is_valid(mapping)) {
    if (mapping != nullptr) {
      BKE_curvemapping_free(mapping);
    }
    node.storage = curve_mapping_create_default();
    mapping = static_cast<CurveMapping *>(node.storage);
  }
  else {
    BKE_curvemapping_init(mapping);
  }
  return mapping;
}

void curve_mapping_blend_write(const bNodeTree & /*tree*/,
                               const bNode &node,
                               BlendWriter &writer)
{
  const CurveMapping *mapping = static_cast<const CurveMapping *>(node.storage);
  if (curve_mapping_is_valid(mapping)) {
    BKE_curvemapping_curves_blend_write(&writer, mapping);
  }
}

void curve_mapping_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  if (node.storage != nullptr) {
    CurveMapping &mapping = *static_cast<CurveMapping *>(node.storage);
    if (curve_mapping_has_valid_point_counts(&mapping)) {
      BKE_curvemapping_blend_read(&reader, &mapping);
    }
    else {
      clear_curve_mapping_data(mapping);
    }
  }
  curve_mapping_ensure(node);
}

}  // namespace nodes::logic

bool logic_node_poll_default(const bke::bNodeType * /*ntype*/,
                             const bNodeTree *ntree,
                             const char **r_disabled_hint)
{
  if (ntree == nullptr || ntree->type != NTREE_LOGIC) {
    *r_disabled_hint = RPT_("Not a logic node tree");
    return false;
  }
  return true;
}

void logic_node_type_base(bke::bNodeType *ntype, UString idname)
{
  bke::node_type_base(*ntype, idname);

  if (const nodes::logic::NodeMetadata *metadata = nodes::logic::node_metadata(idname.ref())) {
    ntype->ui_name = metadata->ui_name;
    ntype->ui_description = metadata->ui_description;
  }

  ntype->poll = logic_node_poll_default;
  ntype->insert_link = node_insert_link_default;
  ntype->gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

/* The options of the merged nodes, by their former idname. Filled while the node types register
 * (NOD_REGISTER_NODE), read by the merged nodes afterwards; never changed after start-up. */
static Map<StringRef, const bke::bNodeType *> &logic_option_types()
{
  static Map<StringRef, const bke::bNodeType *> types;
  return types;
}

void logic_option_type_base(bke::bNodeType *ntype, UString idname)
{
  /* NODE_CUSTOM: no RNA struct of its own (node_type_base looks one up for a Blender node type);
   * the merged node's RNA holds the option's settings. */
  bke::node_type_base(*ntype, idname, NODE_CUSTOM);
  if (const nodes::logic::NodeMetadata *metadata = nodes::logic::node_metadata(idname.ref())) {
    ntype->ui_name = metadata->ui_name;
    ntype->ui_description = metadata->ui_description;
  }
  ntype->poll = logic_node_poll_default;
}

void logic_option_register(bke::bNodeType &ntype)
{
  BLI_assert(nodes::logic::find_merged_node_of_option(ntype.idname.ref()) != nullptr);
  logic_option_types().add_new(ntype.idname.ref(), &ntype);
}

namespace nodes::logic {

const bke::bNodeType *find_option_node_type(const StringRef idname)
{
  return logic_option_types().lookup_default(idname, nullptr);
}

}  // namespace nodes::logic

}  // namespace blender
