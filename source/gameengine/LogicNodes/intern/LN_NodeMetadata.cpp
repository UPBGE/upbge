/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_NodeMetadata.cpp
 *  \ingroup logicnodes
 */

#include "LN_NodeMetadata.h"

#include <utility>
#include <unordered_set>

namespace {

LN_NodeSchedulerPolicy scheduler_policy_for_definition(
    const LN_NodeDefinition &definition,
    const LN_TreeCompiler::CompileHandlerInfo &handler_info)
{
  if (definition.execution_class == LN_ExecutionClass::MainThreadOnly) {
    return LN_NodeSchedulerPolicy::MainThreadOnly;
  }
  if (definition.execution_class == LN_ExecutionClass::CommandEmitting ||
      definition.has_side_effects || handler_info.emits_commands)
  {
    return LN_NodeSchedulerPolicy::CommandEmitting;
  }
  return LN_NodeSchedulerPolicy::SnapshotReadOnly;
}

LN_NodeVisibility visibility_for_handler_info(
    const LN_TreeCompiler::CompileHandlerInfo &handler_info)
{
  return handler_info.has_runtime_implementation ? LN_NodeVisibility::Release :
                                                    LN_NodeVisibility::Hidden;
}

std::vector<LN_NodeMetadataContract> build_metadata_contracts()
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  std::vector<LN_NodeMetadataContract> contracts;
  contracts.reserve(registry.GetNodeDefinitions().size());

  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    const LN_TreeCompiler::CompileHandlerDescriptor *handler_descriptor =
        LN_TreeCompiler::FindCompileHandlerDescriptor(definition.kind);

    LN_NodeMetadataContract contract;
    contract.idname = definition.idname;
    contract.kind = definition.kind;
    contract.category = definition.category;
    contract.inputs = definition.inputs;
    contract.outputs = definition.outputs;
    contract.has_side_effects = definition.has_side_effects;
    contract.required_phase = definition.required_phase;
    contract.requires_jolt = definition.requires_jolt;
    contract.future_pure_batchable = definition.future_pure_batchable;
    contract.execution_class = definition.execution_class;
    if (handler_descriptor != nullptr) {
      contract.compile_handler_info = handler_descriptor->info;
      contract.compile_handler_id = handler_descriptor->handler_id != nullptr ?
                                        handler_descriptor->handler_id :
                                        "";
    }
    contract.scheduler_policy = scheduler_policy_for_definition(definition,
                                                                contract.compile_handler_info);
    contract.visibility = visibility_for_handler_info(contract.compile_handler_info);
    contracts.push_back(std::move(contract));
  }

  return contracts;
}

std::string metadata_error_prefix(const LN_NodeMetadataContract &contract)
{
  return contract.idname.empty() ? std::string("<empty node id>") : contract.idname;
}

void add_error(std::vector<std::string> &errors,
               const LN_NodeMetadataContract &contract,
               const std::string &message)
{
  errors.push_back(metadata_error_prefix(contract) + ": " + message);
}

bool pins_match(const std::vector<LN_PinDefinition> &lhs,
                const std::vector<LN_PinDefinition> &rhs)
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t index = 0; index < lhs.size(); index++) {
    const LN_PinDefinition &left = lhs[index];
    const LN_PinDefinition &right = rhs[index];
    if (left.name != right.name || left.socket_idname != right.socket_idname ||
        left.kind != right.kind || left.value_type != right.value_type ||
        left.requires_link != right.requires_link)
    {
      return false;
    }
  }
  return true;
}

