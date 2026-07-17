/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_gamepad_stick_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Invert X"_ustr);
  b.add_input<decl::Bool>("Invert Y"_ustr).default_value(true);
  b.add_input<decl::Int>("Index"_ustr).default_value(0).min(0);
  b.add_input<decl::Float>("Sensitivity"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Threshold"_ustr).default_value(0.1f).min(0.0f).max(1.0f);
  b.add_output<decl::Float>("X"_ustr);
  b.add_output<decl::Float>("Y"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr, "VEC"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGamepadStick"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_gamepad_stick_cc
