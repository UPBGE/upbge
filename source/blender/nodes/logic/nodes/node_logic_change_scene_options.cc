/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Change Scene (LogicNativeChangeScene, NOD_logic_merged.hh), each
 * the node it was before 2026-11-08: SetScene, LoadScene. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_scene_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::String>("Scene"_ustr)
      .default_value("")
      .description("Blender scene that will replace this tree's owning runtime scene");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the deferred scene replacement is scheduled");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetScene"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_scene_cc

namespace blender::nodes::node_logic_load_scene_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::String>("Scene"_ustr)
      .default_value("")
      .description("Name of an inactive Blender scene to merge into this tree's runtime scene");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the scene merge command is issued");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeLoadScene"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_load_scene_cc
