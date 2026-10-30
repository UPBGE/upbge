/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_volume_cc {
namespace decl = blender::nodes::logic::decl;

/* The sound_group enum values in rna_nodetree.cc (LN_SoundGroup). */
static constexpr int16_t GROUP_MASTER = 5;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Float>("Volume"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("The group's volume: 1 is full, 0 silent. It scales every sound of the group, "
                   "also those already playing; link a Slider's game property for a volume "
                   "setting");
  b.add_input<decl::Float>("Fade"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Seconds the change takes (real time, also while paused): 0 changes it at "
                   "once");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "sound_group", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = GROUP_MASTER;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetVolume"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_volume_cc
