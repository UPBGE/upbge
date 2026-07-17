/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_object_attribute_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int set_object_attribute_use_x = 1 << 0;
static constexpr int set_object_attribute_use_y = 1 << 1;
static constexpr int set_object_attribute_use_z = 1 << 2;
static constexpr int set_object_attribute_mask_initialized = 1 << 3;

static void node_update(bNodeTree *ntree, bNode *node);

static void sync_xyz_socket_default(bNode &node)
{
  if ((node.custom2 & set_object_attribute_mask_initialized) == 0) {
    node.custom2 |= set_object_attribute_mask_initialized | set_object_attribute_use_x |
                    set_object_attribute_use_y | set_object_attribute_use_z;
  }

  bNodeSocket *socket = node.input_by_identifier("XYZ"_ustr);
  if (socket == nullptr || socket->default_value == nullptr) {
    return;
  }

  bNodeSocketValueVector *value = static_cast<bNodeSocketValueVector *>(socket->default_value);
  value->value[0] = (node.custom2 & set_object_attribute_use_x) ? 1.0f : 0.0f;
  value->value[1] = (node.custom2 & set_object_attribute_use_y) ? 1.0f : 0.0f;
  value->value[2] = (node.custom2 & set_object_attribute_use_z) ? 1.0f : 0.0f;
}

static bool uses_color_input(const int attribute_type)
{
  return attribute_type == 11;
}

static bool uses_bool_input(const int attribute_type)
{
  return attribute_type == 12;
}

static bool uses_transform_input(const int attribute_type)
{
  return ELEM(attribute_type, 4, 10);
}

static bool uses_xyz_input(const int attribute_type)
{
  return !uses_bool_input(attribute_type) && !uses_transform_input(attribute_type);
}

static bool uses_rotation_input(const int attribute_type)
{
  return attribute_type == 1 || attribute_type == 7;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Vector>("Components"_ustr, "XYZ"_ustr)
      .default_value(float3(1.0f, 1.0f, 1.0f))
      .description(
          "Component mask: enabled components are written, disabled components keep their current "
          "value")
      .custom_draw([](CustomSocketDrawParams &params) {
        if (params.socket.is_directly_linked()) {
          if (!params.label.is_empty()) {
            params.layout.label(params.label, ICON_NONE);
          }
          return;
        }
        ui::Layout &row = params.layout.row(true);
        row.label("Components:", ICON_NONE);
        row.prop(&params.node_ptr, "use_x", ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
        row.prop(&params.node_ptr, "use_y", ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
        row.prop(&params.node_ptr, "use_z", ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
      });
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::Vector>("Value"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Vector>("Position"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Rotation>("Rotation"_ustr)
      .description("Target rotation as Euler angles (radians)");
  b.add_input<decl::Vector>("Scale"_ustr).default_value({1.0f, 1.0f, 1.0f});
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Bool>("Visible"_ustr).default_value(true);
  b.add_input<decl::Bool>("Include Children"_ustr).default_value(false);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = set_object_attribute_mask_initialized | set_object_attribute_use_x |
                  set_object_attribute_use_y | set_object_attribute_use_z;
  sync_xyz_socket_default(*node);
  node_update(ntree, node);
}

static void migrate_value_links_to_rotation(bNodeTree *ntree, bNode *node)
{
  if (!uses_rotation_input(node->custom1)) {
    return;
  }

  bNodeSocket *value_socket = node->input_by_identifier("Value"_ustr);
  bNodeSocket *rotation_socket = node->input_by_identifier("Rotation"_ustr);
  if (value_socket == nullptr || rotation_socket == nullptr) {
    return;
  }

  for (bNodeLink *link : ntree->all_links()) {
    if (link->tonode != node || link->tosock != value_socket) {
      continue;
    }
    link->tosock = rotation_socket;
    rotation_socket->link = link;
    value_socket->link = nullptr;
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  sync_xyz_socket_default(*node);
  migrate_value_links_to_rotation(ntree, node);

  bNodeSocket *xyz_socket = node->input_by_identifier("XYZ"_ustr);
  bNodeSocket *value_socket = node->input_by_identifier("Value"_ustr);
  bNodeSocket *position_socket = node->input_by_identifier("Position"_ustr);
  bNodeSocket *rotation_socket = node->input_by_identifier("Rotation"_ustr);
  bNodeSocket *scale_socket = node->input_by_identifier("Scale"_ustr);
  bNodeSocket *color_socket = node->input_by_identifier("Color"_ustr);
  bNodeSocket *visible_socket = node->input_by_identifier("Visible"_ustr);
  bNodeSocket *include_children_socket = node->input_by_identifier("Include Children"_ustr);
  const bool use_color = uses_color_input(node->custom1);
  const bool use_bool = uses_bool_input(node->custom1);
  const bool use_transform = uses_transform_input(node->custom1);
  const bool use_rotation = uses_rotation_input(node->custom1) || use_transform;

  if (xyz_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *xyz_socket, uses_xyz_input(node->custom1));
  }
  if (value_socket != nullptr) {
    bke::node_set_socket_availability(
        *ntree, *value_socket, !use_color && !use_bool && !use_rotation && !use_transform);
  }
  if (position_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *position_socket, use_transform);
  }
  if (rotation_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *rotation_socket, use_rotation);
  }
  if (scale_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *scale_socket, use_transform);
  }
  if (color_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *color_socket, use_color);
  }
  if (visible_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *visible_socket, use_bool);
  }
  if (include_children_socket != nullptr) {
    bke::node_set_socket_availability(*ntree, *include_children_socket, use_bool);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(
      &ntype, "LogicNativeSetObjectAttribute"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_object_attribute_cc
