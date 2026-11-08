/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Get Light Attribute (LogicNativeGetLightAttribute,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: GetLightColor, GetLightPower. They
 * are not node types of their own: logic_option_register keeps them for the merged node, which
 * forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_light_color_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Light Object"_ustr, "Object"_ustr).description(
      "Light object to read, or the owner when empty");
  b.add_output<decl::Color>("Color"_ustr).description("Snapshot light RGB color");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetLightColor"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_light_color_cc

namespace blender::nodes::node_logic_get_light_power_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Light Object"_ustr, "Object"_ustr).description(
      "Light object to read, or the owner when empty");
  b.add_output<decl::Float>("Power"_ustr).description("Snapshot light power");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetLightPower"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_light_power_cc
