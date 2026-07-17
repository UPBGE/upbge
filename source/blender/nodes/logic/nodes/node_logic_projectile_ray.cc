/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_projectile_ray_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Caster"_ustr);
  b.add_input<decl::Vector>("Origin"_ustr);
  b.add_input<decl::Vector>("Aim"_ustr).default_value(float3(0.0f, 0.0f, 1.0f));
  b.add_input<decl::Bool>("Local"_ustr);
  b.add_input<decl::Float>("Power"_ustr).default_value(10.0f).min(0.0f);
  b.add_input<decl::Float>("Distance"_ustr).default_value(20.0f).min(0.0f);
  b.add_input<decl::Float>("Resolution"_ustr).default_value(0.9f).min(0.0f).max(1.0f);
  b.add_input<decl::String>("Property"_ustr);
  b.add_input<decl::Bool>("X-Ray"_ustr);
  b.add_input<decl::CollisionLayers>("Mask"_ustr).default_value(1023).min(0).max(1023);
  b.add_input<decl::Bool>("Visualize"_ustr);
  b.add_output<decl::Condition>("Has Result"_ustr, "RESULT"_ustr);
  b.add_output<decl::Object>("Picked Object"_ustr, "PICKED_OBJECT"_ustr);
  b.add_output<decl::Vector>("Picked Point"_ustr, "POINT"_ustr);
  b.add_output<decl::Vector>("Picked Normal"_ustr, "NORMAL"_ustr);
  b.add_output<decl::List>("Parabola"_ustr, "PARABOLA"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeProjectileRay"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_projectile_ray_cc
