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

namespace blender::nodes::node_logic_get_rigid_body_attribute_cc {
namespace decl = blender::nodes::logic::decl;

static void node_update(bNodeTree *ntree, bNode *node);

static bool uses_value_output(const int attribute_type)
{
  return ELEM(attribute_type, 0, 1, 2, 4, 5, 6, 7, 8);
}

static bool uses_damping_output(const int attribute_type)
{
  return attribute_type == 3;
}

static bool uses_enabled_output(const int attribute_type)
{
  return ELEM(attribute_type, 9, 12);
}

static bool uses_sleeping_output(const int attribute_type)
{
  return attribute_type == 10;
}

static bool uses_axis_locks_output(const int attribute_type)
{
  return attribute_type == 11;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description("Object to read, or the owner when empty");
  b.add_output<decl::Bool>("Valid"_ustr);
  b.add_output<decl::Float>("Value"_ustr);
  b.add_output<decl::Float>("Linear Damping"_ustr);
  b.add_output<decl::Float>("Angular Damping"_ustr);
  b.add_output<decl::Bool>("Enabled"_ustr);
  b.add_output<decl::Bool>("Allow Sleeping"_ustr);
  b.add_output<decl::Vector>("Lock Translation"_ustr);
  b.add_output<decl::Vector>("Lock Rotation"_ustr);
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
  bNodeSocket *value_socket = node->output_by_identifier("Value"_ustr);
  bNodeSocket *linear_damping_socket = node->output_by_identifier("Linear Damping"_ustr);
  bNodeSocket *angular_damping_socket = node->output_by_identifier("Angular Damping"_ustr);
  bNodeSocket *enabled_socket = node->output_by_identifier("Enabled"_ustr);
  bNodeSocket *allow_sleeping_socket = node->output_by_identifier("Allow Sleeping"_ustr);
  bNodeSocket *lock_translation_socket = node->output_by_identifier("Lock Translation"_ustr);
  bNodeSocket *lock_rotation_socket = node->output_by_identifier("Lock Rotation"_ustr);

  const int attribute_type = node->custom1;
  if (value_socket) {
    bke::node_set_socket_availability(*ntree, *value_socket, uses_value_output(attribute_type));
  }
  if (linear_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *linear_damping_socket, uses_damping_output(attribute_type));
  }
  if (angular_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *angular_damping_socket, uses_damping_output(attribute_type));
  }
  if (enabled_socket) {
    bke::node_set_socket_availability(*ntree, *enabled_socket, uses_enabled_output(attribute_type));
  }
  if (allow_sleeping_socket) {
    bke::node_set_socket_availability(
        *ntree, *allow_sleeping_socket, uses_sleeping_output(attribute_type));
  }
  if (lock_translation_socket) {
    bke::node_set_socket_availability(
        *ntree, *lock_translation_socket, uses_axis_locks_output(attribute_type));
  }
  if (lock_rotation_socket) {
    bke::node_set_socket_availability(
        *ntree, *lock_rotation_socket, uses_axis_locks_output(attribute_type));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetRigidBodyAttribute"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_rigid_body_attribute_cc
