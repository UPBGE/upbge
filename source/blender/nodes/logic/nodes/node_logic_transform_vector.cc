/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Transform Vector (2026-11-16): custom1 Local to World 0 / World to Local 1, custom2 Point 0 /
 * Direction 1. Compiled from the object's world position, rotation and scale read as Get Object
 * Attribute reads them, and Vector::RotationFunction.
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_transform_vector_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object whose own space is meant; empty: this tree's object");
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Point: a place (moved, turned and scaled with the object); Direction: a "
                   "way to point (turned only)");
  b.add_output<decl::Vector>("Vector"_ustr).description("The point or direction in the other space");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "space", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "vector_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeTransformVector"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_transform_vector_cc
