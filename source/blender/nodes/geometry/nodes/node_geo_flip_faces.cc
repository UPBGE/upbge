/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_flip_faces_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to flip faces of");
  b.add_output<decl::Geometry>("Mesh").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (Mesh *mesh = geometry_set.get_mesh_for_write()) {
      const bke::MeshFieldContext field_context(*mesh, AttrDomain::Face);
      fn::FieldEvaluator evaluator(field_context, mesh->faces_num);
      evaluator.add(selection_field);
      evaluator.evaluate();
      const IndexMask selection = evaluator.get_evaluated_as_mask(0);
      if (selection.is_empty()) {
        return;
      }

      bke::mesh_flip_faces(*mesh, selection);
    }
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFlipFaces", GEO_NODE_FLIP_FACES);
  ntype.ui_name = "Flip Faces";
  ntype.ui_description =
      "Reverse the order of the vertices and edges of selected faces, flipping their normal "
      "direction";
  ntype.enum_name_legacy = "FLIP_FACES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_flip_faces_cc
