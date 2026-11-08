/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Save Variable (LogicNativeVariableSave, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: SaveVariable, SaveVariableDict. They are not node types
 * of their own: logic_option_register keeps them for the merged node, which forwards its
 * declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_save_variable_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Path"_ustr).default_value("//Data");
  b.add_input<decl::String>("File"_ustr).default_value("variables");
  b.add_input<decl::String>("Name"_ustr).default_value("var");
  b.add_input<decl::Generic>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_option_type_base(&ntype, "LogicNativeSaveVariable"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_save_variable_cc

namespace blender::nodes::node_logic_save_variable_dict_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Path"_ustr).default_value("//Data");
  b.add_input<decl::String>("File"_ustr).default_value("variables");
  b.add_input<decl::Dictionary>("Variables"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_option_type_base(&ntype, "LogicNativeSaveVariableDict"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_save_variable_dict_cc
