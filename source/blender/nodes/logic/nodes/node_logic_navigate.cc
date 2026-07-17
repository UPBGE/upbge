/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_navigate_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Moving Object"_ustr);
  b.add_input<decl::Object>("Rotating Object"_ustr);
  b.add_input<decl::Object>("Navmesh Object"_ustr);
  b.add_input<decl::Vector>("Destination"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Bool>("Move as Dynamic"_ustr).default_value(false);
  b.add_input<decl::Float>("Lin Speed"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Reach Threshold"_ustr).default_value(1.0f);
  b.add_input<decl::Bool>("Look At"_ustr).default_value(true);
  b.add_input<decl::Int>("Rot Axis"_ustr).default_value(2);
  b.add_input<decl::Int>("Front"_ustr).default_value(1);
  b.add_input<decl::Float>("Rot Speed"_ustr).default_value(1.0f);
  b.add_input<decl::Bool>("Visualize"_ustr).default_value(false);
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Execution>("When Reached"_ustr);
  b.add_output<decl::List>("Next Point"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeNavigate"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_navigate_cc
