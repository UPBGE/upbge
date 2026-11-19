/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_value_invert_cc {
namespace decl = blender::nodes::logic::decl;

/* One row, as Blender's Invert Color (Compositor) and Invert Rotation: the input shares the
 * output's row and shows no number field (a typed number would only be typed negative elsewhere;
 * a value saved in an older file still counts). */
static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Float>("Value"_ustr)
      .default_value(0.0f)
      .hide_value()
      .description(
          "The number to multiply by -1. To flip true/false, use Boolean Combine's Not: a true "
          "linked here is 1, gives -1 and still reads as true");
  b.add_output<decl::Float>("Value"_ustr)
      .align_with_previous()
      .description("The input multiplied by -1");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeInvertValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_value_invert_cc