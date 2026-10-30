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

#include "node_util.hh"

namespace blender::nodes::node_logic_float_curve_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Factor"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .no_muted_links()
      .description("Influence of the curve mapping; linked values may extrapolate");
  b.add_input<decl::Float>("Value"_ustr)
      .default_value(1.0f)
      .is_default_link_socket()
      .description("Horizontal position at which to evaluate the curve");
  b.add_output<decl::Float>("Value"_ustr).description("Curve-mapped value");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  bNode &node = *static_cast<bNode *>(ptr->data);
  logic::curve_mapping_ensure(node);
  template_curve_mapping(&layout, ptr, "mapping", 0, false, false, false, false, false);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->storage = logic::curve_mapping_create_default();
}

static void node_update(bNodeTree * /*tree*/, bNode *node)
{
  logic::curve_mapping_ensure(*node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFloatCurve"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  ntype.blend_write_storage_content = logic::curve_mapping_blend_write;
  ntype.blend_data_read_storage_content = logic::curve_mapping_blend_read;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_float_curve_cc
