/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_instance_transform_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Instances")
      .only_instances()
      .description("Instances to transform individually");
  b.add_output<decl::Geometry>("Instances").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Matrix>("Transform")
      .field_on_all()
      .implicit_field(NODE_DEFAULT_INPUT_INSTANCE_TRANSFORM_FIELD)
      .structure_type(StructureType::Field);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Instances");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<float4x4> transform_field = params.extract_input<Field<float4x4>>("Transform");

  if (geometry_set.has_instances()) {
    InstancesComponent &instances = geometry_set.get_component_for_write<InstancesComponent>();
    bke::try_capture_field_on_geometry(
        instances, "instance_transform", AttrDomain::Instance, selection_field, transform_field);
  }

  params.set_output("Instances", std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetInstanceTransform", GEO_NODE_SET_INSTANCE_TRANSFORM);
  ntype.ui_name = "Set Instance Transform";
  ntype.ui_description = "Set the transformation matrix of every instance";
  ntype.enum_name_legacy = "SET_INSTANCE_TRANSFORM";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_type_size(ntype, 160, 100, 700);
  node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_instance_transform_cc
