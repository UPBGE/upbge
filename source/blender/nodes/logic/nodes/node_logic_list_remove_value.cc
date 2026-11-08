/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_list_remove_value_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description(logic::collection_store_object_description());
  b.add_input<decl::String>("Property"_ustr)
      .description(logic::collection_store_property_description("list"));
  b.add_input<decl::List>("List"_ustr);
  b.add_input<decl::Generic>("Value"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::List>("List"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeListRemoveValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = logic::collection_store_layout;
  ntype.initfunc = logic::collection_store_init;
  ntype.updatefunc = logic::collection_store_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_remove_value_cc
