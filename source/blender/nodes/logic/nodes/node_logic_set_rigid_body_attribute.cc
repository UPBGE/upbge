/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_rigid_body_attribute_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int lock_translation_x = 1 << 0;
static constexpr int lock_translation_y = 1 << 1;
static constexpr int lock_translation_z = 1 << 2;
static constexpr int lock_rotation_x = 1 << 3;
static constexpr int lock_rotation_y = 1 << 4;
static constexpr int lock_rotation_z = 1 << 5;

static void node_update(bNodeTree *ntree, bNode *node);

static bool uses_value_input(const int attribute_type)
{
  return ELEM(attribute_type, 0, 1, 2, 4, 5, 6, 7, 8);
}

static bool uses_damping_input(const int attribute_type)
{
  return attribute_type == 3;
}

static bool uses_enabled_input(const int attribute_type)
{
  return ELEM(attribute_type, 9, 12);
}

static bool uses_sleeping_input(const int attribute_type)
{
  return attribute_type == 10;
}

static bool uses_axis_locks_input(const int attribute_type)
{
  return attribute_type == 11;
}

static void sync_axis_lock_socket_defaults(bNode &node)
{
  bNodeSocket *translation_socket = node.input_by_identifier("Lock Translation"_ustr);
  if (translation_socket && translation_socket->default_value) {
    bNodeSocketValueVector *value = static_cast<bNodeSocketValueVector *>(
        translation_socket->default_value);
    value->value[0] = (node.custom2 & lock_translation_x) ? 1.0f : 0.0f;
    value->value[1] = (node.custom2 & lock_translation_y) ? 1.0f : 0.0f;
    value->value[2] = (node.custom2 & lock_translation_z) ? 1.0f : 0.0f;
  }

  bNodeSocket *rotation_socket = node.input_by_identifier("Lock Rotation"_ustr);
  if (rotation_socket && rotation_socket->default_value) {
    bNodeSocketValueVector *value = static_cast<bNodeSocketValueVector *>(
        rotation_socket->default_value);
    value->value[0] = (node.custom2 & lock_rotation_x) ? 1.0f : 0.0f;
    value->value[1] = (node.custom2 & lock_rotation_y) ? 1.0f : 0.0f;
    value->value[2] = (node.custom2 & lock_rotation_z) ? 1.0f : 0.0f;
  }
}

static void draw_axis_lock_socket(CustomSocketDrawParams &params,
                                  const char *property_x,
                                  const char *property_y,
                                  const char *property_z)
{
  if (params.socket.is_directly_linked()) {
    if (!params.label.is_empty()) {
      params.layout.label(params.label, ICON_NONE);
    }
    return;
  }

  ui::Layout &row = params.layout.row(true);
  row.label(params.label, ICON_NONE);
  row.prop(&params.node_ptr, property_x, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row.prop(&params.node_ptr, property_y, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row.prop(&params.node_ptr, property_z, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description("Object to affect, or the owner when empty");
  b.add_input<decl::Float>("Value"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Linear Damping"_ustr).default_value(0.04f);
  b.add_input<decl::Float>("Angular Damping"_ustr).default_value(0.1f);
  b.add_input<decl::Bool>("Enabled"_ustr).default_value(true);
  b.add_input<decl::Bool>("Allow Sleeping"_ustr).default_value(true);
  b.add_input<decl::Bool>("Wake"_ustr).default_value(false);
  b.add_input<decl::Vector>("Lock Translation"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .custom_draw([](CustomSocketDrawParams &params) {
        draw_axis_lock_socket(params, "lock_translation_x", "lock_translation_y", "lock_translation_z");
      });
  b.add_input<decl::Vector>("Lock Rotation"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .custom_draw([](CustomSocketDrawParams &params) {
        draw_axis_lock_socket(params, "lock_rotation_x", "lock_rotation_y", "lock_rotation_z");
      });
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
  sync_axis_lock_socket_defaults(*node);
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  sync_axis_lock_socket_defaults(*node);

  bNodeSocket *value_socket = node->input_by_identifier("Value"_ustr);
  bNodeSocket *linear_damping_socket = node->input_by_identifier("Linear Damping"_ustr);
  bNodeSocket *angular_damping_socket = node->input_by_identifier("Angular Damping"_ustr);
  bNodeSocket *enabled_socket = node->input_by_identifier("Enabled"_ustr);
  bNodeSocket *allow_sleeping_socket = node->input_by_identifier("Allow Sleeping"_ustr);
  bNodeSocket *wake_socket = node->input_by_identifier("Wake"_ustr);
  bNodeSocket *lock_translation_socket = node->input_by_identifier("Lock Translation"_ustr);
  bNodeSocket *lock_rotation_socket = node->input_by_identifier("Lock Rotation"_ustr);

  const int attribute_type = node->custom1;
  if (value_socket) {
    bke::node_set_socket_availability(*ntree, *value_socket, uses_value_input(attribute_type));
  }
  if (linear_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *linear_damping_socket, uses_damping_input(attribute_type));
  }
  if (angular_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *angular_damping_socket, uses_damping_input(attribute_type));
  }
  if (enabled_socket) {
    bke::node_set_socket_availability(*ntree, *enabled_socket, uses_enabled_input(attribute_type));
  }
  if (allow_sleeping_socket) {
    bke::node_set_socket_availability(
        *ntree, *allow_sleeping_socket, uses_sleeping_input(attribute_type));
  }
  if (wake_socket) {
    bke::node_set_socket_availability(*ntree, *wake_socket, uses_sleeping_input(attribute_type));
  }
  if (lock_translation_socket) {
    bke::node_set_socket_availability(
        *ntree, *lock_translation_socket, uses_axis_locks_input(attribute_type));
  }
  if (lock_rotation_socket) {
    bke::node_set_socket_availability(
        *ntree, *lock_rotation_socket, uses_axis_locks_input(attribute_type));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetRigidBodyAttribute"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_rigid_body_attribute_cc
