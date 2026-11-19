/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_play_sound_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Float>("Volume"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Pitch"_ustr).default_value(1.0f).min(0.0001f);
  b.add_input<decl::Bool>("Loop"_ustr).default_value(false);
  b.add_input<decl::Sound>("Sound"_ustr)
      .hide_value()
      .description("Linked: this sound instead of the one picked above (sounds in a list into "
                   "Get Random List Item: a random footstep each time)");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Bool>("Playing"_ustr)
      .description("The sound this node started last is playing (from the step after it "
                   "starts)");
  b.add_output<decl::Bool>("Finished"_ustr)
      .description("True for one step when the sound this node started last has ended (a "
                   "looping one: when stopped)");
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  template_id(&layout, C, ptr, "sound", nullptr, nullptr, nullptr);
  layout.prop(ptr, "sound_group", UI_ITEM_NONE, "Group", ICON_NONE);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativePlaySound"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_play_sound_cc
