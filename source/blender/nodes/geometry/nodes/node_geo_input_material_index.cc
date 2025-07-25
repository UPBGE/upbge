/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_material_index_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Material Index").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> material_index_field = AttributeFieldInput::from<int>("material_index");
  params.set_output("Material Index", std::move(material_index_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputMaterialIndex", GEO_NODE_INPUT_MATERIAL_INDEX);
  ntype.ui_name = "Material Index";
  ntype.ui_description =
      "Retrieve the index of the material used for each element in the geometry's list of "
      "materials";
  ntype.enum_name_legacy = "INPUT_MATERIAL_INDEX";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_material_index_cc
