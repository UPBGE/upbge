/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Navmesh Query (2026-11-18): asks a navigation mesh without moving anything: the way between
 * two points, the navmesh point nearest a position, a random reachable point, whether a point
 * can be reached, whether a straight line is walkable. One query,
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
enum Mode { FindPath = 0, NearestPoint, RandomPoint, IsReachable, WalkableLine };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Bool>("Found"_ustr)
      .description("Find Path: a way all the way to To. Nearest / Random Point: a point was "
                   "found. Is Reachable: To can be reached. Walkable Line: nothing stops a "
                   "straight walk from From to To");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("Find Path: where the way ends (To, or the nearest it gets). Nearest / Random "
                   "Point: the point. Walkable Line: where a wall stops the walk (To when "
                   "nothing does)");
  b.add_output<decl::List>("Points"_ustr)
      .description("Find Path: the way's corners, From first");
  b.add_output<decl::Float>("Length"_ustr)
      .description("Find Path / Is Reachable: the way's length. Walkable Line: how far it walks");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("Walkable Line: the normal of the wall that stops it");
  b.add_default_layout();
  b.add_input<decl::Object>("Navmesh Object"_ustr)
      .description("A navigation mesh object (Build Navigation Mesh). Empty: the scene's own "
                   "navigation mesh (Scene > Navigation Mesh > Build at Game Start)");
  b.add_input<decl::Vector>("From"_ustr).description("Where the way or the line starts");
  b.add_input<decl::Vector>("To"_ustr).description("Where the way or the line should end");
  b.add_input<decl::Vector>("Position"_ustr)
      .description("Nearest Point: the position to snap onto the navigation mesh. Random Point: "
                   "the centre of the circle the point is taken in");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(10.0f)
      .min(0.0f)
      .description("Random Point: how far from Position (0: anywhere reachable). A new point "
                   "every step");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "query", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = FindPath;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int mode = node->custom1;
  const bool point_mode = mode == NearestPoint || mode == RandomPoint;
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
  input("From"_ustr, !point_mode);
  input("To"_ustr, !point_mode);
  input("Position"_ustr, point_mode);
  input("Radius"_ustr, mode == RandomPoint);
  output("Point"_ustr, mode != IsReachable);
  output("Points"_ustr, mode == FindPath);
  output("Length"_ustr, mode == FindPath || mode == IsReachable || mode == WalkableLine);
  output("Normal"_ustr, mode == WalkableLine);
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
