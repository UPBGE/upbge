/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_string_join_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::String>("Delimiter");
  b.add_input<decl::String>("Strings").multi_input().hide_value();
  b.add_output<decl::String>("String").align_with_previous();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  auto strings = params.extract_input<GeoNodesMultiInput<std::string>>("Strings");
  const std::string delim = params.extract_input<std::string>("Delimiter");

  std::string output;
  for (const int i : strings.values.index_range()) {
    output += strings.values[i];
    if (i < (strings.values.size() - 1)) {
      output += delim;
    }
  }
  params.set_output("String", std::move(output));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeStringJoin", GEO_NODE_STRING_JOIN);
  ntype.ui_name = "Join Strings";
  ntype.ui_description = "Combine any number of input strings";
  ntype.enum_name_legacy = "STRING_JOIN";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_string_join_cc
