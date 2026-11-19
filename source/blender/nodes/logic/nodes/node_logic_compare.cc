/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_compare_cc {
namespace decl = blender::nodes::logic::decl;

/* Type (custom2, 2026-11-16): Number 0 (every older node), Vector 1, Object 2, Text 3. Each type
 * has its own sockets with names of their own (the compiler finds a socket by name or
 * identifier). */
enum { TYPE_NUMBER = 0, TYPE_VECTOR = 1, TYPE_OBJECT = 2, TYPE_TEXT = 3 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("A"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("B"_ustr).default_value(0.0f);
  b.add_input<decl::Vector>("Vector A"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Vector>("Vector B"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Float>("Tolerance"_ustr)
      .default_value(0.0001f)
      .min(0.0f)
      .description("Equal: the vectors are at most this far apart (Not Equal: farther)");
  b.add_input<decl::Object>("Object A"_ustr)
      .description("Empty: nothing (Get Owner gives the own object); a removed object is "
                   "nothing too");
  b.add_input<decl::Object>("Object B"_ustr)
      .description("Empty: nothing (Get Owner gives the own object); a removed object is "
                   "nothing too");
  b.add_input<decl::String>("Text A"_ustr).default_value("");
  b.add_input<decl::String>("Text B"_ustr).default_value("");
  b.add_output<decl::Bool>("Result"_ustr);
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int16_t type = node->custom2;
  auto input = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  input("A"_ustr, type == TYPE_NUMBER);
  input("B"_ustr, type == TYPE_NUMBER);
  input("Vector A"_ustr, type == TYPE_VECTOR);
  input("Vector B"_ustr, type == TYPE_VECTOR);
  input("Tolerance"_ustr, type == TYPE_VECTOR);
  input("Object A"_ustr, type == TYPE_OBJECT);
  input("Object B"_ustr, type == TYPE_OBJECT);
  input("Text A"_ustr, type == TYPE_TEXT);
  input("Text B"_ustr, type == TYPE_TEXT);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeCompare"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_compare_cc