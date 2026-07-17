/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_add_object_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).default_value(true);
  b.add_input<decl::Object>("Object to Add"_ustr).description("Scene object to duplicate");
  b.add_input<decl::Object>("Copy Transform"_ustr).description(
      "Transform reference; defaults to the owner when unset");
  b.add_input<decl::Float>("Life"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Lifetime in seconds; zero keeps the object alive until removed");
  b.add_input<decl::Bool>("Full Copy"_ustr).default_value(false);
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Object>("Added Object"_ustr, "Object"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeAddObject"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_add_object_cc
