/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_collision_cc {
namespace decl = blender::nodes::logic::decl;

/* The Events dropdown (custom1, RNA `events`): LN_CollisionEvents. */
constexpr int16_t collision_events_any_object = 0;
constexpr int16_t collision_events_each_object = 1;

static void collision_event_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Property"_ustr).default_value("");
  b.add_input<decl::Material>("Material"_ustr);
  b.add_output<decl::Execution>("On Enter"_ustr)
      .description(
          "Each Object: runs once for every object that starts touching, in the step it "
          "arrives. Any Object: runs once when the first object arrives");
  b.add_output<decl::Execution>("On Stay"_ustr)
      .description(
          "Each Object: runs once a step for every object that keeps touching. Any Object: runs "
          "once a step while anything keeps touching");
  b.add_output<decl::Execution>("On Exit"_ustr)
      .description(
          "Each Object: runs once for every object that stops touching (also when it is "
          "removed: then with no Collided Object). Any Object: runs once when the last object "
          "leaves");
  b.add_output<decl::Condition>("Colliding"_ustr);
  b.add_output<decl::Object>("Collided Object"_ustr)
      .description(
          "Each Object, inside a run of On Enter, On Stay or On Exit: that run's object. "
          "Elsewhere: the first object touching");
  b.add_output<decl::List>("Collided Objects"_ustr);
  b.add_output<decl::Int>("Contact Count"_ustr);
  b.add_output<decl::Vector>("Point"_ustr);
  b.add_output<decl::List>("Points"_ustr);
  b.add_output<decl::Vector>("Normal"_ustr);
  b.add_output<decl::List>("Normals"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "events", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  /* Nodes made before the dropdown hold 0 (Any Object) and keep their behaviour. */
  node->custom1 = collision_events_each_object;
  static_assert(collision_events_any_object == 0);
}

static void node_register()
{
  static bke::bNodeType event_type;
  logic_node_type_base(&event_type, "LogicNativeOnCollision"_ustr);
  event_type.nclass = NODE_CLASS_INPUT;
  event_type.declare = collision_event_node_declare;
  event_type.draw_buttons = node_layout;
  event_type.initfunc = node_init;
  bke::node_register_type(event_type);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_collision_cc
