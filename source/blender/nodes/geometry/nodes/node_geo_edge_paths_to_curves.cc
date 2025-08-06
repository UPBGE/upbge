/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_to_curve.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_edge_paths_to_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Edges to convert to curves");
  b.add_input<decl::Bool>("Start Vertices").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Int>("Next Vertex Index").default_value(-1).hide_value().field_on_all();
  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static Curves *edge_paths_to_curves_convert(const Mesh &mesh,
                                            const IndexMask &start_verts_mask,
                                            const Span<int> next_indices,
                                            const AttributeFilter &attribute_filter)
{
  Vector<int> vert_indices;
  Vector<int> curve_offsets;
  Array<bool> visited(mesh.verts_num, false);
  start_verts_mask.foreach_index([&](const int first_vert) {
    const int second_vert = next_indices[first_vert];
    if (first_vert == second_vert) {
      return;
    }
    if (second_vert < 0 || second_vert >= mesh.verts_num) {
      return;
    }

    curve_offsets.append(vert_indices.size());

    /* Iterate through path defined by #next_indices. */
    int current_vert = first_vert;
    while (!visited[current_vert]) {
      visited[current_vert] = true;
      vert_indices.append(current_vert);
      const int next_vert = next_indices[current_vert];
      if (next_vert < 0 || next_vert >= mesh.verts_num) {
        break;
      }
      current_vert = next_vert;
    }

    /* Reset visited status. */
    const int points_in_curve_num = vert_indices.size() - curve_offsets.last();
    for (const int vert_in_curve : vert_indices.as_span().take_back(points_in_curve_num)) {
      visited[vert_in_curve] = false;
    }
  });

  if (vert_indices.is_empty()) {
    return nullptr;
  }
  Curves *curves_id = bke::curves_new_nomain(geometry::create_curve_from_vert_indices(
      mesh.attributes(), vert_indices, curve_offsets, IndexRange(0), attribute_filter));
  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *mesh = geometry_set.get_mesh();
    if (mesh == nullptr) {
      geometry_set.keep_only({GeometryComponent::Type::Edit});
      return;
    }

    const bke::MeshFieldContext context{*mesh, AttrDomain::Point};
    fn::FieldEvaluator evaluator{context, mesh->verts_num};
    evaluator.add(params.get_input<Field<int>>("Next Vertex Index"));
    evaluator.add(params.get_input<Field<bool>>("Start Vertices"));
    evaluator.evaluate();
    const VArraySpan<int> next_vert = evaluator.get_evaluated<int>(0);
    IndexMask start_verts = evaluator.get_evaluated_as_mask(1);

    if (start_verts.is_empty()) {
      geometry_set.keep_only({GeometryComponent::Type::Edit});
      return;
    }

    geometry_set.replace_curves(edge_paths_to_curves_convert(
        *mesh, start_verts, next_vert, params.get_attribute_filter("Curves")));
    geometry_set.keep_only({GeometryComponent::Type::Curve, GeometryComponent::Type::Edit});
  });

  params.set_output("Curves", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeEdgePathsToCurves", GEO_NODE_EDGE_PATHS_TO_CURVES);
  ntype.ui_name = "Edge Paths to Curves";
  ntype.ui_description = "Output curves following paths across mesh edges";
  ntype.enum_name_legacy = "EDGE_PATHS_TO_CURVES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_edge_paths_to_curves_cc
