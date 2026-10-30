/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_on_ui_sound_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Execution>("Move"_ustr)
      .description("Runs when a player's highlight moves to another UI Element (mouse, keys or "
                   "gamepad), not when a menu opens");
  b.add_output<decl::Execution>("Accept"_ustr)
      .description("Runs for a click or right click on a UI Element (also Enter, Space or the "
                   "gamepad's A), a Toggle or Selector the player changed, a finished Text "
                   "Field, a drop that moved an item");
  b.add_output<decl::Execution>("Back"_ustr)
      .description("Runs when Back closes a menu (Back: Close Menu). Put this node outside that "
                   "menu, in the game's logic: the menu is gone by then");
  b.add_output<decl::Execution>("Denied"_ustr)
      .description("Runs for a press that does nothing: a disabled UI Element, a drag that goes "
                   "back, a drop that changed nothing");
  b.add_output<decl::Object>("Object"_ustr)
      .description("The UI Element the sound is for (empty for Back)");
  b.add_output<decl::String>("Action"_ustr)
      .description("That UI Element's Action name, to give some elements their own sound "
                   "(Switch Flow (String))");
  b.add_output<decl::String>("Menu"_ustr)
      .description("The name of the menu's collection the sound came from (for Back, the menu "
                   "it closed), to give each menu its own sounds; empty outside menus");
  b.add_output<decl::Int>("User"_ustr)
      .description("The player (0-7) whose UI made the sound");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeOnUISound"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_on_ui_sound_cc
