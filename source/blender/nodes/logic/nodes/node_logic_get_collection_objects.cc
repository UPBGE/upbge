/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_collection_objects_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Collection>("Collection"_ustr);
  b.add_input<decl::Bool>("Include Child Collections"_ustr)
      .default_value(false)
      .description("Also the objects in the collection's child collections, such as copies "
                   "of templates kept in a hidden child collection");
  b.add_output<decl::List>("Objects"_ustr)
      .description("The scene's objects in the collection while the game runs: those linked "
                   "to it in the file first, then Add Object copies of its objects and objects "
                   "Set Collection Member put in; without those it took out");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeGetCollectionObjects"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_collection_objects_cc
