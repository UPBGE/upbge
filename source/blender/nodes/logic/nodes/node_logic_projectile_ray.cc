/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_projectile_ray_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1 (2026-11-11): every older node holds Scene. */
enum GravitySource { SceneGravity = 0, CustomGravity };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues when the path was cast (Flow linked: each time Flow runs)");
  b.add_output<decl::Condition>("Has Result"_ustr, "RESULT"_ustr)
      .description("True when the thrown path hits an object");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when an object without the Property stops the path (X-Ray off)");
  b.add_output<decl::Object>("Hit Object"_ustr, "PICKED_OBJECT"_ustr)
      .description("Object the path hits");
  b.add_output<decl::Object>("Hit Part"_ustr)
      .description("The part of a compound object that was hit (its child with Compound); the "
                   "Hit Object itself for any other object");
  b.add_output<decl::Vector>("Hit Point"_ustr, "POINT"_ustr)
      .description("Where the path touches the hit object; zero without a hit");
  b.add_output<decl::Vector>("Hit Normal"_ustr, "NORMAL"_ustr)
      .description("The hit surface's normal; zero without a hit");
  b.add_output<decl::String>("Hit Material"_ustr)
      .description("Name of the material on the hit face; empty without a hit or a material");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("Length of the path to the hit, or of the whole path checked without one");
  b.add_output<decl::Float>("Fraction"_ustr)
      .description("Distance divided by Max Distance");
  b.add_output<decl::Float>("Flight Time"_ustr)
      .description("Seconds the thrown object needs to reach the hit (or the end of the path "
                   "checked): time a grenade's fuse or an AI's lead with it");
  b.add_output<decl::Vector>("Impact Velocity"_ustr)
      .description("The thrown object's velocity when it gets there (world units per second): "
                   "its speed for damage, its direction for a bounce");
  b.add_output<decl::Int>("Face Index"_ustr).description("Hit mesh polygon index, or -1");
  b.add_output<decl::Condition>("Has UV"_ustr).description("True when UV is valid");
  b.add_output<decl::Vector>("UV"_ustr).description("Hit UV as X/Y, with Z set to 0");
  b.add_output<decl::List>("Parabola"_ustr, "PARABOLA"_ustr)
      .description("The points of the path, from Origin to the hit (or to where the check "
                   "ended): draw a trajectory preview with them");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr).description(
      "Optional trigger; unconnected, the throw is cast whenever an output is read");
  b.add_input<decl::Object>("Caster"_ustr).description(
      "The object the throw ignores, whose space Local uses; empty: the owner with Local, none "
      "without");
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("Another object the path passes through");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Objects the path passes through; invalid entries and duplicates are ignored");
  b.add_input<decl::Vector>("Origin"_ustr).description(
      "Where the throw starts: a world point, or (Local) a point in the Caster's space");
  b.add_input<decl::Vector>("Aim"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("World: the point the throw first heads for. Local: the throw's direction in "
                   "the Caster's space");
  b.add_input<decl::Bool>("Local"_ustr).description(
      "Origin is a point and Aim a direction in the Caster's space");
  b.add_input<decl::Float>("Power"_ustr)
      .default_value(10.0f)
      .min(0.0f)
      .description("Launch speed in world units (metres) per second, along Aim. Aim for Arc "
                   "finds the speed (or the angle) that lands on a target");
  b.add_input<decl::Float>("Max Distance"_ustr, "Distance"_ustr)
      .default_value(20.0f)
      .min(0.0f)
      .description("The longest path checked, measured along the curve; nothing beyond it is "
                   "hit");
  b.add_input<decl::Float>("Resolution"_ustr)
      .default_value(0.9f)
      .min(0.0f)
      .max(1.0f)
      .description("How finely the curve is followed: each segment covers 1 - Resolution seconds "
                   "of flight (0.9: steps of 0.1 s; 0.99: of 0.01 s, the finest). Higher gives a "
                   "smoother arc and more exact hits, and casts more segments");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("The thrown object's radius: a ball this size is swept along the path, so it "
                   "does not pass through gaps it would not fit. 0: a thin ray (cheaper)");
  b.add_input<decl::Float>("Max Time"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .subtype(PROP_TIME_ABSOLUTE)
      .description("The longest flight checked, in seconds (a grenade's fuse); 0: no limit "
                   "(Max Distance still applies)");
  b.add_input<decl::Vector>("Gravity"_ustr)
      .default_value(float3(0.0f, 0.0f, -9.81f))
      .description("The pull that bends the path, in world units per second squared");
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
  layout.prop(ptr, "gravity_source", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SceneGravity;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 != SceneGravity && node->custom1 != CustomGravity) {
    node->custom1 = SceneGravity;
  }
  if (bNodeSocket *socket = node->input_by_identifier("Gravity"_ustr)) {
    bke::node_set_socket_availability(*tree, *socket, node->custom1 == CustomGravity);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeProjectileRay"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_projectile_ray_cc
