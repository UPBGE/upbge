/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_global_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Category"_ustr)
      .description("The group of the global property, such as \"Options\". Pick one the file's "
                   "global property nodes use, or type any name")
      .custom_draw(logic::global_category_field());
  b.add_input<decl::String>("Property"_ustr)
      .description("The global property's name in its Category. Pick one the file's global "
                   "property nodes use, or type any name (never red: another .blend of the "
                   "game, or an earlier play with Persistent, may have set it)")
      .custom_draw(logic::global_property_field("Category"));
  b.add_input<decl::Generic>("Default Value"_ustr)
      .description("Given while nothing has set the property in this play, and no earlier play "
                   "saved it with Persistent");
  b.add_output<decl::Generic>("Value"_ustr)
      .description("The value last set (any type), else Default Value");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeGetGlobalProperty"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_global_property_cc
