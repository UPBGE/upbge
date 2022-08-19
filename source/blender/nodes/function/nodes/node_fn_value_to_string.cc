/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"
#include <iomanip>

namespace blender::nodes::node_fn_value_to_string_cc {

static void fn_node_value_to_string_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Value"));
  b.add_input<decl::Int>(N_("Decimals")).min(0);
  b.add_output<decl::String>(N_("String"));
}

static void fn_node_value_to_string_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static fn::CustomMF_SI_SI_SO<float, int, std::string> to_str_fn{
      "Value To String", [](float a, int b) {
        std::stringstream stream;
        stream << std::fixed << std::setprecision(std::max(0, b)) << a;
        return stream.str();
      }};
  builder.set_matching_fn(&to_str_fn);
}

}  // namespace blender::nodes::node_fn_value_to_string_cc

void register_node_type_fn_value_to_string()
{
  namespace file_ns = blender::nodes::node_fn_value_to_string_cc;

  static bNodeType ntype;

  fn_node_type_base(&ntype, FN_NODE_VALUE_TO_STRING, "Value to String", NODE_CLASS_CONVERTER);
  ntype.declare = file_ns::fn_node_value_to_string_declare;
  ntype.build_multi_function = file_ns::fn_node_value_to_string_build_multi_function;
  nodeRegisterType(&ntype);
}
