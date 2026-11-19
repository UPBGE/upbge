/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Insert (2026-11-17): puts a value into a list at a place, the items from there on moving one
 * place up (a queue, a turn order, a sorted high score table). Store (custom1) as Append: Game
 * Property changes the list a game property holds, Output Only gives the changed copy.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_list_insert_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description(logic::collection_store_object_description());
  b.add_input<decl::String>("Property"_ustr)
      .description(logic::collection_store_property_description("list"))
      .custom_draw(logic::game_property_field("Object"));
  b.add_input<decl::List>("List"_ustr);
  b.add_input<decl::Int>("Index"_ustr)
      .default_value(0)
      .description("The place the value takes (0 the first; past the end: added at the end; "
                   "below 0 counts from the end, -1 before the last item). To add at the end, "
                   "use Append");
  b.add_input<decl::Generic>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::List>("List"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeListInsert"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = logic::collection_store_layout;
  ntype.initfunc = logic::collection_store_init;
  ntype.updatefunc = logic::collection_store_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_insert_cc
