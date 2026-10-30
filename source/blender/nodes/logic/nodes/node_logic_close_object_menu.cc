/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_close_object_menu_cc {
namespace decl = blender::nodes::logic::decl;

enum CloseOperation {
  Top = 0,
  Specific = 1,
  All = 2,
};

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Int>("Menu"_ustr)
      .min(1)
      .description("Session handle returned by Open Object Menu");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("Top and All: whose menus to close. In a menu's own logic: that menu's player");
  b.add_output<decl::Execution>("Closed"_ustr)
      .description("Runs after the runtime confirms that the menu is closed");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs when the handle was never allocated or closing failed");
  b.add_output<decl::Int>("Result"_ustr)
      .description("Stable result code; closing an already-closed valid handle is No Change");
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "menu_close_operation", UI_ITEM_NONE, "Close", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = Top;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *menu = node->input_by_identifier("Menu"_ustr);
  if (menu != nullptr) {
    bke::node_set_socket_availability(*ntree, *menu, node->custom1 == Specific);
  }
  /* A handle names one menu, whoever's it is. */
  if (bNodeSocket *user = node->input_by_identifier("User"_ustr)) {
    bke::node_set_socket_availability(*ntree, *user, node->custom1 != Specific);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeCloseObjectMenu"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_close_object_menu_cc
