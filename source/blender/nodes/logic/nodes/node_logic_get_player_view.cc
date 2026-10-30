/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_player_view_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player: 0 has the keyboard and mouse. Gamepads: Scene > Game Input Map "
                   "(by default player N has gamepad N)");
  b.add_output<decl::Object>("Camera"_ustr)
      .description("The camera the player sees through (player 0: the active camera)");
  b.add_output<decl::Bool>("Shown"_ustr).description("The player's view is on screen");
  b.add_output<decl::Float>("Left"_ustr)
      .description("Where the player's view starts, from the window's left edge (0 to 1)");
  b.add_output<decl::Float>("Top"_ustr)
      .description("Where the player's view starts, from the window's top edge (0 to 1)");
  b.add_output<decl::Float>("Width"_ustr)
      .description("The view's width as a part of the window's width (0 to 1)");
  b.add_output<decl::Float>("Height"_ustr)
      .description("The view's height as a part of the window's height (0 to 1)");
  b.add_output<decl::Int>("Views"_ustr).description("How many player views are on screen");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetPlayerView"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_player_view_cc
