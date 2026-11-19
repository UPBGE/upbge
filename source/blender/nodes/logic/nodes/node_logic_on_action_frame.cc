/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * On Action Frame (2026-11-16): pulses in the Logic + Physics step an action on the layer reaches
 * or passes Frame (Bool::ActionFrameReached, LN_ActionFrameCrossed in LN_ActionFrames.h).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_on_action_frame_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description(
      "The object playing the action; empty: the tree's own object");
  b.add_input<decl::Int>("Layer"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::Float>("Frame"_ustr)
      .default_value(10.0f)
      .description("The frame to react to: it pulses once each time the action passes it, "
                   "forward or back, also after a loop (a footstep, a sword's hit)");
  b.add_input<decl::String>("Action"_ustr)
      .description("Only this action (by name); empty: whichever action plays on the layer");
  b.add_output<decl::Execution>("On Frame"_ustr)
      .description("Pulses in the Logic + Physics step the frame is reached or passed");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOnActionFrame"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_action_frame_cc
