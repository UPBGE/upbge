/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Get Game Property (LogicNativeGetGamePropertyValue,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: GetGamePropertyFloat,
 * GetGamePropertyInt, GetGamePropertyBool, GetGamePropertyString, GetGameProperty. They are not
 * node types of their own: logic_option_register keeps them for the merged node, which forwards
 * its declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_game_property_float_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_output<decl::Float>("Property Value"_ustr, "Value"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeGetGamePropertyFloat"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_game_property_float_cc

namespace blender::nodes::node_logic_get_game_property_int_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_output<decl::Int>("Property Value"_ustr, "Value"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeGetGamePropertyInt"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_game_property_int_cc

namespace blender::nodes::node_logic_get_game_property_bool_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_output<decl::Bool>("Property Value"_ustr, "Value"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeGetGamePropertyBool"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_game_property_bool_cc

namespace blender::nodes::node_logic_get_game_property_string_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_output<decl::String>("Property Value"_ustr, "Value"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeGetGamePropertyString"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_game_property_string_cc

namespace blender::nodes::node_logic_get_game_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description("Empty uses the owner");
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_output<decl::Generic>("Value"_ustr)
      .description("The property's value of any type, such as a List, Table or Object");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetGameProperty"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_game_property_cc
