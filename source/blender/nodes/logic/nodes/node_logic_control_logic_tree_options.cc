/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Control Logic Tree (LogicNativeControlLogicTree,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: StartLogicTree, StopLogicTree,
 * RunLogicTree, InstallLogicTree. They are not node types of their own: logic_option_register
 * keeps them for the merged node, which forwards its declaration, buttons, init and update to the
 * option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_start_logic_tree_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Object that owns the logic tree; defaults to the owner when unset");
  b.add_input<decl::String>("Tree Name"_ustr).default_value("").description(
      "Compiled logic tree ID name (matches the LogicNodeTree data-block name)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeStartLogicTree"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_start_logic_tree_cc

namespace blender::nodes::node_logic_stop_logic_tree_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Object that owns the logic tree; defaults to the owner when unset");
  b.add_input<decl::String>("Tree Name"_ustr).default_value("").description(
      "Compiled logic tree ID name (matches the LogicNodeTree data-block name)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeStopLogicTree"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_stop_logic_tree_cc

namespace blender::nodes::node_logic_run_logic_tree_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Object that owns the logic tree; defaults to the owner when unset");
  b.add_input<decl::String>("Tree Name"_ustr).default_value("").description(
      "Compiled logic tree ID name (matches the LogicNodeTree data-block name)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeRunLogicTree"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_run_logic_tree_cc

namespace blender::nodes::node_logic_install_logic_tree_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Object that receives the logic tree; defaults to the owner when unset");
  b.add_input<decl::String>("Tree Name"_ustr).default_value("").description(
      "LogicNodeTree to compile and attach (by ID name)");
  b.add_input<decl::Bool>("Initialize"_ustr).default_value(true).description(
      "Whether the installed tree starts enabled");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeInstallLogicTree"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_install_logic_tree_cc
