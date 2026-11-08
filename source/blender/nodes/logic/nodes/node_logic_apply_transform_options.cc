/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Apply Movement (LogicNativeApplyTransform, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: ApplyMovement, Translate, ApplyRotation. They are not
 * node types of their own: logic_option_register keeps them for the merged node, which forwards
 * its declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_apply_movement_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::Vector>("Vector"_ustr).description("Movement delta");
  b.add_input<decl::Bool>("Local"_ustr).description("Apply the delta in local space");
  b.add_output<decl::Execution>("Done"_ustr);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeApplyMovement"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_movement_cc

namespace blender::nodes::node_logic_translate_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::Bool>("Local"_ustr).default_value(false);
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Movement direction and magnitude");
  b.add_input<decl::Float>("Speed"_ustr)
      .default_value(1.0f)
      .description("Distance multiplier per second");
  /* Shown as Done (2026-11-07): it continues at once, it does not wait for the move. The
   * identifier stays "When Done" (links and the compiler use it). */
  b.add_output<decl::Execution>("Done"_ustr, "When Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_option_type_base(&ntype, "LogicNativeTranslate"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_translate_cc

namespace blender::nodes::node_logic_apply_rotation_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::Rotation>("Rotation"_ustr).description("Euler rotation delta (radians)");
  b.add_input<decl::Bool>("Local"_ustr).description("Apply the delta in local space");
  b.add_output<decl::Execution>("Done"_ustr);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeApplyRotation"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_rotation_cc
