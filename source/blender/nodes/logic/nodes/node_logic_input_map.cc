/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_input_map_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: 0 Save, 1 Load, 2 Reset. */
static constexpr int16_t input_map_reset = 2;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Path"_ustr).default_value("//Data");
  b.add_input<decl::String>("File"_ustr).default_value("input_map");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Bool>("Success"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "input_map_operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  /* Reset needs no file. */
  for (const UString name : {"Path"_ustr, "File"_ustr}) {
    if (bNodeSocket *socket = node->input_by_identifier(name)) {
      bke::node_set_socket_availability(*ntree, *socket, node->custom1 != input_map_reset);
    }
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeInputMap"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_input_map_cc
