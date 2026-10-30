/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_split_screen_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Off: one full view of player 0 (menus, cutscenes, results). The players "
                   "keep their cameras");
  b.add_input<decl::Int>("Divider"_ustr)
      .default_value(0)
      .min(0)
      .max(100)
      .description("Pixels between the views. The gap is black (the scene's framing color)");
  b.add_input<decl::Bool>("Keep Mouse in View"_ustr)
      .default_value(true)
      .description("While the window is split, the mouse cursor can't leave player 0's view (the "
                   "mouse is theirs and works only there). It is free while the game is paused, "
                   "so a pause menu over the whole window can be used. On Linux with Wayland "
                   "only a hidden cursor (Show Mouse off) can be kept: a shown one still moves "
                   "out, but does nothing there");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "split_two_players", UI_ITEM_NONE, "2 Players", ICON_NONE);
  layout.prop(ptr, "split_three_players", UI_ITEM_NONE, "3 Players", ICON_NONE);
  layout.prop(ptr, "split_field_of_view", UI_ITEM_NONE, "View", ICON_NONE);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetSplitScreen"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_split_screen_cc
