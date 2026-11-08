/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Control Sound (LogicNativeControlSound, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: PauseSound, ResumeSound, StopSound, StopAllSounds. They
 * are not node types of their own: logic_option_register keeps them for the merged node, which
 * forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_pause_sound_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  template_id(&layout, C, ptr, "sound", nullptr, nullptr, nullptr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativePauseSound"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_pause_sound_cc

namespace blender::nodes::node_logic_resume_sound_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  template_id(&layout, C, ptr, "sound", nullptr, nullptr, nullptr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeResumeSound"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_resume_sound_cc

namespace blender::nodes::node_logic_stop_sound_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  template_id(&layout, C, ptr, "sound", nullptr, nullptr, nullptr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeStopSound"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_stop_sound_cc

namespace blender::nodes::node_logic_stop_all_sounds_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeStopAllSounds"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_stop_all_sounds_cc
