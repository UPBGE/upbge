/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * List Operation (2026-11-17). Operation (custom1): Reverse 0, Shuffle 1, Slice 2 (a new list),
 * Min 3, Max 4, Sum 5, Average 6 (over the numbers in the list; Min and Max also give where).
 * Shuffle's Variation in custom2 (0 Per Object, 1 Same for All), as Get Random List Item. Each
 * operation shows only the sockets it uses.
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_list_operation_cc {
namespace decl = blender::nodes::logic::decl;

enum Operation : int16_t {
  REVERSE = 0,
  SHUFFLE = 1,
  SLICE = 2,
  MIN = 3,
  MAX = 4,
  SUM = 5,
  AVERAGE = 6,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::List>("List"_ustr);
  b.add_input<decl::Int>("Start"_ustr)
      .default_value(0)
      .description("Slice: the first item taken (0 the first; below 0 counts from the end, -1 "
                   "the last)");
  b.add_input<decl::Int>("Length"_ustr)
      .default_value(-1)
      .description("Slice: how many items it takes (below 0: all the rest)");
  b.add_input<decl::Int>("Seed"_ustr)
      .default_value(0)
      .description("Shuffle: another number gives another order. The order is new every step "
                   "(or frame) and the same in every play with the same game seed");
  b.add_output<decl::List>("List"_ustr).description("The changed copy of the list");
  b.add_output<decl::Float>("Value"_ustr)
      .description("Over the numbers in the list (whole or decimal; other items are skipped); "
                   "0 when it has none");
  b.add_output<decl::Int>("Index"_ustr)
      .description("Where the first smallest (Min) or largest (Max) number is (0 the first), -1 "
                   "when the list has no numbers");
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int16_t operation = node->custom1;
  const auto input = [&](const UString identifier, const bool shown) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, shown);
    }
  };
  const auto output = [&](const UString identifier, const bool shown) {
    if (bNodeSocket *socket = node->output_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, shown);
    }
  };
  input("Start"_ustr, operation == SLICE);
  input("Length"_ustr, operation == SLICE);
  input("Seed"_ustr, operation == SHUFFLE);
  output("List"_ustr, ELEM(operation, REVERSE, SHUFFLE, SLICE));
  output("Value"_ustr, ELEM(operation, MIN, MAX, SUM, AVERAGE));
  output("Index"_ustr, ELEM(operation, MIN, MAX));
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
  if (ptr->data_as<bNode>()->custom1 == SHUFFLE) {
    layout.prop(ptr, "variation", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = REVERSE;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeListOperation"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_operation_cc
