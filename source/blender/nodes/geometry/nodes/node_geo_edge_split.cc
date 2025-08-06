/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_mesh_split_edges.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_edge_split_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh whose edges to split");
  b.add_output<decl::Geometry>("Mesh").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const Mesh *mesh = geometry_set.get_mesh()) {
      const bke::MeshFieldContext field_context{*mesh, AttrDomain::Edge};
      fn::FieldEvaluator selection_evaluator{field_context, mesh->edges_num};
      selection_evaluator.set_selection(selection_field);
      selection_evaluator.evaluate();
      const IndexMask mask = selection_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        return;
      }

      geometry::split_edges(
          *geometry_set.get_mesh_for_write(), mask, params.get_attribute_filter("Mesh"));
    }
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSplitEdges", GEO_NODE_SPLIT_EDGES);
  ntype.ui_name = "Split Edges";
  ntype.ui_description = "Duplicate mesh edges and break connections with the surrounding faces";
  ntype.enum_name_legacy = "SPLIT_EDGES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_edge_split_cc
