/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_player_gamepad_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("User"_ustr).default_value(0).min(0).max(7).description("The player");
  b.add_output<decl::Int>("Gamepad"_ustr)
      .description("The player's gamepad number, for the Gamepad nodes' Index; -1 for none");
  b.add_output<decl::Bool>("Connected"_ustr)
      .description("The player's gamepad is plugged in. Unplugged, the player keeps it");
  b.add_output<decl::Bool>("Joined"_ustr)
      .description("The player joined (On Player Join ran for them), until Set Player Gamepad "
                   "takes their gamepad away or gives it to another player");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetPlayerGamepad"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_player_gamepad_cc
