/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * String Operation. Operation (custom1): Join 0, Contains 2, Count 3, Replace 4, Starts With 5,
 * Ends With 6, To Uppercase 7, To Lowercase 8, Prepend Zeroes 9; Substring 10, Length 11, Find
 * 12, Split 13, Format Time 14 (2026-11-17, counted in characters: LN_TextOps.h). Format Time's
 * Time Format in custom2. Each operation shows only the sockets it uses (2026-11-17; a link into
 * or out of a hidden socket of an older file still runs).
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_string_operation_cc {
namespace decl = blender::nodes::logic::decl;

enum Operation : int16_t {
  JOIN = 0,
  CONTAINS = 2,
  COUNT = 3,
  REPLACE = 4,
  STARTS_WITH = 5,
  ENDS_WITH = 6,
  UPPER = 7,
  LOWER = 8,
  ZFILL = 9,
  SUBSTRING = 10,
  LENGTH = 11,
  FIND = 12,
  SPLIT = 13,
  FORMAT_TIME = 14,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String"_ustr).default_value("").description("The text");
  b.add_input<decl::String>("Substring"_ustr)
      .default_value("")
      .description("Join: the text added at the end. Contains, Count, Replace, Starts With, Ends "
                   "With, Find: the text looked for");
  b.add_input<decl::String>("Replacement"_ustr)
      .default_value("")
      .description("Replace: the text put in its place");
  b.add_input<decl::Int>("Length"_ustr)
      .default_value(0)
      .description("Prepend Zeroes: how long the text becomes. Substring: how many characters it "
                   "takes (below 0: all the rest)");
  b.add_input<decl::Int>("Start"_ustr)
      .default_value(0)
      .description("Substring: the first character taken (0 the first; below 0 counts from the "
                   "end, -1 the last)");
  b.add_input<decl::String>("Separator"_ustr)
      .default_value(",")
      .description("Split: where the text is cut (empty: at spaces, tabs and new lines)");
  b.add_input<decl::Float>("Seconds"_ustr)
      .default_value(0.0f)
      .description("Format Time: the time shown as a clock (a Timer, a countdown)");
  b.add_input<decl::Bool>("Round Up"_ustr)
      .default_value(false)
      .description("Format Time: round up instead of cutting down (a countdown shows 0:01 until "
                   "it reaches 0)");
  b.add_output<decl::String>("String"_ustr);
  b.add_output<decl::Bool>("Result"_ustr);
  b.add_output<decl::Int>("Count"_ustr).description("Count: how often the substring appears");
  b.add_output<decl::Int>("Length"_ustr).description("How many characters the text has");
  b.add_output<decl::Int>("Index"_ustr)
      .description("Where the substring first appears (0 the first character), -1 when it does "
                   "not");
  b.add_output<decl::List>("List"_ustr).description("The parts between the separators, in order");
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int16_t operation = node->custom1;
  const bool looks_for = ELEM(operation, JOIN, CONTAINS, COUNT, REPLACE, STARTS_WITH, ENDS_WITH, FIND);
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
  input("String"_ustr, operation != FORMAT_TIME);
  input("Substring"_ustr, looks_for);
  input("Replacement"_ustr, operation == REPLACE);
  input("Length"_ustr, ELEM(operation, ZFILL, SUBSTRING));
  input("Start"_ustr, operation == SUBSTRING);
  input("Separator"_ustr, operation == SPLIT);
  input("Seconds"_ustr, operation == FORMAT_TIME);
  input("Round Up"_ustr, operation == FORMAT_TIME);
  output("String"_ustr,
         ELEM(operation, JOIN, REPLACE, UPPER, LOWER, ZFILL, SUBSTRING, FORMAT_TIME));
  output("Result"_ustr, ELEM(operation, CONTAINS, STARTS_WITH, ENDS_WITH));
  output("Count"_ustr, operation == COUNT);
  output("Length"_ustr, operation == LENGTH);
  output("Index"_ustr, operation == FIND);
  output("List"_ustr, operation == SPLIT);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
  if (ptr->data_as<bNode>()->custom1 == FORMAT_TIME) {
    layout.prop(ptr, "time_format", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeStringOperation"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_string_operation_cc
