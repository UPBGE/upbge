/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include "BKE_node_runtime.hh"

#include "node_function_util.hh"

#include "NOD_socket_search_link.hh"

namespace blender::nodes::node_fn_match_string_cc {

enum class MatchStringOperation : int8_t { StartsWith, EndsWith, Contains };

const EnumPropertyItem rna_enum_node_match_string_items[] = {
    {int(MatchStringOperation::StartsWith),
     "STARTS_WITH",
     0,
     "Starts With",
     "True when the first input starts with the second"},
    {int(MatchStringOperation::EndsWith),
     "ENDS_WITH",
     0,
     "Ends With",
     "True when the first input ends with the second"},
    {int(MatchStringOperation::Contains),
     "CONTAINS",
     0,
     "Contains",
     "True when the first input contains the second as a substring"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String").hide_label().is_default_link_socket();
  b.add_input<decl::Menu>("Operation").static_items(rna_enum_node_match_string_items);
  b.add_input<decl::String>("Key").hide_label().description(
      "The string to find in the input string");
  b.add_output<decl::Bool>("Result");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI3_SO<std::string, std::string, int, bool>(
      "Starts With", [](const std::string &a, const std::string &b, const int mode) {
        const StringRef strref_a(a);
        const StringRef strref_b(b);
        switch (MatchStringOperation(mode)) {
          case MatchStringOperation::StartsWith: {
            return strref_a.startswith(strref_b);
          }
          case MatchStringOperation::EndsWith: {
            return strref_a.endswith(strref_b);
          }
          case MatchStringOperation::Contains: {
            return strref_a.find(strref_b) != StringRef::not_found;
          }
        }
        return false;
      });
  builder.set_matching_fn(&fn);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(eNodeSocketDatatype(params.other_socket().type),
                                                   SOCK_STRING))
    {
      for (const EnumPropertyItem *item = rna_enum_node_match_string_items;
           item->identifier != nullptr;
           item++)
      {
        if (item->name != nullptr && item->identifier[0] != '\0') {
          MatchStringOperation operation = MatchStringOperation(item->value);
          params.add_item(IFACE_(item->name), [operation](LinkSearchOpParams &params) {
            bNode &node = params.add_node("FunctionNodeMatchString");
            params.update_and_connect_available_socket(node, "String");
            bke::node_find_socket(node, SOCK_IN, "Operation")
                ->default_value_typed<bNodeSocketValueMenu>()
                ->value = int(operation);
          });
        }
      }
    }
  }

  else {
    params.add_item(IFACE_("Result"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeMatchString");
      params.update_and_connect_available_socket(node, "Result");
    });
  }
}

static void node_label(const bNodeTree * /*tree*/,
                       const bNode *node,
                       char *label,
                       int label_maxncpy)
{
  const char *name;
  bool enum_label = RNA_enum_name(rna_enum_node_match_string_items, node->custom1, &name);
  if (!enum_label) {
    name = N_("Unknown");
  }
  BLI_strncpy_utf8(label, IFACE_(name), label_maxncpy);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeMatchString");
  ntype.ui_name = "Match String";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.build_multi_function = node_build_multi_function;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_match_string_cc
