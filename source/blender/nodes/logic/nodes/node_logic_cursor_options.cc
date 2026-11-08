/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Cursor (LogicNativeCursor, NOD_logic_merged.hh), each the node it
 * was before 2026-11-08: CursorPosition, CursorMovement. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_cursor_position_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Position"_ustr);
  b.add_output<decl::Float>("X"_ustr);
  b.add_output<decl::Float>("Y"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "invert_y", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeCursorPosition"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_cursor_position_cc

namespace blender::nodes::node_logic_cursor_movement_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Movement"_ustr);
  b.add_output<decl::Float>("X"_ustr);
  b.add_output<decl::Float>("Y"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeCursorMovement"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_cursor_movement_cc
