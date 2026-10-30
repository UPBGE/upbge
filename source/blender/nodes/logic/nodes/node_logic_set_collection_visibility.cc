/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_collection_visibility_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Collection in the logic-tree owner's running scene");
  b.add_input<decl::Bool>("Visible"_ustr)
      .default_value(true)
      .description("Show or hide the collection for runtime rendering and pointer hit testing; "
                   "logic and physics remain active");
  b.add_input<decl::Bool>("Include Child Collections"_ustr)
      .default_value(false)
      .description("Apply the runtime render override recursively to child collections");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues only after the runtime visibility override was applied");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetCollectionVisibility"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_collection_visibility_cc
