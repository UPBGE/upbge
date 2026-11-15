/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Ray to Plane (2026-11-11): where a ray meets a flat plane, with no collider (the cursor on the
 * ground in strategy games and level editors). Math only: LN_AimMath.h.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

namespace blender::nodes::node_logic_ray_to_plane_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Ray Origin"_ustr)
      .description("Where the ray starts (Mouse Ray's or Camera Ray's Ray Origin)");
  b.add_input<decl::Vector>("Ray Direction"_ustr)
      .default_value(float3(0.0f, 0.0f, -1.0f))
      .description("Where the ray goes (Mouse Ray's or Camera Ray's Ray Direction); any length");
  b.add_input<decl::Vector>("Plane Point"_ustr)
      .description("Any point of the plane (0, 0, 0: the ground at height 0)");
  b.add_input<decl::Vector>("Plane Normal"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("The plane's facing (0, 0, 1: flat ground); any length");
  b.add_output<decl::Condition>("Has Result"_ustr)
      .description("True when the ray meets the plane ahead of its origin (false when it runs "
                   "parallel or points away)");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("Where the ray meets the plane; zero without a result");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("The plane's unit normal, turned toward the ray's origin; zero without a "
                   "result");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("How far along the ray the plane is, in world units; 0 without a result");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeRayToPlane"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_ray_to_plane_cc
