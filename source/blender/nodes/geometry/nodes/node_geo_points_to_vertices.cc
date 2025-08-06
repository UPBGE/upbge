/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "DNA_pointcloud_types.h"

#include "BKE_customdata.hh"
#include "BKE_mesh.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_points_to_vertices_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Points")
      .supported_type(GeometryComponent::Type::PointCloud)
      .description("Points that are converted to vertices in a mesh");
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_output<decl::Geometry>("Mesh").propagate_all();
}

/* One improvement would be to move the attribute arrays directly to the mesh when possible. */
static void geometry_set_points_to_vertices(GeometrySet &geometry_set,
                                            Field<bool> &selection_field,
                                            const AttributeFilter &attribute_filter)
{
  const PointCloud *points = geometry_set.get_pointcloud();
  if (points == nullptr) {
    geometry_set.keep_only({GeometryComponent::Type::Edit});
    return;
  }
  if (points->totpoint == 0) {
    geometry_set.keep_only({GeometryComponent::Type::Edit});
    return;
  }

  const bke::PointCloudFieldContext field_context{*points};
  fn::FieldEvaluator selection_evaluator{field_context, points->totpoint};
  selection_evaluator.add(selection_field);
  selection_evaluator.evaluate();
  const IndexMask selection = selection_evaluator.get_evaluated_as_mask(0);

  Map<StringRef, AttributeDomainAndType> attributes;
  geometry_set.gather_attributes_for_propagation({GeometryComponent::Type::PointCloud},
                                                 GeometryComponent::Type::Mesh,
                                                 false,
                                                 attribute_filter,
                                                 attributes);

  Mesh *mesh;
  if (selection.size() == points->totpoint) {
    /* Create a mesh without positions so the attribute can be shared. */
    mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
    CustomData_free_layer_named(&mesh->vert_data, "position");
    mesh->verts_num = selection.size();
  }
  else {
    mesh = BKE_mesh_new_nomain(selection.size(), 0, 0, 0);
  }

  const AttributeAccessor src_attributes = points->attributes();
  MutableAttributeAccessor dst_attributes = mesh->attributes_for_write();

  for (MapItem<StringRef, AttributeDomainAndType> entry : attributes.items()) {
    const StringRef id = entry.key;
    const bke::AttrType data_type = entry.value.data_type;
    const GAttributeReader src = src_attributes.lookup(id);
    if (selection.size() == points->totpoint && src.sharing_info && src.varray.is_span()) {
      const bke::AttributeInitShared init(src.varray.get_internal_span().data(),
                                          *src.sharing_info);
      dst_attributes.add(id, AttrDomain::Point, data_type, init);
    }
    else {
      GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
          id, AttrDomain::Point, data_type);
      array_utils::gather(src.varray, selection, dst.span);
      dst.finish();
    }
  }

  mesh->tag_loose_edges_none();
  mesh->tag_overlapping_none();

  geometry_set.replace_mesh(mesh);
  geometry_set.keep_only({GeometryComponent::Type::Mesh, GeometryComponent::Type::Edit});
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    geometry_set_points_to_vertices(
        geometry_set, selection_field, params.get_attribute_filter("Mesh"));
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodePointsToVertices", GEO_NODE_POINTS_TO_VERTICES);
  ntype.ui_name = "Points to Vertices";
  ntype.ui_description = "Generate a mesh vertex for each point cloud point";
  ntype.enum_name_legacy = "POINTS_TO_VERTICES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_points_to_vertices_cc
