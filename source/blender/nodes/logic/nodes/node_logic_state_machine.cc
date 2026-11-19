/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * State Machine (2026-11-17): the state an object is in (a number from 0), kept per object. Go To
 * changes it (Entered pulses then), each Update pulse continues on the output of the state it is
 * in. custom2 the number of states (2 to 16). The compiler builds it from tree properties and
 * guarded routes (no runtime of its own), so wide runs take it as they take those.
 */

#include "node_logic_util.hh"

#include <algorithm>

#include "BLT_translation.hh"
#include <string>

#include "NOD_socket_items_ops.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

namespace blender::nodes::node_logic_state_machine_cc {
namespace logic_decl = blender::nodes::logic::decl;

static constexpr int min_state_count = 2;
static constexpr int max_state_count = 16;

static int state_count(const bNode &node)
{
  const int stored = node.custom2 > 0 ? node.custom2 : min_state_count;
  return std::clamp(stored, min_state_count, max_state_count);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Execution>("Update"_ustr)
      .is_default_link_socket()
      .description("Each pulse continues on the output of the state the object is in (link On "
                   "Logic + Physics Step: what each state does every step)");
  b.add_input<logic_decl::Execution>("Go To"_ustr)
      .description("Changes the state to State (nothing when it is already in it)");
  b.add_input<logic_decl::Int>("State"_ustr)
      .default_value(0)
      .min(0)
      .description("The state Go To changes to");
  b.add_input<logic_decl::Int>("Start State"_ustr)
      .default_value(0)
      .min(0)
      .description("The state the object starts in");
  const bNode *node = b.node_or_null();
  const int count = node != nullptr ? state_count(*node) : min_state_count;
  for (const int index : IndexRange(count)) {
    const UString name("State " + std::to_string(index));
    b.add_output<logic_decl::Execution>(name).description(
        "Update continues here while the object is in this state");
  }
  b.add_output<logic_decl::Execution>("Entered"_ustr)
      .description("Pulses after Go To changed the state (what to do once on entering: compare "
                   "Current State)");
  b.add_output<logic_decl::Int>("Current State"_ustr).description("The state it is in");
  b.add_output<logic_decl::Int>("Previous State"_ustr)
      .description("The state it was in before (the Start State until the first change)");
  b.add_output<logic_decl::Float>("Time in State"_ustr)
      .description("Seconds of game time since it entered the state (since the game started "
                   "for the Start State): the steps' time, slowed with the Timescale, stopped "
                   "while paused");
}

static void draw_state_count(ui::Layout &layout, const bNode &node)
{
  ui::Layout &row = layout.row(true);
  row.label(IFACE_("States"), ICON_NONE);
  ui::Layout &remove = row.row(true);
  remove.enabled_set(state_count(node) > min_state_count);
  PointerRNA op_ptr = remove.op("NODE_OT_logic_state_machine_state_count", "", ICON_REMOVE);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", -1);
  ui::Layout &add = row.row(true);
  add.enabled_set(state_count(node) < max_state_count);
  op_ptr = add.op("NODE_OT_logic_state_machine_state_count", "", ICON_ADD);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", 1);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  draw_state_count(layout, *ptr->data_as<bNode>());
}

static wmOperatorStatus state_count_exec(bContext *C, wmOperator *op)
{
  PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, "LogicNativeStateMachine");
  if (node_ptr.data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  bNode &node = *node_ptr.data_as<bNode>();
  const int delta = std::clamp(RNA_int_get(op->ptr, "delta"), -1, 1);
  const int new_count = std::clamp(state_count(node) + delta, min_state_count, max_state_count);
  if (delta == 0 || new_count == state_count(node)) {
    return OPERATOR_CANCELLED;
  }
  node.custom2 = int16_t(new_count);
  socket_items::ops::update_after_node_change(C, node_ptr);
  return OPERATOR_FINISHED;
}

static void NODE_OT_logic_state_machine_state_count(wmOperatorType *ot)
{
  ot->name = "Change State Machine State Count";
  ot->idname = "NODE_OT_logic_state_machine_state_count";
  ot->description = "Add or remove the last state of a State Machine";
  ot->poll = socket_items::ops::editable_node_active_poll<void>;
  ot->exec = state_count_exec;
  ot->flag = OPTYPE_UNDO;
  socket_items::ops::add_node_identifier_property(ot);
  RNA_def_int(ot->srna, "delta", 1, -1, 1, "Delta", "State count change", -1, 1);
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_logic_state_machine_state_count);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom2 = 3;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeStateMachine"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.register_operators = node_operators;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_state_machine_cc
