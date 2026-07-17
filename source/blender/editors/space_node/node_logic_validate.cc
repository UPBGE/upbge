/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <climits>
#include <memory>
#include <mutex>
#include <string>

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_vector_set.hh"

#include "DNA_ID.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "ED_screen.hh"

#include "LN_CompileReport.h"
#include "LN_Program.h"
#include "LN_TreeCompiler.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

#include "node_intern.hh"

namespace blender::ed::space_node {

static bNodeTree *logic_tree_from_context(const bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return nullptr;
  }
  if (snode->edittree != nullptr && snode->edittree->type == NTREE_LOGIC) {
    return snode->edittree;
  }
  if (snode->selected_node_group != nullptr && snode->selected_node_group->type == NTREE_LOGIC) {
    return snode->selected_node_group;
  }
  if (snode->nodetree != nullptr && snode->nodetree->type == NTREE_LOGIC) {
    return snode->nodetree;
  }
  return nullptr;
}

static bNodeTree *find_logic_tree_by_name(Main &bmain, const char *tree_name)
{
  if (tree_name == nullptr || tree_name[0] == '\0') {
    return nullptr;
  }
  for (bNodeTree &ntree : bmain.nodetrees) {
    if (ntree.type == NTREE_LOGIC && STREQ(ntree.id.name + 2, tree_name)) {
      return &ntree;
    }
  }
  return nullptr;
}

static bNode *find_node_by_identifier(bNodeTree &ntree, const int32_t node_identifier)
{
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (node->identifier == node_identifier) {
      return node;
    }
  }
  return nullptr;
}

static bNodeSocket *find_input_socket(bNode &node, const std::string &socket_name)
{
  if (socket_name.empty()) {
    return nullptr;
  }
  for (bNodeSocket *socket = static_cast<bNodeSocket *>(node.inputs.first); socket != nullptr;
       socket = socket->next)
  {
    if (socket_name == socket->identifier || socket_name == socket->name) {
      return socket;
    }
  }
  return nullptr;
}

static std::string compile_severity_prefix(const LN_CompileSeverity severity)
{
  switch (severity) {
    case LN_CompileSeverity::Error:
      return "Error";
    case LN_CompileSeverity::Warning:
      return "Warning";
    case LN_CompileSeverity::Info:
      return "Info";
  }
  return "Info";
}

static std::string compile_issue_message(const LN_CompileIssue &issue)
{
  return compile_severity_prefix(issue.severity) + ": " + issue.message;
}

static void add_node_validation_message(bNodeTree &ntree,
                                        const LN_SourceRef &source_ref,
                                        const std::string &message)
{
  bNode *node = find_node_by_identifier(ntree, source_ref.source_node_identifier);
  if (node == nullptr) {
    return;
  }

  std::lock_guard lock(ntree.runtime->shader_node_errors_mutex);
  ntree.runtime->shader_node_errors.lookup_or_add_default(node->identifier).add(message);
}

static void add_link_validation_message(bNodeTree &ntree,
                                        const LN_SourceRef &source_ref,
                                        const std::string &message)
{
  bNode *node = find_node_by_identifier(ntree, source_ref.source_node_identifier);
  if (node == nullptr) {
    return;
  }
  bNodeSocket *socket = find_input_socket(*node, source_ref.socket_name);
  if (socket == nullptr) {
    return;
  }

  ntree.ensure_topology_cache();
  for (bNodeLink *link : ntree.all_links()) {
    if (link->tonode == node && link->tosock == socket) {
      ntree.runtime->link_errors.add(bke::NodeLinkKey(*link), bke::NodeLinkError{message});
    }
  }
}

static void clear_logic_compile_messages(bNodeTree &ntree)
{
  {
    std::lock_guard lock(ntree.runtime->shader_node_errors_mutex);
    ntree.runtime->shader_node_errors.clear();
  }
  ntree.runtime->link_errors.clear();
}

static void apply_compile_report_to_tree(bNodeTree &ntree, const LN_Program &program)
{
  clear_logic_compile_messages(ntree);

  const std::vector<LN_SourceRef> &source_refs = program.GetSourceRefs();
  for (const LN_CompileIssue &issue : program.GetCompileReport().GetIssues()) {
    if (issue.source_ref_index >= source_refs.size()) {
      continue;
    }
    const LN_SourceRef &source_ref = source_refs[issue.source_ref_index];
    const std::string message = compile_issue_message(issue);
    add_node_validation_message(ntree, source_ref, message);
    add_link_validation_message(ntree, source_ref, message);
  }
}

