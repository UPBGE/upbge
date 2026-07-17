/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_modify_property_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int modify_property_clamp = 1 << 0;

static void node_update(bNodeTree *ntree, bNode *node)
{
  const bool clamp = (node->custom2 & modify_property_clamp) != 0;
  if (bNodeSocket *socket = node->input_by_identifier("Min"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, clamp);
  }
  if (bNodeSocket *socket = node->input_by_identifier("Max"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, clamp);
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::String>("Property"_ustr).default_value("property");
  b.add_input<decl::Float>("Value"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("Min"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("Max"_ustr).default_value(1.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "clamp", UI_ITEM_NONE, "Clamp", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = NODE_MATH_ADD;
  node->custom2 = 0;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeModifyProperty"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_modify_property_cc
