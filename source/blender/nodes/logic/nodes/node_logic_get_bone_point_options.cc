/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Get Bone Point (LogicNativeGetBonePoint, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: GetBoneHeadPoseWorld, GetBoneTailPoseWorld,
 * GetBoneCenterPoseWorld, GetBoneHeadWorld, GetBoneTailWorld, GetBoneCenterWorld, GetBoneLength.
 * They are not node types of their own: logic_option_register keeps them for the merged node,
 * which forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_bone_head_pose_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("Bone pose head vector in the selected space after pose evaluation");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "space", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(
      &ntype, "LogicNativeGetBoneHeadPoseWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_head_pose_world_cc

namespace blender::nodes::node_logic_get_bone_tail_pose_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("Bone pose tail vector in the selected space after pose evaluation");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "space", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(
      &ntype, "LogicNativeGetBoneTailPoseWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_tail_pose_world_cc

namespace blender::nodes::node_logic_get_bone_center_pose_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("Bone pose center vector in the selected space after pose evaluation");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "space", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(
      &ntype, "LogicNativeGetBoneCenterPoseWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_center_pose_world_cc

namespace blender::nodes::node_logic_get_bone_head_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("World position of the bone rest head (armature space, not posed)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetBoneHeadWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_head_world_cc

namespace blender::nodes::node_logic_get_bone_tail_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("World position of the bone rest tail (armature space, not posed)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetBoneTailWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_tail_world_cc

namespace blender::nodes::node_logic_get_bone_center_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("World position of the bone rest center (midpoint of head and tail)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetBoneCenterWorld"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_center_world_cc

namespace blender::nodes::node_logic_get_bone_length_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Float>("Length"_ustr)
      .description("Rest-pose length of the bone in armature space");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetBoneLength"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_length_cc
