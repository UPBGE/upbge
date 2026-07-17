/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_collision_cc {
namespace decl = blender::nodes::logic::decl;

static void collision_query_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("");
  b.add_input<decl::Material>("Material"_ustr);
  b.add_output<decl::Condition>("Colliding"_ustr);
  b.add_output<decl::Object>("Collided Object"_ustr);
  b.add_output<decl::List>("Collided Objects"_ustr);
  b.add_output<decl::Int>("Contact Count"_ustr);
  b.add_output<decl::Vector>("Point"_ustr);
  b.add_output<decl::List>("Points"_ustr);
  b.add_output<decl::Vector>("Normal"_ustr);
  b.add_output<decl::List>("Normals"_ustr);
}

static void collision_event_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("");
  b.add_input<decl::Material>("Material"_ustr);
  b.add_output<decl::Execution>("On Enter"_ustr);
  b.add_output<decl::Execution>("On Stay"_ustr);
  b.add_output<decl::Execution>("On Exit"_ustr);
  b.add_output<decl::Condition>("Colliding"_ustr);
  b.add_output<decl::Object>("Collided Object"_ustr);
  b.add_output<decl::List>("Collided Objects"_ustr);
  b.add_output<decl::Int>("Contact Count"_ustr);
  b.add_output<decl::Vector>("Point"_ustr);
  b.add_output<decl::List>("Points"_ustr);
  b.add_output<decl::Vector>("Normal"_ustr);
  b.add_output<decl::List>("Normals"_ustr);
}

static void node_register()
{
  static bke::bNodeType query_type;
  logic_node_type_base(&query_type, "LogicNativeCollision"_ustr);
  query_type.nclass = NODE_CLASS_CONVERTER;
  query_type.declare = collision_query_node_declare;
  bke::node_register_type(query_type);

  static bke::bNodeType event_type;
  logic_node_type_base(&event_type, "LogicNativeOnCollision"_ustr);
  event_type.nclass = NODE_CLASS_INPUT;
  event_type.declare = collision_event_node_declare;
  bke::node_register_type(event_type);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_collision_cc
