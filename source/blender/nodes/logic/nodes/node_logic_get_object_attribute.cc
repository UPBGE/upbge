/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_object_attribute_cc {
namespace decl = blender::nodes::logic::decl;

static void node_update(bNodeTree *ntree, bNode *node);

static bool uses_vector_output(const int attribute_type)
{
  return ELEM(attribute_type, 0, 1, 4, 5, 9, 10, 11, 12);
}

static bool uses_transform_output(const int attribute_type)
{
  return ELEM(attribute_type, 13, 14);
}

static bool uses_name_output(const int attribute_type)
{
  return attribute_type == 3;
}

static bool uses_visible_output(const int attribute_type)
{
  return attribute_type == 2;
}

static bool uses_orientation_output(const int attribute_type)
{
  return ELEM(attribute_type, 7, 8) || uses_transform_output(attribute_type);
}

static bool uses_color_output(const int attribute_type)
{
  return attribute_type == 6;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_output<decl::String>("Name"_ustr);
  b.add_output<decl::Vector>("Vector"_ustr);
  b.add_output<decl::Bool>("Visible"_ustr);
  b.add_output<decl::Vector>("Position"_ustr);
  b.add_output<decl::Rotation>("Rotation"_ustr, "Orientation"_ustr);
  b.add_output<decl::Vector>("Scale"_ustr);
  b.add_output<decl::Color>("Color"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *name_socket = node->output_by_identifier("Name"_ustr);
  bNodeSocket *vector_socket = node->output_by_identifier("Vector"_ustr);
  bNodeSocket *visible_socket = node->output_by_identifier("Visible"_ustr);
  bNodeSocket *position_socket = node->output_by_identifier("Position"_ustr);
  bNodeSocket *orientation_socket = node->output_by_identifier("Orientation"_ustr);
  bNodeSocket *scale_socket = node->output_by_identifier("Scale"_ustr);
  bNodeSocket *color_socket = node->output_by_identifier("Color"_ustr);

  if (name_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *name_socket, uses_name_output(node->custom1));
  }
  if (vector_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *vector_socket, uses_vector_output(node->custom1));
  }
  if (visible_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *visible_socket, uses_visible_output(node->custom1));
  }
  if (position_socket != nullptr) {
    bke::node_set_socket_availability(
        *ntree, *position_socket, uses_transform_output(node->custom1));
  }
  if (orientation_socket != nullptr) {
    bke::node_set_socket_availability(
        *ntree, *orientation_socket, uses_orientation_output(node->custom1));
  }
  if (scale_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *scale_socket, uses_transform_output(node->custom1));
  }
  if (color_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *color_socket, uses_color_output(node->custom1));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetObjectAttribute"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_object_attribute_cc
