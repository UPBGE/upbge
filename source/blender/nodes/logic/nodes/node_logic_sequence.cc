/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>
#include <string>

#include "NOD_socket_items_ops.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

namespace blender::nodes::node_logic_sequence_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int min_output_count = 2;

static int output_count(const bNode &node)
{
  const int stored_count = node.custom2 > 0 ? node.custom2 : min_output_count;
  return std::clamp(stored_count, min_output_count, logic::max_dynamic_socket_count);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Incoming execution pulse");
  const bNode *node = b.node_or_null();
  const int count = node != nullptr ? output_count(*node) : min_output_count;
  for (const int index : IndexRange(count)) {
    const UString name("Then " + std::to_string(index));
    auto &output = b.add_output<decl::Execution>(name);
    if (index == 0) {
      output.is_default_link_socket();
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode &node = *ptr->data_as<bNode>();
  ui::Layout &row = layout.row(true);
  row.label("Outputs", ICON_NONE);

  ui::Layout &remove = row.row(true);
  remove.enabled_set(output_count(node) > min_output_count);
  PointerRNA op_ptr = remove.op("node.logic_sequence_output_count", "", ICON_REMOVE);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", -1);

  ui::Layout &add = row.row(true);
  add.enabled_set(output_count(node) < logic::max_dynamic_socket_count);
  op_ptr = add.op("node.logic_sequence_output_count", "", ICON_ADD);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", 1);
}

static wmOperatorStatus node_output_count_exec(bContext *C, wmOperator *op)
{
  PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, "LogicNativeSequence");
  if (node_ptr.data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bNode &node = *node_ptr.data_as<bNode>();
  const int delta = std::clamp(RNA_int_get(op->ptr, "delta"), -1, 1);
  const int new_count = std::clamp(
      output_count(node) + delta, min_output_count, logic::max_dynamic_socket_count);
  if (delta == 0 || new_count == output_count(node)) {
    return OPERATOR_CANCELLED;
  }

  node.custom2 = int16_t(new_count);
  socket_items::ops::update_after_node_change(C, node_ptr);
  return OPERATOR_FINISHED;
}

static void NODE_OT_logic_sequence_output_count(wmOperatorType *ot)
{
  ot->name = "Change Sequence Output Count";
  ot->idname = "NODE_OT_logic_sequence_output_count";
  ot->description = "Append or remove the last ordered Sequence output";
  ot->poll = socket_items::ops::editable_node_active_poll<void>;
  ot->exec = node_output_count_exec;
  ot->flag = OPTYPE_UNDO;

  socket_items::ops::add_node_identifier_property(ot);
  RNA_def_int(ot->srna, "delta", 1, -1, 1, "Delta", "Output count change", -1, 1);
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_logic_sequence_output_count);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom2 = min_output_count;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSequence"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.register_operators = node_operators;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_sequence_cc
