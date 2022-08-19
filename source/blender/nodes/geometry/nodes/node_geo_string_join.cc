/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_string_join_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>(N_("Delimiter"));
  b.add_input<decl::String>(N_("Strings")).multi_input().hide_value();
  b.add_output<decl::String>(N_("String"));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Vector<std::string> strings = params.extract_multi_input<std::string>("Strings");
  const std::string delim = params.extract_input<std::string>("Delimiter");

  std::string output;
  for (const int i : strings.index_range()) {
    output += strings[i];
    if (i < (strings.size() - 1)) {
      output += delim;
    }
  }
  params.set_output("String", std::move(output));
}

}  // namespace blender::nodes::node_geo_string_join_cc

void register_node_type_geo_string_join()
{
  namespace file_ns = blender::nodes::node_geo_string_join_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_STRING_JOIN, "Join Strings", NODE_CLASS_CONVERTER);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
