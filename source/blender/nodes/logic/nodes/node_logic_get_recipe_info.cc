/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_recipe_info_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Recipe"_ustr)
      .description("One recipe line, such as \"2 Wood + Stone = Axe\" or \"Apple + Mushroom = "
                   "Potion @ Kitchen\"");
  b.add_input<decl::List>("Inventory"_ustr)
      .description("Optional: the inventory Can Craft and Craftable look in");
  b.add_output<decl::Condition>("Valid"_ustr).description("The text is a recipe");
  b.add_output<decl::String>("Result"_ustr).description("The item it makes (the first one)");
  b.add_output<decl::Int>("Count"_ustr).description("How many of it");
  b.add_output<decl::List>("Results"_ustr)
      .description("Everything it makes as {item, count} slots, such as Butter and Bucket of "
                   "\"Milk Bucket = Butter + Bucket\"");
  b.add_output<decl::List>("Ingredients"_ustr)
      .description("Its ingredients as {item, count} slots, ready for a Grid View");
  b.add_output<decl::String>("Station"_ustr)
      .description("The station written after @; empty when it needs none");
  b.add_output<decl::Condition>("Can Craft"_ustr)
      .description("The inventory holds every ingredient");
  b.add_output<decl::Int>("Craftable"_ustr)
      .description("How many times in a row the inventory has the ingredients (0: not once). An "
                   "ingredient the recipe gives back, such as a tool, only has to be there; a "
                   "recipe that uses nothing up gives 2147483647");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetRecipeInfo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_recipe_info_cc
