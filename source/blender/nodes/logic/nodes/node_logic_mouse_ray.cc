/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_mouse_ray_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Camera"_ustr).available(false);
  b.add_input<decl::String>("Property"_ustr);
  b.add_input<decl::Bool>("X-Ray"_ustr).default_value(false);
  b.add_input<decl::Float>("Distance"_ustr).default_value(100.0f);
  b.add_input<decl::CollisionLayers>("Mask"_ustr).default_value(1023).min(0).max(1023);
  b.add_output<decl::Condition>("Has Result"_ustr);
  b.add_output<decl::Object>("Picked Object"_ustr);
  b.add_output<decl::Vector>("Picked Point"_ustr);
  b.add_output<decl::Vector>("Picked Normal"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeMouseRay"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_ray_cc
