/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_geometry_to_instance_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .multi_input()
      .description("Each input geometry is turned into a separate instance");
  b.add_output<decl::Geometry>("Instances").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Vector<SocketValueVariant> input_values = params.extract_input<Vector<SocketValueVariant>>(
      "Geometry");
  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>();

  for (bke::SocketValueVariant &value : input_values) {
    bke::GeometrySet geometry = value.extract<bke::GeometrySet>();
    geometry.ensure_owns_direct_data();
    const int handle = instances->add_reference(std::move(geometry));
    instances->add_instance(handle, float4x4::identity());
  }

  GeometrySet new_geometry = GeometrySet::from_instances(instances.release());
  params.set_output("Instances", std::move(new_geometry));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGeometryToInstance", GEO_NODE_GEOMETRY_TO_INSTANCE);
  ntype.ui_name = "Geometry to Instance";
  ntype.ui_description =
      "Convert each input geometry into an instance, which can be much faster than the Join "
      "Geometry node when the inputs are large";
  ntype.enum_name_legacy = "GEOMETRY_TO_INSTANCE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  blender::bke::node_type_size(ntype, 160, 100, 300);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_geometry_to_instance_cc
