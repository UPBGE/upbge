/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Camera Lens (LogicNativeSetCameraLens, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: SetCameraFov, SetCameraOrthoScale. They are not node
 * types of their own: logic_option_register keeps them for the merged node, which forwards its
 * declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_camera_fov_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Camera"_ustr).description("Perspective camera to update");
  b.add_input<decl::Float>("FOV"_ustr)
      .default_value(90.0f)
      .min(0.0f)
      .description("Field of view in degrees, as the camera's Field of View in Blender (along "
                   "the side its Sensor Fit names; 0.367 to 172.847)");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetCameraFov"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_camera_fov_cc

namespace blender::nodes::node_logic_set_camera_ortho_scale_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Camera"_ustr).description("Orthographic camera to update");
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Orthographic scale value");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetCameraOrthoScale"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_camera_ortho_scale_cc
