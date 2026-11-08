/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Get Display Setting (LogicNativeGetDisplaySetting,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: GetFullscreen, GetResolution,
 * GetVSync. They are not node types of their own: logic_option_register keeps them for the merged
 * node, which forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_fullscreen_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>("Fullscreen"_ustr).description("Snapshot current fullscreen state");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetFullscreen"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_fullscreen_cc

namespace blender::nodes::node_logic_get_resolution_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Width"_ustr).description("Snapshot current window width in pixels");
  b.add_output<decl::Int>("Height"_ustr).description("Snapshot current window height in pixels");
  b.add_output<decl::Vector>("Resolution"_ustr).description(
      "Snapshot current window resolution as (width, height, 0)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetResolution"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_resolution_cc

namespace blender::nodes::node_logic_get_vsync_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Mode"_ustr).description(
      "The VSync mode: 0 On, 1 Off, 2 Adaptive (Set VSync's From Input takes the same numbers)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetVSync"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_vsync_cc
