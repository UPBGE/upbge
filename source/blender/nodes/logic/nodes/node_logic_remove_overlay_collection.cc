/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_remove_overlay_collection_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Active overlay collection to remove, including its overlay-only replicas");
  b.add_output<decl::Execution>("Success"_ustr)
      .description("Runs after the runtime confirms that the independent overlay request was "
                   "cleared");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs after the runtime rejects or fails the request");
  b.add_output<decl::Int>("Result"_ustr)
      .description("Stable result code; repeating remove when already disabled is No Change");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRemoveOverlayCollection"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_remove_overlay_collection_cc
