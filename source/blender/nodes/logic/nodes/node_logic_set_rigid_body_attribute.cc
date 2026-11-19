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
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

static constexpr int lock_translation_x = 1 << 0;
static constexpr int lock_translation_y = 1 << 1;
static constexpr int lock_translation_z = 1 << 2;
static constexpr int lock_rotation_x = 1 << 3;
static constexpr int lock_rotation_y = 1 << 4;
static constexpr int lock_rotation_z = 1 << 5;

static void node_update(bNodeTree *ntree, bNode *node);

static bool uses_value_input(const int attribute_type)
{
  return ELEM(attribute_type, 0, 1, 2, 4, 5, 8);
}

/** Min / Max Angular Velocity: an angle per second, shown in degrees as the Physics panel's. */
static bool uses_angular_velocity_input(const int attribute_type)
{
  return ELEM(attribute_type, 6, 7);
}

/* The Attribute (custom1, RNA `attribute_type`; the engine's LN_RigidBodyAttribute). Linear and
 * Angular Damping each set one damping (2026-11-15, the user's call); Damping (both at once) is no
 * choice any more: node_update makes a node saved with it Linear Damping. */
static constexpr int old_damping = 3;
static constexpr int linear_damping = 13;
static constexpr int angular_damping = 14;

static bool uses_linear_damping_input(const int attribute_type)
{
  return attribute_type == linear_damping;
}

static bool uses_angular_damping_input(const int attribute_type)
{
  return attribute_type == angular_damping;
}

/* Compound (2026-11-18): the Physics panel's checkbox while the game runs. */
static constexpr int compound = 15;

static bool uses_enabled_input(const int attribute_type)
{
  return ELEM(attribute_type, 9, 12, compound);
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

  /* The label in its own part (2026-11-15: "Translation" was cut to "Tran..." when it shared the
   * row equally with the three toggles). */
  ui::Layout &split = params.layout.split(0.42f, false);
  split.label(params.label, ICON_NONE);
  ui::Layout &row = split.row(true);
  row.prop(&params.node_ptr, property_x, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row.prop(&params.node_ptr, property_y, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
  row.prop(&params.node_ptr, property_z, ui::ITEM_R_TOGGLE, std::nullopt, ICON_NONE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<logic_decl::Object>("Object"_ustr)
      .description("Object to affect, or the owner when empty");
  b.add_input<logic_decl::Float>("Value"_ustr)
      .default_value(1.0f)
      .description("The new value, as the Physics panel's: Mass, Friction and Elasticity (below "
                   "0 taken as 0), Linear Velocity Min / Max in meters per second, Gravity "
                   "Factor");
  /* Blender's Float: the logic Float ignores the angle subtype. */
  b.add_input<decl::Float>("Angular Velocity"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .subtype(PROP_ANGLE)
      .description("Angle per second, as the Physics panel's Angular Velocity Min / Max. Max 0 "
                   "is Jolt's own cap, Min 0 no minimum");
  b.add_input<logic_decl::Float>("Linear Damping"_ustr)
      .default_value(0.05f)
      .description("As the Physics panel's Translation Damping, a rate per second: 0 never "
                   "slows, 0.05 (Jolt's default) loses about 5% of the speed a second, 1 about "
                   "63%, 5 about 99% (up to 100)");
  b.add_input<logic_decl::Float>("Angular Damping"_ustr)
      .default_value(0.05f)
      .description("As the Physics panel's Rotation Damping, a rate per second: 0 never slows, "
                   "0.05 (Jolt's default) loses about 5% of the spin a second, 1 about 63%, 5 "
                   "about 99% (up to 100)");
  b.add_input<logic_decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("On or off: Continuous Collision Detection, Allow Physics Rotation, or "
                   "Compound (a part below a compound joins (on) or leaves (off) its shape; any "
                   "other object holds the parts of the objects below it with Compound, or lets "
                   "them all go)");
  b.add_input<logic_decl::Bool>("Allow Sleeping"_ustr).default_value(true);
  b.add_input<logic_decl::Bool>("Wake"_ustr).default_value(false);
  /* Axis Locks (2026-11-15): each axis on its own. Lock says what happens to the chosen axes;
   * the axes not chosen keep their lock (the identifiers of the two rows are the old Lock
   * Translation / Lock Rotation). */
  b.add_input<logic_decl::Bool>("Lock"_ustr)
      .default_value(true)
      .description("On: lock the chosen axes. Off: free them. The axes not chosen keep their "
                   "lock (as the Physics panel or an earlier node set it)");
  b.add_input<logic_decl::Vector>("Translation"_ustr, "Lock Translation"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("The movement axes to lock or free (world X, Y, Z). Linked: a vector, each "
                   "axis not 0 is chosen")
      .custom_draw([](CustomSocketDrawParams &params) {
        draw_axis_lock_socket(params, "lock_translation_x", "lock_translation_y", "lock_translation_z");
      });
  b.add_input<logic_decl::Vector>("Rotation"_ustr, "Lock Rotation"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("The rotation axes to lock or free (around world X, Y, Z). Linked: a vector, "
                   "each axis not 0 is chosen")
      .custom_draw([](CustomSocketDrawParams &params) {
        draw_axis_lock_socket(params, "lock_rotation_x", "lock_rotation_y", "lock_rotation_z");
      });
  b.add_output<logic_decl::Execution>("Done"_ustr);
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
  /* A node saved with Damping (both): Linear Damping, its Linear Damping value and link kept
   * (this runs when the file is read; the dropdown no longer has Damping). */
  if (node->custom1 == old_damping) {
    node->custom1 = linear_damping;
  }
  sync_axis_lock_socket_defaults(*node);

  bNodeSocket *value_socket = node->input_by_identifier("Value"_ustr);
  bNodeSocket *angular_velocity_socket = node->input_by_identifier("Angular Velocity"_ustr);
  bNodeSocket *linear_damping_socket = node->input_by_identifier("Linear Damping"_ustr);
  bNodeSocket *angular_damping_socket = node->input_by_identifier("Angular Damping"_ustr);
  bNodeSocket *enabled_socket = node->input_by_identifier("Enabled"_ustr);
  bNodeSocket *allow_sleeping_socket = node->input_by_identifier("Allow Sleeping"_ustr);
  bNodeSocket *wake_socket = node->input_by_identifier("Wake"_ustr);
  bNodeSocket *lock_socket = node->input_by_identifier("Lock"_ustr);
  bNodeSocket *lock_translation_socket = node->input_by_identifier("Lock Translation"_ustr);
  bNodeSocket *lock_rotation_socket = node->input_by_identifier("Lock Rotation"_ustr);

  const int attribute_type = node->custom1;
  if (lock_socket) {
    bke::node_set_socket_availability(*ntree, *lock_socket, uses_axis_locks_input(attribute_type));
  }
  if (value_socket) {
    bke::node_set_socket_availability(*ntree, *value_socket, uses_value_input(attribute_type));
  }
  if (angular_velocity_socket) {
    bke::node_set_socket_availability(
        *ntree, *angular_velocity_socket, uses_angular_velocity_input(attribute_type));
  }
  if (linear_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *linear_damping_socket, uses_linear_damping_input(attribute_type));
  }
  if (angular_damping_socket) {
    bke::node_set_socket_availability(
        *ntree, *angular_damping_socket, uses_angular_damping_input(attribute_type));
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
  /* Wide (2026-11-15): its title, "Minimum Angular Velocity" and the axis rows fit. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_rigid_body_attribute_cc
