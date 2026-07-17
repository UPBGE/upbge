/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

namespace blender::nodes::node_logic_follow_path_cc {
namespace decl = blender::nodes::logic::decl;

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Moving Object"_ustr);
  b.add_input<decl::Object>("Rotating Object"_ustr);
  b.add_input<decl::List>("Path Points"_ustr);
  b.add_input<decl::Bool>("Loop"_ustr).default_value(false);
  b.add_input<decl::Bool>("Continue"_ustr).default_value(false);
  b.add_input<decl::Object>("Optional Navmesh"_ustr);
  b.add_input<decl::Bool>("Move as Dynamic"_ustr).default_value(false);
  b.add_input<decl::Float>("Lin Speed"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Reach Threshold"_ustr).default_value(0.2f);
  b.add_input<decl::Bool>("Look At"_ustr).default_value(true);
  b.add_input<decl::Float>("Rot Speed"_ustr).default_value(1.0f);
  b.add_input<decl::Int>("Rot Axis"_ustr).default_value(2);
  b.add_input<decl::Int>("Front"_ustr).default_value(1);
  b.add_output<decl::Execution>("Done"_ustr);
}

static bool socket_bool_default(const bNodeSocket *socket)
{
  if (socket == nullptr || socket->type != SOCK_BOOLEAN || socket->default_value == nullptr) {
    return false;
  }
  return static_cast<const bNodeSocketValueBoolean *>(socket->default_value)->value;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const bool look_at = socket_bool_default(node->input_by_identifier("Look At"_ustr));
  for (const StringRef socket_name : {"Rot Speed", "Rot Axis", "Front"}) {
    if (bNodeSocket *socket = node->input_by_identifier(UString(socket_name))) {
      bke::node_set_socket_availability(*ntree, *socket, look_at);
    }
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeFollowPath"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_follow_path_cc
