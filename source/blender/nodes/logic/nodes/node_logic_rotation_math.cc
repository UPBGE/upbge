/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Rotation Math (2026-11-16): custom1 Combine 0, Combine Local 1, Invert 2, Difference 3
 * (Vector::RotationFunction, LN_MathOps.h).
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_rotation_math_cc {
namespace decl = blender::nodes::logic::decl;

constexpr int16_t INVERT = 2;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Rotation>("A"_ustr).description("The first rotation (Euler radians)");
  b.add_input<decl::Rotation>("B"_ustr).description(
      "The second rotation (Euler radians); Invert reads none");
  b.add_output<decl::Rotation>("Rotation"_ustr).description("The resulting rotation");
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (bNodeSocket *b = node->input_by_identifier("B"_ustr)) {
    bke::node_set_socket_availability(*tree, *b, node->custom1 != INVERT);
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRotationMath"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_rotation_math_cc
