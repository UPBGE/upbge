/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_pointcloud_types.h"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_point_radius_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Points")
      .supported_type(GeometryComponent::Type::PointCloud)
      .description("Points to set the radius of");
  b.add_output<decl::Geometry>("Points").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Float>("Radius")
      .default_value(0.05f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const Field<float> radius = params.extract_input<Field<float>>("Radius");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (PointCloud *pointcloud = geometry_set.get_pointcloud_for_write()) {
      bke::try_capture_field_on_geometry(pointcloud->attributes_for_write(),
                                         bke::PointCloudFieldContext(*pointcloud),
                                         "radius",
                                         bke::AttrDomain::Point,
                                         selection,
                                         radius);
    }
  });

  params.set_output("Points", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetPointRadius", GEO_NODE_SET_POINT_RADIUS);
  ntype.ui_name = "Set Point Radius";
  ntype.ui_description = "Set the display size of point cloud points";
  ntype.enum_name_legacy = "SET_POINT_RADIUS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_point_radius_cc
