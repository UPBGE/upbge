/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_scroll_object_view_cc {
namespace decl = blender::nodes::logic::decl;

/* The scroll_mode enum values in rna_nodetree.cc. */
static constexpr int16_t MODE_SHOW_ITEM = 0;
static constexpr int16_t MODE_LINES = 5;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Viewport Object"_ustr)
      .description("The Viewport Object (or Content Object) of the Object Scroll View or Object "
                   "Grid View to scroll; empty uses the owner");
  b.add_input<decl::Generic>("Item"_ustr)
      .description("Show Item. Grid View: a number is the item's index in its list (as Index and "
                   "Inventory slots count); an item name or slot shows the first item like it, "
                   "such as Inventory's Made after a craft. Scroll View: an object in its content");
  b.add_input<decl::Int>("Lines"_ustr)
      .default_value(1)
      .description("Lines: how many rows to scroll (columns of a horizontal grid, Wheel Steps of "
                   "a Scroll View); negative toward the start");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Runs once the scroll is asked for; the view moves when it next lays out (the "
                   "next frame)");
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "scroll_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = MODE_SHOW_ITEM;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  node->custom1 = std::clamp<int16_t>(node->custom1, MODE_SHOW_ITEM, MODE_LINES);
  if (bNodeSocket *item = node->input_by_identifier("Item"_ustr)) {
    bke::node_set_socket_availability(*ntree, *item, node->custom1 == MODE_SHOW_ITEM);
  }
  if (bNodeSocket *lines = node->input_by_identifier("Lines"_ustr)) {
    bke::node_set_socket_availability(*ntree, *lines, node->custom1 == MODE_LINES);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeScrollObjectView"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_scroll_object_view_cc