bool validate_contract_table(const std::vector<LN_NodeMetadataContract> &contracts,
                             std::vector<std::string> &errors)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  if (contracts.size() != registry.GetNodeDefinitions().size()) {
    errors.push_back("node metadata contract count does not match registry definition count");
  }

  std::unordered_set<std::string> seen_idnames;

  for (const LN_NodeMetadataContract &contract : contracts) {
    if (contract.idname.empty()) {
      add_error(errors, contract, "node idname is empty");
    }
    else if (!seen_idnames.insert(contract.idname).second) {
      add_error(errors, contract, "duplicate node idname in metadata contract table");
    }

    const LN_NodeDefinition *definition = registry.FindNodeDefinition(contract.idname);
    if (definition == nullptr) {
      add_error(errors, contract, "metadata contract has no registry definition");
      continue;
    }

    if (contract.kind != definition->kind) {
      add_error(errors, contract, "node kind does not match registry definition");
    }
    if (contract.category != definition->category) {
      add_error(errors, contract, "category does not match registry definition");
    }
    if (!pins_match(contract.inputs, definition->inputs)) {
      add_error(errors, contract, "input pins do not match registry definition");
    }
    if (!pins_match(contract.outputs, definition->outputs)) {
      add_error(errors, contract, "output pins do not match registry definition");
    }
    if (contract.has_side_effects != definition->has_side_effects) {
      add_error(errors, contract, "side-effect flag does not match registry definition");
    }
    if (contract.required_phase != definition->required_phase) {
      add_error(errors, contract, "required phase does not match registry definition");
    }
    if (contract.requires_jolt != definition->requires_jolt) {
      add_error(errors, contract, "Jolt requirement does not match registry definition");
    }
    if (contract.future_pure_batchable != definition->future_pure_batchable) {
      add_error(errors, contract, "future pure-batchable flag does not match registry definition");
    }
    if (contract.execution_class != definition->execution_class) {
      add_error(errors, contract, "execution class does not match registry definition");
    }

    const LN_TreeCompiler::CompileHandlerDescriptor *handler_descriptor =
        LN_TreeCompiler::FindCompileHandlerDescriptor(contract.kind);
    if (handler_descriptor == nullptr) {
      add_error(errors, contract, "metadata contract has no compiler handler descriptor");
      continue;
    }

    if (handler_descriptor->handler_id == nullptr || handler_descriptor->handler_id[0] == '\0') {
      add_error(errors, contract, "compiler handler id is empty");
    }
    else if (contract.compile_handler_id != handler_descriptor->handler_id) {
      add_error(errors, contract, "compiler handler id does not match descriptor");
    }

    const LN_TreeCompiler::CompileHandlerInfo &info = contract.compile_handler_info;
    const LN_TreeCompiler::CompileHandlerInfo &expected_info = handler_descriptor->info;
    if (info.kind != expected_info.kind || info.emits_commands != expected_info.emits_commands ||
        info.requires_flow_input != expected_info.requires_flow_input ||
        info.has_runtime_implementation != expected_info.has_runtime_implementation)
    {
      add_error(errors, contract, "compiler handler info does not match descriptor");
    }

    if (info.emits_commands != contract.has_side_effects) {
      add_error(errors, contract, "compiler command emission does not match side-effect flag");
    }
    if (contract.has_side_effects &&
        contract.execution_class != LN_ExecutionClass::CommandEmitting)
    {
      add_error(errors, contract, "side-effect node is not classified as command-emitting");
    }
    if (contract.has_side_effects &&
        info.kind != LN_TreeCompiler::CompileHandlerKind::Command)
    {
      add_error(errors, contract, "side-effect node does not use a command compiler handler");
    }
    if (info.kind == LN_TreeCompiler::CompileHandlerKind::Command && !contract.has_side_effects) {
      add_error(errors, contract, "command compiler handler is missing side-effect metadata");
    }
    if (info.kind == LN_TreeCompiler::CompileHandlerKind::Command && !info.requires_flow_input) {
      add_error(errors, contract, "command compiler handler does not require flow input");
    }

    const LN_NodeSchedulerPolicy expected_policy = scheduler_policy_for_definition(*definition,
                                                                                   expected_info);
    if (contract.scheduler_policy != expected_policy) {
      add_error(errors, contract, "scheduler policy does not match node and compiler metadata");
    }
    if (contract.scheduler_policy == LN_NodeSchedulerPolicy::MainThreadOnly &&
        contract.execution_class != LN_ExecutionClass::MainThreadOnly)
    {
      add_error(errors, contract, "main-thread scheduler policy lacks main-thread execution class");
    }
    if (contract.execution_class == LN_ExecutionClass::MainThreadOnly &&
        contract.scheduler_policy != LN_NodeSchedulerPolicy::MainThreadOnly)
    {
      add_error(errors, contract, "main-thread execution class lacks main-thread scheduler policy");
    }

    const LN_NodeVisibility expected_visibility = visibility_for_handler_info(expected_info);
    if (contract.visibility != expected_visibility) {
      add_error(errors, contract, "visibility does not match runtime implementation support");
    }
  }

  return errors.empty();
}

}  // namespace

const std::vector<LN_NodeMetadataContract> &LN_GetNodeMetadataContracts()
{
  static const std::vector<LN_NodeMetadataContract> contracts = build_metadata_contracts();
  return contracts;
}

const LN_NodeMetadataContract *LN_FindNodeMetadataContract(const std::string &idname)
{
  for (const LN_NodeMetadataContract &contract : LN_GetNodeMetadataContracts()) {
    if (contract.idname == idname) {
      return &contract;
    }
  }
  return nullptr;
}

bool LN_ValidateNodeMetadataContracts(std::vector<std::string> *r_errors)
{
  return LN_ValidateNodeMetadataContracts(LN_GetNodeMetadataContracts(), r_errors);
}

bool LN_ValidateNodeMetadataContracts(const std::vector<LN_NodeMetadataContract> &contracts,
                                      std::vector<std::string> *r_errors)
{
  std::vector<std::string> errors;
  const bool valid = validate_contract_table(contracts, errors);
  if (r_errors != nullptr) {
    *r_errors = std::move(errors);
  }
  return valid;
}
