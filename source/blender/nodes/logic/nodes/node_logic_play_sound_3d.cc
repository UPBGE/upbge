/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_play_sound_3d_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Speaker"_ustr).description(
      "Object used as the 3D sound emitter; defaults to the owner when unset");
  b.add_input<decl::Float>("Volume"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Pitch"_ustr).default_value(1.0f).min(0.0001f);
  b.add_input<decl::Bool>("Loop"_ustr).default_value(false);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  template_id(&layout, C, ptr, "sound", nullptr, nullptr, nullptr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativePlaySound3D"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_play_sound_3d_cc
