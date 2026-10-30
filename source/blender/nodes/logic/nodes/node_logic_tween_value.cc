/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_util.hh"

namespace blender::nodes::node_logic_tween_value_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int tween_type_float = 0;
static constexpr int tween_type_vector = 1;
static constexpr int tween_type_euler = 2;
static constexpr int tween_type_quaternion_slerp = 3;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Forward"_ustr);
  b.add_input<decl::Execution>("Back"_ustr);
  b.add_input<decl::Float>("From"_ustr, "FromFloat"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("To"_ustr, "ToFloat"_ustr).default_value(1.0f);
  b.add_input<decl::Vector>("From"_ustr, "FromVector"_ustr).default_value(float3(0.0f));
  b.add_input<decl::Vector>("To"_ustr, "ToVector"_ustr).default_value(float3(1.0f));
  b.add_input<decl::Rotation>("From"_ustr, "FromRotation"_ustr);
  b.add_input<decl::Rotation>("To"_ustr, "ToRotation"_ustr);
  b.add_input<decl::Float>("Duration"_ustr).default_value(1.0f);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr)
      .default_value(false)
      .description("Advance with unscaled time, including in While Paused frame logic");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Execution>("Reached"_ustr);
  b.add_output<decl::Float>("Result"_ustr, "ResultFloat"_ustr);
  b.add_output<decl::Vector>("Result"_ustr, "ResultVector"_ustr);
  b.add_output<decl::Rotation>("Result"_ustr, "ResultRotation"_ustr);
  b.add_output<decl::Float>("Factor"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  bNode *node = static_cast<bNode *>(ptr->data);
  logic::curve_mapping_ensure(*node);

  layout.prop(ptr, "value_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "on_demand", UI_ITEM_NONE, "On Demand", ICON_NONE);
  if (RNA_boolean_get(ptr, "on_demand")) {
    layout.prop(ptr, "instant_reset", UI_ITEM_NONE, "Instant Reset", ICON_NONE);
  }
  template_curve_mapping(&layout, ptr, "mapping", 0, false, false, false, false, false);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = tween_type_float;
  node->custom2 = 1;
  node->storage = logic::curve_mapping_create_default();
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  logic::curve_mapping_ensure(*node);
  if (node->custom1 < tween_type_float || node->custom1 > tween_type_quaternion_slerp) {
    node->custom1 = tween_type_float;
  }

  const bool on_demand = (node->custom2 & 1) != 0;
  const bool float_type = node->custom1 == tween_type_float;
  const bool vector_type = node->custom1 == tween_type_vector;
  const bool rotation_type = node->custom1 == tween_type_euler ||
                             node->custom1 == tween_type_quaternion_slerp;

  auto set_input = [&](const char *id, const bool visible) {
    if (bNodeSocket *socket = node->input_by_identifier(UString(id))) {
      bke::node_set_socket_availability(*ntree, *socket, visible);
    }
  };
  auto set_output = [&](const char *id, const bool visible) {
    if (bNodeSocket *socket = node->output_by_identifier(UString(id))) {
      bke::node_set_socket_availability(*ntree, *socket, visible);
    }
  };

  set_input("Forward", !on_demand);
  set_input("Back", !on_demand);
  set_input("FromFloat", float_type);
  set_input("ToFloat", float_type);
  set_input("FromVector", vector_type);
  set_input("ToVector", vector_type);
  set_input("FromRotation", rotation_type);
  set_input("ToRotation", rotation_type);
  set_input("Duration", true);
  set_input("Ignore Timescale", true);

  set_output("Done", !on_demand);
  set_output("Reached", !on_demand);
  set_output("ResultFloat", float_type);
  set_output("ResultVector", vector_type);
  set_output("ResultRotation", rotation_type);
  set_output("Factor", true);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeTweenValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  ntype.blend_write_storage_content = logic::curve_mapping_blend_write;
  ntype.blend_data_read_storage_content = logic::curve_mapping_blend_read;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_tween_value_cc
