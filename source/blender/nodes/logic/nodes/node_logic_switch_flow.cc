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

namespace blender::nodes::node_logic_switch_flow_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int min_case_count = 2;

static int case_count(const bNode &node)
{
  const int stored_count = node.custom2 > 0 ? node.custom2 : min_case_count;
  return std::clamp(stored_count, min_case_count, logic::max_dynamic_socket_count);
}

static void draw_case_count(ui::Layout &layout,
                            const bNode &node,
                            const char *operator_idname,
                            const char *label)
{
  ui::Layout &row = layout.row(true);
  row.label(label, ICON_NONE);

  ui::Layout &remove = row.row(true);
  remove.enabled_set(case_count(node) > min_case_count);
  PointerRNA op_ptr = remove.op(operator_idname, "", ICON_REMOVE);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", -1);

  ui::Layout &add = row.row(true);
  add.enabled_set(case_count(node) < logic::max_dynamic_socket_count);
  op_ptr = add.op(operator_idname, "", ICON_ADD);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_int_set(&op_ptr, "delta", 1);
}

static wmOperatorStatus change_case_count(bContext *C,
                                          wmOperator *op,
                                          const char *node_idname)
{
  PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(C, op, node_idname);
  if (node_ptr.data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bNode &node = *node_ptr.data_as<bNode>();
  const int delta = std::clamp(RNA_int_get(op->ptr, "delta"), -1, 1);
  const int new_count = std::clamp(
      case_count(node) + delta, min_case_count, logic::max_dynamic_socket_count);
  if (delta == 0 || new_count == case_count(node)) {
    return OPERATOR_CANCELLED;
  }

  node.custom2 = int16_t(new_count);
  socket_items::ops::update_after_node_change(C, node_ptr);
  return OPERATOR_FINISHED;
}

namespace int_switch {

static void int_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Incoming execution pulse");
  b.add_input<decl::Int>("Index"_ustr).default_value(0);
  const bNode *node = b.node_or_null();
  const int count = node != nullptr ? case_count(*node) : min_case_count;
  for (const int index : IndexRange(count)) {
    const UString output_name("Then " + std::to_string(index));
    b.add_output<decl::Execution>(output_name)
        .description("Runs when Index matches this case number");
  }
  b.add_output<decl::Execution>("Default"_ustr)
      .description("Runs when Index does not match any case number");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  draw_case_count(layout,
                  *ptr->data_as<bNode>(),
                  "node.logic_switch_flow_int_case_count",
                  "Cases");
}

static wmOperatorStatus node_case_count_exec(bContext *C, wmOperator *op)
{
  return change_case_count(C, op, "LogicNativeSwitchFlowInt");
}

static void NODE_OT_logic_switch_flow_int_case_count(wmOperatorType *ot)
{
  ot->name = "Change Integer Switch Flow Case Count";
  ot->idname = "NODE_OT_logic_switch_flow_int_case_count";
  ot->description = "Append or remove the final Integer Switch Flow case";
  ot->poll = socket_items::ops::editable_node_active_poll<void>;
  ot->exec = node_case_count_exec;
  ot->flag = OPTYPE_UNDO;
  socket_items::ops::add_node_identifier_property(ot);
  RNA_def_int(ot->srna, "delta", 1, -1, 1, "Delta", "Case count change", -1, 1);
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_logic_switch_flow_int_case_count);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom2 = min_case_count;
}

static void node_register()
{
  static bke::bNodeType int_ntype;

  logic_node_type_base(&int_ntype, "LogicNativeSwitchFlowInt"_ustr);
  int_ntype.nclass = NODE_CLASS_CONVERTER;
  int_ntype.declare = int_node_declare;
  int_ntype.draw_buttons = node_layout;
  int_ntype.initfunc = node_init;
  int_ntype.register_operators = node_operators;

  bke::node_register_type(int_ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace int_switch

namespace string_switch {

static void string_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Incoming execution pulse");
  b.add_input<decl::String>("Value"_ustr).default_value("");
  const bNode *node = b.node_or_null();
  const int count = node != nullptr ? case_count(*node) : min_case_count;
  for (const int index : IndexRange(count)) {
    const UString case_name("Case " + std::to_string(index));
    b.add_input<decl::String>(case_name)
        .default_value("Case " + std::to_string(index))
        .description("String value for this case; the first matching case wins");
    const UString output_name("Then " + std::to_string(index));
    b.add_output<decl::Execution>(output_name)
        .description("Runs when Value matches this case");
  }
  b.add_output<decl::Execution>("Default"_ustr)
      .description("Runs when Value matches no case");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  draw_case_count(layout,
                  *ptr->data_as<bNode>(),
                  "node.logic_switch_flow_string_case_count",
                  "Cases");
}

static wmOperatorStatus node_case_count_exec(bContext *C, wmOperator *op)
{
  return change_case_count(C, op, "LogicNativeSwitchFlowString");
}

static void NODE_OT_logic_switch_flow_string_case_count(wmOperatorType *ot)
{
  ot->name = "Change String Switch Flow Case Count";
  ot->idname = "NODE_OT_logic_switch_flow_string_case_count";
  ot->description = "Append or remove the final String Switch Flow case";
  ot->poll = socket_items::ops::editable_node_active_poll<void>;
  ot->exec = node_case_count_exec;
  ot->flag = OPTYPE_UNDO;
  socket_items::ops::add_node_identifier_property(ot);
  RNA_def_int(ot->srna, "delta", 1, -1, 1, "Delta", "Case count change", -1, 1);
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_logic_switch_flow_string_case_count);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom2 = min_case_count;
}

static void node_register()
{
  static bke::bNodeType string_ntype;

  logic_node_type_base(&string_ntype, "LogicNativeSwitchFlowString"_ustr);
  string_ntype.nclass = NODE_CLASS_CONVERTER;
  string_ntype.declare = string_node_declare;
  string_ntype.draw_buttons = node_layout;
  string_ntype.initfunc = node_init;
  string_ntype.register_operators = node_operators;

  bke::node_register_type(string_ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace string_switch

}  // namespace blender::nodes::node_logic_switch_flow_cc
