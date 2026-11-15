/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Velocity at Point (2026-11-14): how fast a point on a physics body moves, its body's movement
 * plus its spin around its centre of mass (Jolt's GetPointVelocity), at a world position or at
 * the vertices of a vertex group of the object's mesh (a sword's tip, a hammer's head, a wheel's
 * rim). One query, LN_QueryExpressionKind::PointVelocity, its outputs reading the result.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_velocity_at_point_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: where the point comes from. custom2: which vertices of the group (Vertex Group). */
enum PointFrom { Position = 0, VertexGroup };
enum VertexPick { Center = 0, Fastest, EachVertex };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Vector>("Velocity"_ustr)
      .description("How fast and in which direction the point moves, in world space, meters per "
                   "second: the body's movement plus its spin around its centre of mass");
  b.add_output<decl::Float>("Speed"_ustr)
      .description("How fast the point moves, in meters per second");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("The world point measured: Position, the group's centre, or its fastest "
                   "vertex");
  b.add_output<decl::List>("Velocities"_ustr)
      .description("Each vertex's velocity, in the group's order");
  b.add_output<decl::List>("Points"_ustr)
      .description("Each vertex's world position, in the same order as Velocities");
  b.add_default_layout();
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object, or the owner when empty. Without a physics body of its own, the "
                   "body of its nearest parent that has one carries it (an Empty parented at a "
                   "sword's tip)");
  b.add_input<decl::Vector>("Position"_ustr)
      .description("The world point whose velocity is wanted (a hit point, an Empty's "
                   "position)");
  b.add_input<decl::String>("Vertex Group"_ustr)
      .description("A vertex group (or number attribute) of the object's mesh: its vertices as "
                   "they were when the game started, moving with the object (an armature or "
                   "shape keys are not followed)")
      .custom_draw(logic::vertex_group_field("Object"));
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "point_from", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "point_from") == VertexGroup) {
    layout.prop(ptr, "vertex_pick", UI_ITEM_NONE, "", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Position;
  node->custom2 = Center;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const bool group = node->custom1 == VertexGroup;
  const bool each = group && node->custom2 == EachVertex;
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
  input("Position"_ustr, !group);
  input("Vertex Group"_ustr, group);
  output("Velocity"_ustr, !each);
  output("Speed"_ustr, !each);
  output("Point"_ustr, !each);
  output("Velocities"_ustr, each);
  output("Points"_ustr, each);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeVelocityAtPoint"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_velocity_at_point_cc
