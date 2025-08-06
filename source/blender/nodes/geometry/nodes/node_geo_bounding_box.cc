/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_primitive_cuboid.hh"
#include "GEO_transform.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_bounding_box_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .description(
          "Geometry to compute the bounding box of. Instances have to be realized before the full "
          "bounding box can be computed");
  b.add_input<decl::Bool>("Use Radius")
      .default_value(true)
      .description(
          "For curves, point clouds, and Grease Pencil, take the radius attribute into account "
          "when computing the bounds.");
  b.add_output<decl::Geometry>("Bounding Box")
      .propagate_all_instance_attributes()
      .description("A cube mesh enclosing the input geometry");
  b.add_output<decl::Vector>("Min");
  b.add_output<decl::Vector>("Max");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const bool use_radius = params.extract_input<bool>("Use Radius");

  /* Compute the min and max of all realized geometry for the two
   * vector outputs, which are only meant to consider real geometry. */
  const std::optional<Bounds<float3>> bounds = geometry_set.compute_boundbox_without_instances(
      use_radius);
  if (!bounds) {
    params.set_output("Min", float3(0));
    params.set_output("Max", float3(0));
  }
  else {
    params.set_output("Min", bounds->min);
    params.set_output("Max", bounds->max);
  }

  /* Generate the bounding box meshes inside each unique geometry set (including individually for
   * every instance). Because geometry components are reference counted anyway, we can just
   * repurpose the original geometry sets for the output. */
  if (params.output_is_required("Bounding Box")) {
    geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &sub_geometry) {
      std::optional<Bounds<float3>> sub_bounds;

      /* Reuse the min and max calculation if this is the main "real" geometry set. */
      if (&sub_geometry == &geometry_set) {
        sub_bounds = bounds;
      }
      else {
        sub_bounds = sub_geometry.compute_boundbox_without_instances(use_radius);
      }

      if (!sub_bounds) {
        sub_geometry.clear();
      }
      else {
        const float3 scale = sub_bounds->max - sub_bounds->min;
        const float3 center = sub_bounds->min + scale / 2.0f;
        Mesh *mesh = geometry::create_cuboid_mesh(scale, 2, 2, 2, "uv_map");
        geometry::transform_mesh(*mesh, center, math::Quaternion::identity(), float3(1));
        sub_geometry.replace_mesh(mesh);
        sub_geometry.keep_only({GeometryComponent::Type::Mesh, GeometryComponent::Type::Edit});
      }
    });

    params.set_output("Bounding Box", std::move(geometry_set));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeBoundBox", GEO_NODE_BOUNDING_BOX);
  ntype.ui_name = "Bounding Box";
  ntype.ui_description =
      "Calculate the limits of a geometry's positions and generate a box mesh with those "
      "dimensions";
  ntype.enum_name_legacy = "BOUNDING_BOX";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_bounding_box_cc
