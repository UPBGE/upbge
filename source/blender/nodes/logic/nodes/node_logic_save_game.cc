/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_save_game_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("Slot"_ustr).default_value(0);
  b.add_input<decl::String>("Path"_ustr)
      .default_value("")
      .description(
          "Checkpoint file or directory; use the same explicit path in different scenes for "
          "cross-scene loading");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Bool>("Success"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSaveGame"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_save_game_cc
