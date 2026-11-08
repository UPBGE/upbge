/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Overlay Collection (LogicNativeOverlayCollection,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetOverlayCollection,
 * RemoveOverlayCollection. They are not node types of their own: logic_option_register keeps them
 * for the merged node, which forwards its declaration, buttons, init and update to the option it
 * holds (node_logic_merged.cc).
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

  logic_option_type_base(&ntype, "LogicNativeSetOverlayCollection"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_overlay_collection_cc

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

  logic_option_type_base(&ntype, "LogicNativeRemoveOverlayCollection"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_remove_overlay_collection_cc
