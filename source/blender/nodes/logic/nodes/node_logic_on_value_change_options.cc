/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node On Value Changed (LogicNativeOnValueChange, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: ValueChanged, ValueChangedTo. They are not node types of
 * their own: logic_option_register keeps them for the merged node, which forwards its declaration,
 * buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_value_changed_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Value sampled once per fixed Logic + Physics step");
  b.add_output<decl::Execution>("If Changed"_ustr)
      .description("Emits for one fixed Logic + Physics step when Value differs from the previous sample");
  b.add_output<decl::Generic>("Old"_ustr)
      .description("Value from the previous sample");
  b.add_output<decl::Generic>("New"_ustr)
      .description("Current sampled value");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "ignore_initial_none", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeValueChanged"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_value_changed_cc

namespace blender::nodes::node_logic_value_changed_to_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Value sampled once per fixed Logic + Physics step");
  b.add_input<decl::Generic>("Target"_ustr)
      .description("Target value to detect; leave unlinked to detect a change to None");
  b.add_output<decl::Execution>("If Changed To"_ustr, "Result"_ustr)
      .description(
          "Emits for one fixed Logic + Physics step when Value reaches Target; numeric values also emit when "
          "they cross Target between samples");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeValueChangedTo"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_value_changed_to_cc
