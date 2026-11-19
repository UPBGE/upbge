/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Input Combo (2026-11-17): inputs pressed in order within a time (a fighting game's combo, a
 * cheat code), or held together (a chord: two shoulder buttons for a special move). Mode
 * (custom1): Sequence 0, Chord 1; custom2 bit 0: not Strict (RNA use_strict). Each Step is an input's Active (held) state: the node sees the
 * presses itself, so any key, button or Input Map action can be a step; steps left unlinked are
 * no part of it. Time on the step clock without timescale (LN_InputCombo).
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_input_combo_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Triggered"_ustr)
      .description("A pulse when the combo is done (Chord: once each time they come together)");
  b.add_output<decl::Int>("Progress"_ustr)
      .description("Sequence: how many steps were pressed in order so far. Chord: how many are "
                   "held");
  b.add_default_layout();
  b.add_input<decl::Float>("Time"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .description("Sequence: the most seconds from one step's press to the next. Chord: the "
                   "most seconds between the first press and the last");
  const char *step_description =
      "An input's Active (held) output: a key, a button, an Input Action. Left unlinked: no "
      "part of the combo";
  b.add_input<decl::Bool>("Step 1"_ustr).hide_value().description(step_description);
  b.add_input<decl::Bool>("Step 2"_ustr).hide_value().description(step_description);
  b.add_input<decl::Bool>("Step 3"_ustr).hide_value().description(step_description);
  b.add_input<decl::Bool>("Step 4"_ustr).hide_value().description(step_description);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "combo_mode", UI_ITEM_NONE, "", ICON_NONE);
  if (ptr->data_as<bNode>()->custom1 == 0) {
    layout.prop(ptr, "use_strict", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeInputCombo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_input_combo_cc
