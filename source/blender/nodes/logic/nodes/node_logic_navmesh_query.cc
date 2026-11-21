/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Navmesh Query (2026-11-18): asks a navigation mesh without moving anything: the way between
 * two points, the navmesh point nearest a position, a random reachable point, whether a point
 * can be reached, whether a straight line is walkable, a good place to go (Find Spot,
 * 2026-11-20: hidden, in cover, seen, near, not taken). One query,
 * LN_QueryExpressionKind::NavmeshQuery, its outputs reading the result.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_navmesh_query_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: LN_NavmeshQueryMode. */
enum Mode {
  FindPath = 0,
  NearestPoint,
  RandomPoint,
  IsReachable,
  WalkableLine,
  AirNearestPoint,
  AirClearLine,
  AirFindPath,
  FindSpot,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Bool>("Found"_ustr)
      .description("Find Path: a way all the way to To. Nearest / Random Point: a point was "
                   "found. Is Reachable: To can be reached. Walkable Line: nothing stops a "
                   "straight walk from From to To. Air: a point, a clear line, a way all the way");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("Find Path: where the way ends (To, or the nearest it gets). Nearest / Random "
                   "Point: the point. Walkable Line: where a wall stops the walk (To when "
                   "nothing does). Air: the point, or where the way ends");
  b.add_output<decl::List>("Points"_ustr)
      .description("Find Path, Air: Find Path: the way's corners, From first");
  b.add_output<decl::Float>("Length"_ustr)
      .description("Find Path / Is Reachable: the way's length. Walkable Line: how far it walks. "
                   "Air: Find Path: the way's length");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("Walkable Line: the normal of the wall that stops it");
  b.add_output<decl::Float>("Score"_ustr)
      .description("Find Spot: the best place's score (higher is better: minus Walk Weight x its "
                   "walk, minus Look Weight x its distance from Look From)");
  b.add_default_layout();
  b.add_input<decl::Object>("Navmesh Object"_ustr)
      .description("A navigation mesh object (Build Navigation Mesh). Empty: the scene's own "
                   "navigation mesh (Scene > Navigation Mesh > Build at Game Start)");
  b.add_input<decl::Vector>("From"_ustr).description("Where the way or the line starts");
  b.add_input<decl::Vector>("To"_ustr).description("Where the way or the line should end");
  b.add_input<decl::Vector>("Position"_ustr)
      .description("Nearest Point: the position to snap onto the navigation mesh. Random Point: "
                   "the centre of the circle the point is taken in. Air: Nearest Point: the "
                   "position to find free air at");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(10.0f)
      .min(0.0f)
      .description("Random Point: how far from Position (0: anywhere reachable). A new point "
                   "every step. Air: Nearest Point: how far from Position to look. Find Spot: "
                   "how far round Position places are tried");
  b.add_input<decl::Vector>("Look From"_ustr)
      .description("Find Spot: the eye of the one places are judged against (an enemy's head): "
                   "Visibility, Cover and Look Weight use it. Unlinked and no Look Object: none");
  b.add_input<decl::Object>("Look Object"_ustr)
      .description("Find Spot: the one at Look From (its own body never blocks its sight). "
                   "Empty: none");
  b.add_input<decl::Int>("Samples"_ustr)
      .default_value(24)
      .min(1)
      .max(128)
      .description("Find Spot: how many places round Position are tried (each costs up to two "
                   "rays)");
  b.add_input<decl::Bool>("Cover"_ustr)
      .default_value(false)
      .description("Find Spot: only places with something solid toward Look From within Cover "
                   "Distance, at a crouching chest's height (0.45 of the asking NPC's height, "
                   "0.8 m for a 1.8 m NPC or an asker without a body: a wall of about 1.2 m or "
                   "more, a crate, a doorway's side)");
  b.add_input<decl::Float>("Cover Distance"_ustr)
      .default_value(1.5f)
      .min(0.1f)
      .description("Find Spot: how near the covering wall must be");
  b.add_input<decl::Float>("Walk Weight"_ustr)
      .default_value(1.0f)
      .description("Find Spot: how much a metre of walk from From lowers a place's score (a "
                   "negative weight: farther is better)");
  b.add_input<decl::Float>("Look Weight"_ustr)
      .default_value(0.0f)
      .description("Find Spot: how much a metre from Look From lowers a place's score (negative: "
                   "far from the look is better, positive: near)");
  b.add_input<decl::Bool>("Not Taken"_ustr)
      .default_value(true)
      .description("Find Spot: no other NPC standing there or going there (within Spacing)");
  b.add_input<decl::Float>("Spacing"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Find Spot: how far from other NPCs and their goals a place must be");
  b.add_input<decl::String>("Area"_ustr)
      .description("Find Spot: only places in this area (Physics > Navigation > Area of the "
                   "level objects). Empty: anywhere");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "query", UI_ITEM_NONE, "", ICON_NONE);
  const int mode = static_cast<const bNode *>(ptr->data)->custom1;
  if (mode >= AirNearestPoint && mode <= AirFindPath) {
    layout.prop(ptr, "use_water", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (mode == FindSpot) {
    layout.prop(ptr, "spot_visibility", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = FindPath;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int mode = node->custom1;
  const bool point_mode = mode == NearestPoint || mode == RandomPoint || mode == AirNearestPoint;
  auto input = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  auto output = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->output_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  const bool spot = mode == FindSpot;
  input("From"_ustr, !point_mode);
  input("To"_ustr, !point_mode && !spot);
  input("Position"_ustr, point_mode || spot);
  input("Navmesh Object"_ustr, mode < AirNearestPoint || spot);
  input("Radius"_ustr, mode == RandomPoint || mode == AirNearestPoint || spot);
  for (const UString identifier : {"Look From"_ustr,
                                   "Look Object"_ustr,
                                   "Samples"_ustr,
                                   "Cover"_ustr,
                                   "Cover Distance"_ustr,
                                   "Walk Weight"_ustr,
                                   "Look Weight"_ustr,
                                   "Not Taken"_ustr,
                                   "Spacing"_ustr,
                                   "Area"_ustr})
  {
    input(identifier, spot);
  }
  output("Point"_ustr, mode != IsReachable && mode != AirClearLine);
  output("Points"_ustr, mode == FindPath || mode == AirFindPath || spot);
  output("Length"_ustr,
         mode == FindPath || mode == IsReachable || mode == WalkableLine ||
             mode == AirFindPath || spot);
  output("Normal"_ustr, mode == WalkableLine);
  output("Score"_ustr, spot);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeNavmeshQuery"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_navmesh_query_cc
