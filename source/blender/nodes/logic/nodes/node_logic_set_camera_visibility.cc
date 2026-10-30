/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_camera_visibility_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object this camera draws or not. Empty: the tree's own object");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("The camera, such as a split screen player's, a minimap or a mirror camera "
                   "(render to texture), or the menu camera. Empty: the active camera");
  b.add_input<decl::Bool>("Visible"_ustr)
      .default_value(false)
      .description("Off: this camera does not draw the object (the other cameras still do), "
                   "and its player cannot select it if it is a UI Element. On: it draws it "
                   "again. Collision, rays and logic are not changed. Instances Blender makes "
                   "from the object (geometry nodes, particles) still show. Use Viewport "
                   "Render shows everything");
  b.add_input<decl::Bool>("Keep Shadow"_ustr)
      .default_value(false)
      .description("Hidden, but it still casts its shadow and shows in reflections in this "
                   "camera's view, such as a first person body");
  b.add_input<decl::Bool>("Include Children"_ustr)
      .default_value(false)
      .description("Also the object's children it has now");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetCameraVisibility"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_camera_visibility_cc
