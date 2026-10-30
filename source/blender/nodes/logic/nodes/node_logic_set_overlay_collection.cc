/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_overlay_collection_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Dedicated inactive camera for the overlay pass; empty reuses the established "
                   "overlay camera after the first active overlay");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Same-scene collection to render only in the overlay pass, including child "
                   "collections; objects may be authored hidden but the collection must remain "
                   "included in the scene view layer so the game can convert it");
  b.add_output<decl::Execution>("Success"_ustr)
      .description("Runs after the runtime confirms that the overlay request committed");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs after the runtime rejects or fails the request");
  b.add_output<decl::Int>("Result"_ustr)
      .description("Stable result code for the completed operation; zero is Success");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetOverlayCollection"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_overlay_collection_cc