static void report_compile_summary(ReportList *reports,
                                   const bNodeTree &ntree,
                                   const LN_Program &program)
{
  const LN_CompileReport &report = program.GetCompileReport();
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    const eReportType report_type = issue.severity == LN_CompileSeverity::Error ? RPT_ERROR :
                                    issue.severity == LN_CompileSeverity::Warning ? RPT_WARNING :
                                                                                     RPT_INFO;
    BKE_reportf(reports, report_type, "%s", issue.message.c_str());
  }

  if (!report.GetDisabledReason().empty()) {
    BKE_reportf(reports, RPT_WARNING, "%s", report.GetDisabledReason().c_str());
  }

  BKE_reportf(reports,
              RPT_INFO,
              "Logic tree '%s': %s",
              ntree.id.name + 2,
              program.DescribeSchedulerSummary().c_str());
}

static bNodeTree *resolve_logic_tree_for_operator(bContext *C, wmOperator *op)
{
  char tree_name[MAX_ID_NAME - 2] = "";
  RNA_string_get(op->ptr, "tree_name", tree_name);
  if (tree_name[0] != '\0') {
    Main *bmain = CTX_data_main(C);
    return bmain ? find_logic_tree_by_name(*bmain, tree_name) : nullptr;
  }
  return logic_tree_from_context(C);
}

static wmOperatorStatus validate_logic_tree_exec(bContext *C, wmOperator *op)
{
  bNodeTree *ntree = resolve_logic_tree_for_operator(C, op);
  if (ntree == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No LogicNodeTree to validate");
    return OPERATOR_CANCELLED;
  }

  LN_TreeCompiler compiler;
  std::shared_ptr<LN_Program> program = compiler.Compile(*ntree);
  if (program == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Logic tree validation did not produce a program");
    return OPERATOR_CANCELLED;
  }

  apply_compile_report_to_tree(*ntree, *program);
  report_compile_summary(op->reports, *ntree, *program);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, ntree);

  return program->GetCompileReport().HasErrors() ? OPERATOR_CANCELLED : OPERATOR_FINISHED;
}

struct LogicKeyboardKeyCaptureData {
  std::string tree_name;
  int32_t node_identifier = 0;
  std::string socket_identifier;
  std::string old_value;
};

static bNodeSocket *resolve_keyboard_key_socket(bContext *C,
                                                wmOperator *op,
                                                bNodeTree **r_ntree)
{
  if (r_ntree != nullptr) {
    *r_ntree = nullptr;
  }

  bNodeTree *ntree = resolve_logic_tree_for_operator(C, op);
  if (ntree == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No LogicNodeTree for keyboard key selection");
    return nullptr;
  }

  const int32_t node_identifier = RNA_int_get(op->ptr, "node_identifier");
  bNode *node = find_node_by_identifier(*ntree, node_identifier);
  if (node == nullptr || !STREQ(node->idname, "LogicNativeKeyboardKey")) {
    BKE_report(op->reports, RPT_ERROR, "Keyboard Key node not found");
    return nullptr;
  }

  char socket_identifier[64] = "";
  RNA_string_get(op->ptr, "socket_identifier", socket_identifier);
  bNodeSocket *socket = find_input_socket(*node, socket_identifier);
  if (socket == nullptr || socket->type != SOCK_STRING || socket->default_value == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Keyboard Key input socket not found");
    return nullptr;
  }

  if (r_ntree != nullptr) {
    *r_ntree = ntree;
  }
  return socket;
}

static void tag_keyboard_key_socket_changed(bContext *C, bNodeTree &ntree, bNodeSocket &socket)
{
  BKE_ntree_update_tag_socket_property(&ntree, &socket);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, &ntree.id);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, &ntree.id);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

static void set_keyboard_key_socket_value(bContext *C,
                                          bNodeTree &ntree,
                                          bNodeSocket &socket,
                                          const std::string &value)
{
  auto *socket_value = static_cast<bNodeSocketValueString *>(socket.default_value);
  STRNCPY(socket_value->value, value.c_str());
  tag_keyboard_key_socket_changed(C, ntree, socket);
}

static void free_keyboard_key_capture_data(wmOperator *op)
{
  MEM_delete(static_cast<LogicKeyboardKeyCaptureData *>(op->customdata));
  op->customdata = nullptr;
}

