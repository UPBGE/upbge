/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_player_gamepad_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player who gets the gamepad");
  b.add_input<decl::Int>("Gamepad"_ustr)
      .default_value(0)
      .min(-1)
      .max(7)
      .description("The gamepad's number (0 is the first one plugged in). -1 takes the player's "
                   "gamepad away: the player left, and joins again when given one. A player who "
                   "had this gamepad loses it");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetPlayerGamepad"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_player_gamepad_cc
