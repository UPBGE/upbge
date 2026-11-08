/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Load Variable (LogicNativeVariableLoad, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: LoadVariable, LoadVariableDict. They are not node types
 * of their own: logic_option_register keeps them for the merged node, which forwards its
 * declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_load_variable_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path"_ustr).default_value("//Data");
  b.add_input<decl::String>("File"_ustr).default_value("variables");
  b.add_input<decl::String>("Name"_ustr).default_value("var");
  b.add_input<decl::Generic>("Default Value"_ustr);
  b.add_output<decl::Generic>("Value"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_option_type_base(&ntype, "LogicNativeLoadVariable"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_load_variable_cc

namespace blender::nodes::node_logic_load_variable_dict_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path"_ustr).default_value("//Data");
  b.add_input<decl::String>("File"_ustr).default_value("variables");
  b.add_output<decl::Dictionary>("Variables"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_option_type_base(&ntype, "LogicNativeLoadVariableDict"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_load_variable_dict_cc
