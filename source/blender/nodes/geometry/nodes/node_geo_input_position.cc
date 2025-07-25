/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_position_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Position").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> position_field{AttributeFieldInput::from<float3>("position")};
  params.set_output("Position", std::move(position_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputPosition", GEO_NODE_INPUT_POSITION);
  ntype.ui_name = "Position";
  ntype.ui_description = "Retrieve a vector indicating the location of each element";
  ntype.enum_name_legacy = "POSITION";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_position_cc
