/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_material.hh"
#include "BKE_mesh.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "GEO_mesh_primitive_cylinder_cone.hh"

#include "node_geometry_util.hh"

#include "RNA_enum_types.hh"

namespace blender::nodes::node_geo_mesh_primitive_cone_cc {

NODE_STORAGE_FUNCS(NodeGeometryMeshCone)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Vertices")
      .default_value(32)
      .min(3)
      .max(512)
      .description("Number of points on the circle at the top and bottom");
  b.add_input<decl::Int>("Side Segments")
      .default_value(1)
      .min(1)
      .max(512)
      .description("The number of edges running vertically along the side of the cone");
  auto &fill = b.add_input<decl::Int>("Fill Segments")
                   .default_value(1)
                   .min(1)
                   .max(512)
                   .description("Number of concentric rings used to fill the round face");
  b.add_input<decl::Float>("Radius Top")
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Radius of the top circle of the cone");
  b.add_input<decl::Float>("Radius Bottom")
      .default_value(1.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Radius of the bottom circle of the cone");
  b.add_input<decl::Float>("Depth")
      .default_value(2.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Height of the generated cone");
  b.add_output<decl::Geometry>("Mesh");
  b.add_output<decl::Bool>("Top").field_on_all().translation_context(BLT_I18NCONTEXT_ID_NODETREE);
  b.add_output<decl::Bool>("Bottom").field_on_all().translation_context(
      BLT_I18NCONTEXT_ID_NODETREE);
  b.add_output<decl::Bool>("Side").field_on_all();
  b.add_output<decl::Vector>("UV Map").field_on_all();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    const NodeGeometryMeshCone &storage = node_storage(*node);
    const GeometryNodeMeshCircleFillType fill_type = GeometryNodeMeshCircleFillType(
        storage.fill_type);
    fill.available(fill_type != GEO_NODE_MESH_CIRCLE_FILL_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryMeshCone *node_storage = MEM_callocN<NodeGeometryMeshCone>(__func__);

  node_storage->fill_type = GEO_NODE_MESH_CIRCLE_FILL_NGON;

  node->storage = node_storage;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "fill_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryMeshCone &storage = node_storage(params.node());
  const GeometryNodeMeshCircleFillType fill = (GeometryNodeMeshCircleFillType)storage.fill_type;

  const int circle_segments = params.extract_input<int>("Vertices");
  if (circle_segments < 3) {
    params.error_message_add(NodeWarningType::Info, TIP_("Vertices must be at least 3"));
    params.set_default_remaining_outputs();
    return;
  }

  const int side_segments = params.extract_input<int>("Side Segments");
  if (side_segments < 1) {
    params.error_message_add(NodeWarningType::Info, TIP_("Side Segments must be at least 1"));
    params.set_default_remaining_outputs();
    return;
  }

  const bool no_fill = fill == GEO_NODE_MESH_CIRCLE_FILL_NONE;
  const int fill_segments = no_fill ? 1 : params.extract_input<int>("Fill Segments");
  if (fill_segments < 1) {
    params.error_message_add(NodeWarningType::Info, TIP_("Fill Segments must be at least 1"));
    params.set_default_remaining_outputs();
    return;
  }

  const float radius_top = params.extract_input<float>("Radius Top");
  const float radius_bottom = params.extract_input<float>("Radius Bottom");
  const float depth = params.extract_input<float>("Depth");

  geometry::ConeAttributeOutputs attribute_outputs;
  attribute_outputs.top_id = params.get_output_anonymous_attribute_id_if_needed("Top");
  attribute_outputs.bottom_id = params.get_output_anonymous_attribute_id_if_needed("Bottom");
  attribute_outputs.side_id = params.get_output_anonymous_attribute_id_if_needed("Side");
  attribute_outputs.uv_map_id = params.get_output_anonymous_attribute_id_if_needed("UV Map");

  Mesh *mesh = geometry::create_cylinder_or_cone_mesh(radius_top,
                                                      radius_bottom,
                                                      depth,
                                                      circle_segments,
                                                      side_segments,
                                                      fill_segments,
                                                      geometry::ConeFillType(fill),
                                                      attribute_outputs);
  BKE_id_material_eval_ensure_default_slot(&mesh->id);

  /* Transform the mesh so that the base of the cone is at the origin. */
  bke::mesh_translate(*mesh, float3(0.0f, 0.0f, depth * 0.5f), false);

  params.set_output("Mesh", GeometrySet::from_mesh(mesh));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "fill_type",
                    "Fill Type",
                    "",
                    rna_enum_node_geometry_mesh_circle_fill_type_items,
                    NOD_storage_enum_accessors(fill_type),
                    GEO_NODE_MESH_CIRCLE_FILL_NGON,
                    nullptr,
                    true);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMeshCone", GEO_NODE_MESH_PRIMITIVE_CONE);
  ntype.ui_name = "Cone";
  ntype.ui_description = "Generate a cone mesh";
  ntype.enum_name_legacy = "MESH_PRIMITIVE_CONE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryMeshCone", node_free_standard_storage, node_copy_standard_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_primitive_cone_cc
