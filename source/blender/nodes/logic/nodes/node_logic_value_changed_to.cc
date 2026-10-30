/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_value_changed_to_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Value sampled once per fixed Logic + Physics step");
  b.add_input<decl::Generic>("Target"_ustr)
      .description("Target value to detect; leave unlinked to detect a change to None");
  b.add_output<decl::Execution>("If Changed To"_ustr, "Result"_ustr)
      .description(
          "Emits for one fixed Logic + Physics step when Value reaches Target; numeric values also emit when "
          "they cross Target between samples");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeValueChangedTo"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_value_changed_to_cc
