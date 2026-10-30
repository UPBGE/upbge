/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_cursor_visibility_cc {
namespace decl = blender::nodes::logic::decl;

enum CursorMode {
  /** The Visible input decides. */
  Set = 0,
  /** Shown only while a menu or a UI in the game world uses player 0's mouse. */
  InMenus = 1,
};

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Bool>("Visible"_ustr).default_value(true);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "cursor_mode", UI_ITEM_NONE, "Show", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = Set;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *visible = node->input_by_identifier("Visible"_ustr)) {
    bke::node_set_socket_availability(*ntree, *visible, node->custom1 != InMenus);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype,
                       "LogicNativeSetCursorVisibility"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_cursor_visibility_cc
