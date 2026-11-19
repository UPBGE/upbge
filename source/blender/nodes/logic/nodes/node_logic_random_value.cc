/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Random Value (2026-11-07): one random node for every type, as Geometry Nodes' Random Value.
 * custom1 the type (0 Float, 1 Integer, 2 Vector, 3 Boolean; the shapes, 2026-11-16: 4 Direction,
 * 5 In Sphere, 6 In Circle, 7 In Cone into the Vector output), custom2 the variation (0 Per Object,
 * 1 Same for All). Each type has its own sockets; the Float ones keep the identifiers the node had
 * before (Min, Max, Value), so files made with it load unchanged. The numbers: LN_Random.h.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_random_value_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

enum class RandomType : int16_t {
  Float = 0,
  Integer = 1,
  Vector = 2,
  Boolean = 3,
  /* The shapes (2026-11-16, LN_RandomShape + 4): vectors into the Vector output. */
  Direction = 4,
  InSphere = 5,
  InCircle = 6,
  InCone = 7,
};

static bool gives_vector(const RandomType type)
{
  return type == RandomType::Vector || int(type) >= int(RandomType::Direction);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<logic_decl::Float>("Value"_ustr);
  b.add_output<logic_decl::Int>("Value"_ustr, "Value_Int"_ustr);
  b.add_output<logic_decl::Vector>("Value"_ustr, "Value_Vector"_ustr);
  b.add_output<logic_decl::Bool>("Value"_ustr, "Value_Bool"_ustr);
  b.add_input<logic_decl::Float>("Min"_ustr).default_value(0.0f);
  b.add_input<logic_decl::Float>("Max"_ustr).default_value(1.0f);
  b.add_input<logic_decl::Int>("Min"_ustr, "Min_Int"_ustr).default_value(0);
  b.add_input<logic_decl::Int>("Max"_ustr, "Max_Int"_ustr)
      .default_value(100)
      .description("Both ends can come out");
  b.add_input<logic_decl::Vector>("Min"_ustr, "Min_Vector"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<logic_decl::Vector>("Max"_ustr, "Max_Vector"_ustr)
      .default_value({1.0f, 1.0f, 1.0f})
      .description("Each axis between its Min and Max: (-1, -1, -1) to (1, 1, 1) is any "
                   "direction; an axis whose ends are equal stays that value");
  b.add_input<logic_decl::Float>("Probability"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .description("How often it is true: 0.1 is one time in ten");
  b.add_input<logic_decl::Float>("Radius"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("The size of the ball (In Sphere) or the disc on the ground (In Circle) "
                   "around (0, 0, 0): add a position for points around it");
  b.add_input<logic_decl::Vector>("Axis"_ustr)
      .default_value({0.0f, 0.0f, 1.0f})
      .description("The direction the cone points along (an aim, a muzzle's forward)");
  b.add_input<decl::Float>("Angle"_ustr)
      .default_value(float(M_PI) / 6.0f)
      .min(0.0f)
      .max(float(M_PI))
      .subtype(PROP_ANGLE)
      .description("How far from Axis a direction may lean (a spread); 180: any direction");
  b.add_input<logic_decl::Int>("Seed"_ustr)
      .default_value(0)
      .description("Another number gives other values. Link a Loop's Index for a new value in "
                   "every iteration");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "variation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const RandomType type = RandomType(node->custom1);
  const auto show = [&](bNodeSocket *socket, const bool available) {
    if (socket != nullptr) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  show(node->output_by_identifier("Value"_ustr), type == RandomType::Float);
  show(node->output_by_identifier("Value_Int"_ustr), type == RandomType::Integer);
  show(node->output_by_identifier("Value_Vector"_ustr), gives_vector(type));
  show(node->output_by_identifier("Value_Bool"_ustr), type == RandomType::Boolean);
  show(node->input_by_identifier("Min"_ustr), type == RandomType::Float);
  show(node->input_by_identifier("Max"_ustr), type == RandomType::Float);
  show(node->input_by_identifier("Min_Int"_ustr), type == RandomType::Integer);
  show(node->input_by_identifier("Max_Int"_ustr), type == RandomType::Integer);
  show(node->input_by_identifier("Min_Vector"_ustr), type == RandomType::Vector);
  show(node->input_by_identifier("Max_Vector"_ustr), type == RandomType::Vector);
  show(node->input_by_identifier("Probability"_ustr), type == RandomType::Boolean);
  show(node->input_by_identifier("Radius"_ustr),
       type == RandomType::InSphere || type == RandomType::InCircle);
  show(node->input_by_identifier("Axis"_ustr), type == RandomType::InCone);
  show(node->input_by_identifier("Angle"_ustr), type == RandomType::InCone);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = int16_t(RandomType::Float);
  node->custom2 = 0; /* Per Object */
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRandomValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_random_value_cc
