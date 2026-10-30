/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_collection_member_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object to put in or take out. Empty: the tree's own object. The "
                   "objects of a collection instance follow the Empty instancing them");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("A collection of the running scene. The file's collections don't change");
  b.add_input<decl::Bool>("Member"_ustr)
      .default_value(true)
      .description("On: from now on the object counts as in the collection, for cameras' "
                   "Collection settings (a minimap's Only, the players' Hide), Set Collection "
                   "Render Visibility and Get Collection Objects. Off: it counts as out of the "
                   "collection and its child collections. Add Object copies made from it keep "
                   "this; Save Game keeps it for objects with Save Object State");
  b.add_input<decl::Bool>("Include Children"_ustr)
      .default_value(false)
      .description("Also the object's children it has now");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues once the object's membership was changed");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetCollectionMember"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_collection_member_cc
