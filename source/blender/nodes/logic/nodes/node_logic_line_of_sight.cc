/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Line of Sight (2026-11-11): whether an eye sees a target, for guards, turrets and stealth.
 * The engine casts one ray (only when the target is within range and the view cone), passing the
 * eye's and the target's own bodies (query LN_QueryExpressionKind::LineOfSight).
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_line_of_sight_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

/** custom1 (Eye) and custom2 (Target): LN_DistanceEndpointMode. */
enum EndMode { Object = 0, Position };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<logic_decl::Execution>("Done"_ustr)
      .description("Continues when the check was made (Flow linked: each time Flow runs)");
  b.add_output<logic_decl::Condition>("Visible"_ustr)
      .description("True when the target is within Max Distance and the View Angle and nothing "
                   "blocks the line between the eye and the target");
  b.add_output<logic_decl::Object>("Blocker"_ustr)
      .description("What blocks the view (a wall, a crate); none when the target is visible or "
                   "out of range or angle");
  b.add_output<logic_decl::Float>("Distance"_ustr)
      .description("Distance from the eye to the target, whether it is seen or not");
  b.add_output<logic_decl::Float>("Angle"_ustr)
      .description("Degrees between the eye's Forward and the direction to the target (0: "
                   "straight ahead, 180: right behind), whether it is seen or not");
  b.add_default_layout();
  b.add_input<logic_decl::Execution>("Flow"_ustr)
      .description("Optional trigger; unconnected, the check is made whenever an output is read");
  b.add_input<logic_decl::Object>("Eye"_ustr)
      .description("The object that looks; empty: this tree's object. Tip: an empty parented at "
                   "a character's head");
  b.add_input<logic_decl::Vector>("Eye Offset"_ustr)
      .description("Where the eye is in the Eye object's own space (0, 0, 1.7: head height)");
  b.add_input<logic_decl::Vector>("Eye Position"_ustr)
      .description("The world position that looks");
  b.add_input<logic_decl::Vector>("Forward"_ustr)
      .default_value(float3(0.0f, 1.0f, 0.0f))
      .description("Where the eye faces: in the Eye object's own space (0, 1, 0: its Y axis; a "
                   "camera looks along 0, 0, -1), or in the world with Eye: Position. Zero: it "
                   "sees all around");
  b.add_input<logic_decl::Object>("Target"_ustr).description("The object looked at");
  b.add_input<logic_decl::Vector>("Target Offset"_ustr)
      .description("The point looked at in the Target's own space (0, 0, 1: its chest rather "
                   "than its feet)");
  b.add_input<logic_decl::Vector>("Target Position"_ustr)
      .description("The world position looked at");
  b.add_input<logic_decl::Float>("Max Distance"_ustr)
      .default_value(50.0f)
      .min(0.0f)
      .description("How far the eye sees; 0: no limit");
  b.add_input<decl::Float>("View Angle"_ustr)
      .default_value(float(M_PI_2))
      .min(0.0f)
      .max(float(2.0 * M_PI))
      .subtype(PROP_ANGLE)
      .description("The whole width of the view cone around Forward (90: 45 to each side); 360: "
                   "all around");
  b.add_input<logic_decl::List>("Ignore Objects"_ustr)
      .description("Objects that never block the view (glass, foliage, the guard's own weapon); "
                   "the eye and the target are passed anyway");
  b.add_input<logic_decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers that can block the view");
  logic::add_ray_filters_panel_without_xray(b);
  PanelDeclarationBuilder &visualize = b.add_panel("Visualize"_ustr).default_closed(true);
  visualize.add_input<logic_decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .panel_toggle()
      .description("Draw the line of sight and the view cone as debug lines while the game "
                   "runs, and in the 3D viewport before it (Logic Node Editor > Dashboard > Ray "
                   "Previews)");
  visualize.add_input<logic_decl::Float>("Duration"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .subtype(PROP_TIME_ABSOLUTE)
      .description("How long each check stays drawn, in seconds of game time; 0: only the step "
                   "(or frame) it was made in");
  visualize.add_input<logic_decl::Color>("Seen Color"_ustr)
      .default_value({0.1f, 1.0f, 0.2f, 1.0f})
      .description("Colour of the line when the target is visible");
  visualize.add_input<logic_decl::Color>("Hidden Color"_ustr)
      .default_value({1.0f, 0.05f, 0.02f, 1.0f})
      .description("Colour of the line when the target is blocked (faded: out of range or angle)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "eye_mode", UI_ITEM_NONE, IFACE_("Eye"), ICON_NONE);
  layout.prop(ptr, "target_mode", UI_ITEM_NONE, IFACE_("Target"), ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Object;
  node->custom2 = Object;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 != Object && node->custom1 != Position) {
    node->custom1 = Object;
  }
  if (node->custom2 != Object && node->custom2 != Position) {
    node->custom2 = Object;
  }
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Eye"_ustr, node->custom1 == Object);
  available("Eye Offset"_ustr, node->custom1 == Object);
  available("Eye Position"_ustr, node->custom1 == Position);
  available("Target"_ustr, node->custom2 == Object);
  available("Target Offset"_ustr, node->custom2 == Object);
  available("Target Position"_ustr, node->custom2 == Position);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeLineOfSight"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_line_of_sight_cc
