/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_mouse_over_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description("Object to test against the cursor ray");
  b.add_output<decl::Execution>("On Enter"_ustr)
      .description("Execution pulse on the tick the cursor enters the object");
  b.add_output<decl::Execution>("On Over"_ustr)
      .description("Execution pulse every tick while the cursor remains over the object");
  b.add_output<decl::Execution>("On Exit"_ustr)
      .description("Execution pulse on the tick the cursor leaves the object");
  b.add_output<decl::Condition>("Entered"_ustr)
      .description("True on the tick the cursor enters the object");
  b.add_output<decl::Condition>("Is Over"_ustr)
      .description("True while the cursor remains over the object");
  b.add_output<decl::Condition>("Exited"_ustr)
      .description("True on the tick the cursor leaves the object");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("World-space hit point while over the object");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("World-space hit normal while over the object");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeMouseOver"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_over_cc
