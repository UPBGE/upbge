/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_menu_player_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("User"_ustr)
      .description("The player whose menu this tree's object is in: with per-player menus, "
                   "each player's copy of the menu gets its own. Outside menus, the Player of "
                   "the UI Element the object is or hangs from, else 0. Link it to "
                   "the User of Input Action and Rebind Input Action in the menu's logic (UI "
                   "events, views and Open/Close Object Menu there already use it)");
  b.add_output<decl::Object>("Owner"_ustr)
      .description("Whose menu it is: Open Object Menu's Owner, else the object whose logic "
                   "opened it, such as that player's character (empty outside menus). Point the "
                   "menu's Inventory and property nodes at it");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetMenuPlayer"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_menu_player_cc
