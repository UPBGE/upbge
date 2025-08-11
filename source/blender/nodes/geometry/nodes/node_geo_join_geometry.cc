/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_join_geometries.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_join_geometry_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Geometry")
      .multi_input()
      .description("Geometries to merge together by concatenating their elements");
  b.add_output<decl::Geometry>("Geometry").propagate_all().align_with_previous();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Vector<SocketValueVariant> input_values = params.extract_input<Vector<SocketValueVariant>>(
      "Geometry");
  Vector<GeometrySet> geometry_sets;
  for (SocketValueVariant &value : input_values) {
    geometry_sets.append(value.extract<GeometrySet>());
  }

  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Geometry");

  for (GeometrySet &geometry : geometry_sets) {
    GeometryComponentEditData::remember_deformed_positions_if_necessary(geometry);
  }

  GeometrySet geometry_set_result = geometry::join_geometries(geometry_sets, attribute_filter);

  params.set_output("Geometry", std::move(geometry_set_result));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeJoinGeometry", GEO_NODE_JOIN_GEOMETRY);
  ntype.ui_name = "Join Geometry";
  ntype.ui_description = "Merge separately generated geometries into a single one";
  ntype.enum_name_legacy = "JOIN_GEOMETRY";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_join_geometry_cc
