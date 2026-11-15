/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_list_global_properties_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Category"_ustr)
      .description("The group of global properties to list. Pick one the file's global property "
                   "nodes use, or type any name")
      .custom_draw(logic::global_category_field());
  b.add_input<decl::Bool>("Print"_ustr)
      .default_value(false)
      .description("Print the Category's properties and values to the console when Flow "
                   "arrives");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Dictionary>("Value"_ustr)
      .description("Every property of the Category with its value, as a Dictionary");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeListGlobalProperties"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_global_properties_cc
