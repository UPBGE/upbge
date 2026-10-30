/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_on_player_join_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Execution>("Out"_ustr)
      .description("When a player's gamepad presses a button for the first time (with Press to "
                   "Join: when a gamepad gets a player). A scene that starts later (Restart "
                   "Scene, the next level) also gets one for each player already in. One a "
                   "frame; a join while the game is paused comes when it runs again");
  b.add_output<decl::Int>("User"_ustr).description("The player who joined");
  b.add_output<decl::Int>("Gamepad"_ustr).description("Their gamepad's number");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOnPlayerJoin"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_player_join_cc
