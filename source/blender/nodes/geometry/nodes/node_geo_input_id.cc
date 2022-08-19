/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_id_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>(N_("ID")).field_source().description(
      N_("The values from the \"id\" attribute on points, or the index if that attribute does not "
         "exist"));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> position_field{std::make_shared<bke::IDAttributeFieldInput>()};
  params.set_output("ID", std::move(position_field));
}

}  // namespace blender::nodes::node_geo_input_id_cc

void register_node_type_geo_input_id()
{
  namespace file_ns = blender::nodes::node_geo_input_id_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_INPUT_ID, "ID", NODE_CLASS_INPUT);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
