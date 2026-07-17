/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_character_info_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to read; defaults to the owner when unset");
  b.add_output<decl::Int>("Max Jumps"_ustr).description("Configured maximum jumps");
  b.add_output<decl::Int>("Current Jump Count"_ustr).description("Current consumed jumps");
  b.add_output<decl::Vector>("Gravity"_ustr).description("Character gravity vector");
  b.add_output<decl::Vector>("Walk Direction"_ustr).description("Character walk direction");
  b.add_output<decl::Bool>("On Ground"_ustr).description("Whether the character is grounded");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_local_space", UI_ITEM_NONE, "Local", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 1;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetCharacterInfo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_character_info_cc