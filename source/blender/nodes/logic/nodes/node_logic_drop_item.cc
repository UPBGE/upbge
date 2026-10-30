/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_drop_item_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Item"_ustr)
      .description("Item name, an inventory slot (the one a UI event reports), or an object: a "
                   "pickup's item and count properties");
  b.add_input<decl::Int>("Amount"_ustr)
      .default_value(0)
      .min(0)
      .description("How many the dropped copy holds (its count property); 0 is all the item "
                   "carries: a whole slot, or a pickup's count");
  b.add_input<decl::Vector>("Position"_ustr)
      .description("World position of the copy, such as On UI Event's Point where it was let go");
  b.add_input<decl::Collection>("Library"_ustr)
      .description("Item library: the object named like the item is copied; empty uses the "
                   "scene's Item Library");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Runs when the item is dropped, not when it can't be (no such item in the "
                   "library); from a UI event the copy appears at the next Logic + Physics step "
                   "(while the game is paused, at the end of the frame)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeDropItem"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_drop_item_cc
