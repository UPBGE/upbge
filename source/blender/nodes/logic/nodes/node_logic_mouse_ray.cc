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

namespace blender::nodes::node_logic_mouse_ray_cc {
namespace decl = blender::nodes::logic::decl;

enum class InteractionView : int16_t {
  Automatic = 0,
  Main,
  Overlay,
  CustomCamera,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional query trigger; Automatic tests the visible overlay before the main "
                   "world");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Camera used only when Interaction View is Custom Camera");
  b.add_input<decl::String>("Property"_ustr);
  b.add_input<decl::Bool>("X-Ray"_ustr).default_value(false);
  b.add_input<decl::Float>("Distance"_ustr).default_value(100.0f);
  b.add_input<decl::CollisionLayers>("Mask"_ustr).default_value(1023).min(0).max(1023);
  b.add_output<decl::Condition>("Has Result"_ustr);
  b.add_output<decl::Object>("Picked Object"_ustr);
  b.add_output<decl::Vector>("Picked Point"_ustr);
  b.add_output<decl::Vector>("Picked Normal"_ustr);
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
  logic_node_type_base(&ntype, "LogicNativeMouseRay"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_ray_cc