static wmOperatorStatus logic_keyboard_key_selector_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent * /*event*/)
{
  bNodeTree *ntree = nullptr;
  bNodeSocket *socket = resolve_keyboard_key_socket(C, op, &ntree);
  if (ntree == nullptr || socket == nullptr) {
    return OPERATOR_CANCELLED;
  }

  auto *socket_value = static_cast<bNodeSocketValueString *>(socket->default_value);
  auto *data = MEM_new<LogicKeyboardKeyCaptureData>(__func__);
  data->tree_name = ntree->id.name + 2;
  data->node_identifier = RNA_int_get(op->ptr, "node_identifier");
  data->socket_identifier = socket->identifier;
  data->old_value = socket_value->value;

  RNA_string_set(op->ptr, "tree_name", data->tree_name.c_str());
  RNA_string_set(op->ptr, "socket_identifier", data->socket_identifier.c_str());

  op->customdata = data;
  set_keyboard_key_socket_value(C, *ntree, *socket, "Press a key...");
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus logic_keyboard_key_selector_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  auto *data = static_cast<LogicKeyboardKeyCaptureData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (event->val != KM_PRESS) {
    return OPERATOR_PASS_THROUGH;
  }

  bNodeTree *ntree = nullptr;
  bNodeSocket *socket = resolve_keyboard_key_socket(C, op, &ntree);
  if (ntree == nullptr || socket == nullptr) {
    free_keyboard_key_capture_data(op);
    return OPERATOR_CANCELLED;
  }

  if (ELEM(event->type, LEFTMOUSE, MIDDLEMOUSE, RIGHTMOUSE)) {
    set_keyboard_key_socket_value(C, *ntree, *socket, data->old_value);
    free_keyboard_key_capture_data(op);
    return OPERATOR_CANCELLED;
  }

  const char *identifier = nullptr;
  if (!RNA_enum_identifier(rna_enum_event_type_items, event->type, &identifier) ||
      identifier == nullptr || identifier[0] == '\0')
  {
    return OPERATOR_PASS_THROUGH;
  }

  set_keyboard_key_socket_value(C, *ntree, *socket, identifier);
  free_keyboard_key_capture_data(op);
  return OPERATOR_FINISHED;
}

static void logic_keyboard_key_selector_cancel(bContext *C, wmOperator *op)
{
  auto *data = static_cast<LogicKeyboardKeyCaptureData *>(op->customdata);
  if (data == nullptr) {
    return;
  }

  bNodeTree *ntree = nullptr;
  bNodeSocket *socket = resolve_keyboard_key_socket(C, op, &ntree);
  if (ntree != nullptr && socket != nullptr) {
    const auto *socket_value = static_cast<const bNodeSocketValueString *>(socket->default_value);
    if (socket_value != nullptr && STREQ(socket_value->value, "Press a key...")) {
      set_keyboard_key_socket_value(C, *ntree, *socket, data->old_value);
    }
  }

  free_keyboard_key_capture_data(op);
}

static bool logic_keyboard_key_selector_poll(bContext *C)
{
  return logic_tree_from_context(C) != nullptr;
}

void NODE_OT_validate_logic_tree(wmOperatorType *ot)
{
  ot->name = "Validate Logic Tree";
  ot->description = "Compile the LogicNodeTree without starting Play and show compile diagnostics";
  ot->idname = "NODE_OT_validate_logic_tree";

  ot->exec = validate_logic_tree_exec;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna,
                 "tree_name",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Tree Name",
                 "Optional LogicNodeTree data-block name to validate");
}

void NODE_OT_logic_keyboard_key_selector(wmOperatorType *ot)
{
  ot->name = "Select Logic Keyboard Key";
  ot->description = "Press a key to assign it to this Logic Keyboard Key node";
  ot->idname = "NODE_OT_logic_keyboard_key_selector";

  ot->invoke = logic_keyboard_key_selector_invoke;
  ot->modal = logic_keyboard_key_selector_modal;
  ot->cancel = logic_keyboard_key_selector_cancel;
  ot->poll = logic_keyboard_key_selector_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_string(ot->srna,
                 "tree_name",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Tree Name",
                 "LogicNodeTree data-block name");
  RNA_def_int(ot->srna,
              "node_identifier",
              0,
              0,
              INT_MAX,
              "Node Identifier",
              "Runtime-stable identifier of the Keyboard Key node",
              0,
              INT_MAX);
  RNA_def_string(ot->srna,
                 "socket_identifier",
                 nullptr,
                 64,
                 "Socket Identifier",
                 "Identifier of the Keyboard Key input socket");
}

}  // namespace blender::ed::space_node
