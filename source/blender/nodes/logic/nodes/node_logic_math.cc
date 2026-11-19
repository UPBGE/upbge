/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_util.hh"

namespace blender::nodes::node_logic_math_cc {
namespace decl = blender::nodes::logic::decl;

/* Logic Nodes' own operation (custom1; LN_MATH_ANGLE_DIFFERENCE in LN_Types.h). */
constexpr int16_t ANGLE_DIFFERENCE = 100;

static bool reads_b(const int16_t operation)
{
  return ELEM(operation,
              NODE_MATH_ADD,
              NODE_MATH_SUBTRACT,
              NODE_MATH_MULTIPLY,
              NODE_MATH_DIVIDE,
              NODE_MATH_POWER,
              NODE_MATH_MINIMUM,
              NODE_MATH_MAXIMUM,
              NODE_MATH_MODULO,
              NODE_MATH_ARCTAN2,
              NODE_MATH_WRAP,
              NODE_MATH_SNAP,
              NODE_MATH_PINGPONG,
              ANGLE_DIFFERENCE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("A"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("B"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("C"_ustr)
      .default_value(0.0f)
      .description("Wrap's lower end (Min)");
  b.add_output<decl::Float>("Result"_ustr);
}

/* The operation's name as the header (Blender's list for its operations). */
static void node_label(const bNodeTree *tree, const bNode *node, char *label, int label_maxncpy)
{
  if (node->custom1 == ANGLE_DIFFERENCE) {
    BLI_strncpy_utf8(label, IFACE_("Angle Difference"), label_maxncpy);
    return;
  }
  node_math_label(tree, node, label, label_maxncpy);
}

/* Only the inputs the operation reads show (as Blender's Math); Wrap names its inputs Value,
 * Max and Min. A hidden link compiles as before, only into what the operation reads. */
static void node_update(bNodeTree *tree, bNode *node)
{
  const int16_t operation = node->custom1;
  bNodeSocket *a = node->input_by_identifier("A"_ustr);
  bNodeSocket *b = node->input_by_identifier("B"_ustr);
  bNodeSocket *c = node->input_by_identifier("C"_ustr);
  if (b != nullptr) {
    bke::node_set_socket_availability(*tree, *b, reads_b(operation));
  }
  if (c != nullptr) {
    bke::node_set_socket_availability(*tree, *c, operation == NODE_MATH_WRAP);
  }
  if (a != nullptr && b != nullptr && c != nullptr) {
    if (operation == NODE_MATH_WRAP) {
      node_sock_label(a, "Value");
      node_sock_label(b, "Max");
      node_sock_label(c, "Min");
    }
    else {
      node_sock_label_clear(a);
      node_sock_label_clear(b);
      node_sock_label_clear(c);
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = NODE_MATH_ADD;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeMath"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  ntype.labelfunc = node_label;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_math_cc
