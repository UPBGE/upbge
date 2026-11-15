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
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues when the ray was cast (Flow linked: each time Flow runs)");
  b.add_output<decl::Condition>("Has Result"_ustr).description("True when the ray hits an object");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when an object without the Property stops the ray (X-Ray off)");
  b.add_output<decl::Object>("Hit Object"_ustr, "Picked Object"_ustr)
      .description("Object hit by the ray");
  b.add_output<decl::Object>("Hit Part"_ustr)
      .description("The part of a compound object that was hit (its child with Compound, such "
                   "as a wheel or a turret); the Hit Object itself for any other object");
  b.add_output<decl::Vector>("Hit Point"_ustr, "Picked Point"_ustr)
      .description("World-space hit point; zero when Has Result is false");
  b.add_output<decl::Vector>("Hit Normal"_ustr, "Picked Normal"_ustr)
      .description("World-space hit normal; zero when Has Result is false");
  b.add_output<decl::String>("Hit Material"_ustr)
      .description("Name of the material on the hit face (boxes, spheres, capsules and convex "
                   "hulls: their first material); empty without a hit or a material");
  b.add_output<decl::Vector>("Ray Origin"_ustr)
      .description("Where the ray starts (on the camera's near side), hit or not: feed it into "
                   "Shape Cast, Ray to Plane or a Raycast of your own");
  b.add_output<decl::Vector>("Ray Direction"_ustr)
      .description("The ray's unit direction, hit or not");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("Distance to the hit or the blocker; Max Distance on a clear miss");
  b.add_output<decl::Float>("Fraction"_ustr)
      .description("Distance divided by Max Distance; 1 for a clear miss");
  b.add_output<decl::Int>("Face Index"_ustr).description("Hit mesh polygon index, or -1");
  b.add_output<decl::Condition>("Has UV"_ustr).description("True when UV is valid");
  b.add_output<decl::Vector>("UV"_ustr).description("Hit UV as X/Y, with Z set to 0");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional query trigger; Automatic tests the visible overlay before the main "
                   "world");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Camera used only when Interaction View is Custom Camera");
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("An object the ray passes through");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Objects the ray passes through; invalid entries and duplicates are ignored");
  b.add_input<decl::Float>("Max Distance"_ustr, "Distance"_ustr)
      .default_value(100.0f)
      .min(0.0f)
      .description("How far the ray reaches from the camera, in world units");
  b.add_input<decl::String>("Property"_ustr)
      .description(std::string("Only report hits on objects with this game property; empty "
                               "accepts any object. ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter");
  logic::add_ray_filters_panel(b);
  logic::add_visualize_panel(b);
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
