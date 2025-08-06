/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_tool_set_face_set_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Mesh");
  b.add_output<decl::Geometry>("Mesh").align_with_previous().description(
      "Mesh to override the face set attribute on");
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Int>("Face Set").hide_value().field_on_all();
}

static bool is_constant_zero(const Field<int> &face_set)
{
  if (face_set.node().depends_on_input()) {
    return false;
  }
  return fn::evaluate_constant_field<int>(face_set) == 0;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (!check_tool_context_and_error(params)) {
    return;
  }
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const Field<int> face_set = params.extract_input<Field<int>>("Face Set");
  const bool is_zero = is_constant_zero(face_set);

  GeometrySet geometry = params.extract_input<GeometrySet>("Mesh");
  geometry::foreach_real_geometry(geometry, [&](GeometrySet &geometry) {
    if (Mesh *mesh = geometry.get_mesh_for_write()) {
      if (is_zero) {
        mesh->attributes_for_write().remove(".sculpt_face_set");
      }
      else {
        bke::try_capture_field_on_geometry(geometry.get_component_for_write<MeshComponent>(),
                                           ".sculpt_face_set",
                                           AttrDomain::Face,
                                           selection,
                                           face_set);
      }
    }
  });
  params.set_output("Mesh", std::move(geometry));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeToolSetFaceSet", GEO_NODE_TOOL_SET_FACE_SET);
  ntype.ui_name = "Set Face Set";
  ntype.ui_description = "Set sculpt face set values for faces";
  ntype.enum_name_legacy = "TOOL_SET_FACE_SET";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.gather_link_search_ops = search_link_ops_for_tool_node;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tool_set_face_set_cc
