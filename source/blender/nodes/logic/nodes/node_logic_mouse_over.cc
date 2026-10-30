/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_mouse_over_cc {
namespace decl = blender::nodes::logic::decl;

enum class InteractionView : int16_t {
  Automatic = 0,
  Main,
  Overlay,
  CustomCamera,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object to test against the topmost visible hit in the selected interaction "
                   "view");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Camera used only when Interaction View is Custom Camera");
  b.add_output<decl::Execution>("On Enter"_ustr)
      .description("Execution pulse on the Logic + Physics step when the cursor enters the object");
  b.add_output<decl::Execution>("On Over"_ustr)
      .description("Execution pulse every Logic + Physics step while the cursor is over the object");
  b.add_output<decl::Execution>("On Exit"_ustr)
      .description("Execution pulse on the Logic + Physics step when the cursor leaves the object");
  b.add_output<decl::Condition>("Entered"_ustr)
      .description("True when the cursor enters, evaluated in the phase that reads this output");
  b.add_output<decl::Condition>("Is Over"_ustr)
      .description("True while the cursor remains over, evaluated in the consuming phase");
  b.add_output<decl::Condition>("Exited"_ustr)
      .description("True when the cursor exits, evaluated in the phase that reads this output");
  b.add_output<decl::Object>("Object"_ustr)
      .description("Topmost runtime object hit while the target is under the cursor; null when "
                   "the target is not under the cursor");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("World-space hit point while over the object");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("World-space hit normal while over the object");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "interaction_view", UI_ITEM_NONE, "Interaction View", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(InteractionView::Automatic);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const InteractionView view = InteractionView(std::clamp<int>(
      node->custom1, int(InteractionView::Automatic), int(InteractionView::CustomCamera)));
  node->custom1 = int16_t(view);
  if (bNodeSocket *socket = node->input_by_identifier("Camera"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, view == InteractionView::CustomCamera);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeMouseOver"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_over_cc
