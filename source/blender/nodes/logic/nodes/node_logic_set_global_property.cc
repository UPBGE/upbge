/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_global_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Category"_ustr)
      .description("The group of the global property, such as \"Options\"; with Persistent also "
                   "its file, //Globals/<Category>.logdb.txt next to the .blend. Pick one the "
                   "file's global property nodes use, or type a new one")
      .custom_draw(logic::global_category_field());
  b.add_input<decl::String>("Property"_ustr)
      .description("The global property's name in its Category: every tree, object and scene "
                   "reads it, also after Restart Game or Start Other Game, until the game is "
                   "quit. Pick one the file's global property nodes use, or type a new one")
      .custom_draw(logic::global_property_field("Category"));
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Any value, such as a number, text, vector or list");
  b.add_input<decl::Bool>("Persistent"_ustr)
      .default_value(false)
      .description("Also write the whole Category to its file at once, so that a later play "
                   "reads it back (settings, high scores). The file is written each time this "
                   "runs: not every step");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetGlobalProperty"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_global_property_cc
