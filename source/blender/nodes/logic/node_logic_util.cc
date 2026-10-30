/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_colortools.hh"
#include "BKE_context.hh"

#include "DNA_scene_types.h"

#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "BLO_read_write.hh"

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

}  // namespace blender
