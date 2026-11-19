/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_vector_math_cc {
namespace decl = blender::nodes::logic::decl;

/* Logic Nodes' own operations (custom1; LN_VECTOR_MATH_* in LN_Types.h, rna_nodetree.cc). */
constexpr int16_t ANGLE = 100;
constexpr int16_t SIGNED_ANGLE = 101;
constexpr int16_t LIMIT_LENGTH = 102;
constexpr int16_t MOVE_TOWARDS = 103;

static bool gives_number(const int16_t operation)
{
  return ELEM(operation,
              NODE_VECTOR_MATH_DOT_PRODUCT,
              NODE_VECTOR_MATH_LENGTH,
              NODE_VECTOR_MATH_DISTANCE,
              ANGLE,
              SIGNED_ANGLE);
}

static bool reads_b(const int16_t operation)
{
  return !ELEM(operation,
               NODE_VECTOR_MATH_SCALE,
               NODE_VECTOR_MATH_NORMALIZE,
               NODE_VECTOR_MATH_ABSOLUTE,
               NODE_VECTOR_MATH_LENGTH,
               LIMIT_LENGTH);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("A"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Vector>("B"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(1.0f)
      .description("Scale: the factor; Limit Length: the longest length; Move Towards: the "
                   "farthest step");
  b.add_input<decl::Vector>("Axis"_ustr)
      .default_value({0.0f, 0.0f, 1.0f})
      .description("Signed Angle: the axis the turn is measured around (up by default)");
  b.add_output<decl::Vector>("Result"_ustr);
  b.add_output<decl::Float>("Value"_ustr)
      .description("Dot Product, Length, Distance, Angle and Signed Angle's number");
}

/* Only the sockets the operation uses show (as Blender's Vector Math). A hidden link still
 * compiles as before, only into what the operation reads. */
static void node_update(bNodeTree *tree, bNode *node)
{
  const int16_t operation = node->custom1;
  auto input = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  auto output = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->output_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  input("B"_ustr, reads_b(operation));
  input("Scale"_ustr, ELEM(operation, NODE_VECTOR_MATH_SCALE, LIMIT_LENGTH, MOVE_TOWARDS));
  input("Axis"_ustr, operation == SIGNED_ANGLE);
  output("Result"_ustr, !gives_number(operation));
  output("Value"_ustr, gives_number(operation));
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = NODE_VECTOR_MATH_ADD;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeVectorMath"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_vector_math_cc
