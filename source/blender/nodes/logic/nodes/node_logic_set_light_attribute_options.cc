/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Light Attribute (LogicNativeSetLightAttribute,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetLightColor, SetLightPower,
 * SetLightShadow. They are not node types of their own: logic_option_register keeps them for the
 * merged node, which forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_light_color_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Light Object"_ustr, "Object"_ustr).description(
      "Target light object; defaults to the owner when unset");
  b.add_input<decl::Color>("Color"_ustr).description("Target light RGB color");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetLightColor"_ustr);
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_light_color_cc

namespace blender::nodes::node_logic_set_light_power_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Light Object"_ustr, "Object"_ustr).description(
      "Target light object; defaults to the owner when unset");
  b.add_input<decl::Float>("Power"_ustr).default_value(1.0f).description("Target light power");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetLightPower"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_light_power_cc

namespace blender::nodes::node_logic_set_light_shadow_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Light Object"_ustr, "Object"_ustr).description(
      "Target light object; defaults to the owner when unset");
  b.add_input<decl::Bool>("Use Shadow"_ustr).default_value(true).description(
      "Enable shadow casting for the target light");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetLightShadow"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_light_shadow_cc
