/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_find_recipe_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::List>("Recipes"_ustr)
      .description("Recipe lines such as \"Apple + Mushroom = Potion\"");
  b.add_input<decl::Generic>("Items"_ustr)
      .description("The items to make something of: a list of slots or item names, in any "
                   "order");
  b.add_input<decl::String>("Station"_ustr)
      .description("Only recipes made at this station (written after @); empty for recipes "
                   "without one");
  b.add_output<decl::Condition>("Found"_ustr);
  b.add_output<decl::String>("Recipe"_ustr)
      .description("The first recipe that uses exactly these items, no more of each than given");
  b.add_output<decl::Int>("Index"_ustr).description("Its position in Recipes; -1 when none");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFindRecipe"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_find_recipe_cc
