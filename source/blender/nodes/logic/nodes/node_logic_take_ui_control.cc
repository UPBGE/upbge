/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_take_ui_control_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The UI to use, such as a computer in the game world: this object and the UI "
                   "Elements that hang from it (outside menus). Empty: the tree's own object");
  b.add_input<decl::Int>("User"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::Bool>("Take"_ustr)
      .default_value(true)
      .description("On: take it if no other player has it. While a player has it, only they "
                   "can use its UI Elements (other players and the mouse can't), and their keys "
                   "and gamepad stay on it, starting on its first element. Off: give it back. "
                   "It is free again when the object is removed");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Runs once the player has it (or gave it back)");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs if another player has it, or the object is gone");
  b.add_output<decl::Int>("Result"_ustr)
      .description("0: done, 1: nothing changed (the player already had it, or did not have it), "
                   "20: another player has it, 13: the object or User is not valid");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeTakeUIControl"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_take_ui_control_cc
