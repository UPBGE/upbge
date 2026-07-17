/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "LN_NodeMetadata.h"
#include "LN_NodeRegistry.h"
#include "LN_TreeCompiler.h"

#include "nodes/NOD_logic_descriptors.hh"

namespace {

using blender::nodes::logic::ExecutionClass;
using blender::nodes::logic::NodeMetadata;
using blender::nodes::logic::PinMetadata;
using blender::nodes::logic::PinType;
using blender::nodes::logic::RequiredPhase;

const LN_PinDefinition *FindPin(const std::vector<LN_PinDefinition> &pins, const char *name)
{
  for (const LN_PinDefinition &pin : pins) {
    if (pin.name == name) {
      return &pin;
    }
  }
  return nullptr;
}

const NodeMetadata *FindSharedMetadata(const char *idname)
{
  for (const NodeMetadata &metadata : blender::nodes::logic::logic_node_metadata()) {
    if (std::strcmp(metadata.idname, idname) == 0) {
      return &metadata;
    }
  }
  return nullptr;
}

LN_ValueType ValueTypeForPin(const PinType type)
{
  switch (type) {
    case PinType::Execution:
      return LN_ValueType::None;
    case PinType::Condition:
    case PinType::Bool:
      return LN_ValueType::Bool;
    case PinType::Int:
    case PinType::CollisionLayers:
      return LN_ValueType::Int;
    case PinType::Float:
      return LN_ValueType::Float;
    case PinType::String:
      return LN_ValueType::String;
    case PinType::Vector:
    case PinType::VectorXYAngle:
      return LN_ValueType::Vector;
    case PinType::Rotation:
      return LN_ValueType::Rotation;
    case PinType::Color:
      return LN_ValueType::Color;
    case PinType::Object:
      return LN_ValueType::ObjectRef;
    case PinType::List:
      return LN_ValueType::List;
    case PinType::Dictionary:
      return LN_ValueType::Dict;
    case PinType::GeometryTree:
    case PinType::Material:
    case PinType::Image:
    case PinType::Sound:
    case PinType::Font:
    case PinType::Text:
    case PinType::Mesh:
    case PinType::Datablock:
      return LN_ValueType::DatablockRef;
    case PinType::Collection:
      return LN_ValueType::CollectionRef;
    case PinType::Scene:
      return LN_ValueType::SceneRef;
    case PinType::Generic:
    case PinType::UI:
      return LN_ValueType::Generic;
    case PinType::Python:
      return LN_ValueType::None;
  }
  return LN_ValueType::None;
}

LN_PinKind PinKindForPin(const PinType type)
{
  switch (type) {
    case PinType::Execution:
      return LN_PinKind::Execution;
    case PinType::Condition:
      return LN_PinKind::Condition;
    default:
      return LN_PinKind::Value;
  }
}

LN_RequiredPhase RequiredPhaseForMetadata(const RequiredPhase phase)
{
  switch (phase) {
    case RequiredPhase::None:
      return LN_RequiredPhase::None;
    case RequiredPhase::OnInit:
      return LN_RequiredPhase::OnInit;
    case RequiredPhase::FixedUpdate:
      return LN_RequiredPhase::FixedUpdate;
  }
  return LN_RequiredPhase::None;
}

LN_ExecutionClass ExecutionClassForMetadata(const ExecutionClass execution_class)
{
  switch (execution_class) {
    case ExecutionClass::SnapshotReadOnly:
      return LN_ExecutionClass::SnapshotReadOnly;
    case ExecutionClass::CommandEmitting:
      return LN_ExecutionClass::CommandEmitting;
    case ExecutionClass::MainThreadOnly:
      return LN_ExecutionClass::MainThreadOnly;
  }
  return LN_ExecutionClass::SnapshotReadOnly;
}

LN_NodeSchedulerPolicy ExpectedSchedulerPolicy(
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

void ExpectPinsMatch(const std::vector<LN_PinDefinition> &definitions,
                     const blender::Span<PinMetadata> metadata_pins,
                     const char *node_idname)
{
  EXPECT_EQ(definitions.size(), metadata_pins.size()) << node_idname;
  for (const PinMetadata &pin_metadata : metadata_pins) {
    const LN_PinDefinition *pin_definition = FindPin(definitions, pin_metadata.identifier);
    ASSERT_NE(pin_definition, nullptr) << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->kind, PinKindForPin(pin_metadata.type))
      << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->value_type, ValueTypeForPin(pin_metadata.type))
        << node_idname << "." << pin_metadata.identifier;
    const char *expected_socket_idname =
        blender::nodes::logic::pin_type_socket_idname(pin_metadata.type);
    EXPECT_EQ(pin_definition->socket_idname,
              expected_socket_idname ? expected_socket_idname : "")
        << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->requires_link, pin_metadata.requires_link)
        << node_idname << "." << pin_metadata.identifier;
  }
}

bool HasErrorContaining(const std::vector<std::string> &errors, const std::string &text)
{
  for (const std::string &error : errors) {
    if (error.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::vector<LN_TreeCompiler::CompileHandlerDescriptor> CopyCompileHandlerDescriptors()
{
  size_t handler_count = 0;
  const LN_TreeCompiler::CompileHandlerDescriptor *descriptors =
      LN_TreeCompiler::GetCompileHandlerDescriptors(handler_count);
  return std::vector<LN_TreeCompiler::CompileHandlerDescriptor>(descriptors,
                                                                descriptors + handler_count);
}

}  // namespace

TEST(LN_NodeRegistry, MirrorsSharedDescriptors)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const blender::Span<NodeMetadata> metadata_items =
      blender::nodes::logic::logic_node_metadata();

  EXPECT_EQ(registry.GetNodeDefinitions().size(), metadata_items.size());

  for (const NodeMetadata &metadata : metadata_items) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(metadata.idname);
    ASSERT_NE(definition, nullptr) << metadata.idname;

    EXPECT_EQ(definition->category, metadata.category) << metadata.idname;
    EXPECT_EQ(definition->ui_name, metadata.ui_name) << metadata.idname;
    EXPECT_EQ(definition->ui_description, metadata.ui_description) << metadata.idname;
    EXPECT_EQ(definition->has_side_effects, metadata.has_side_effects) << metadata.idname;
    EXPECT_EQ(definition->required_phase, RequiredPhaseForMetadata(metadata.required_phase))
        << metadata.idname;
    EXPECT_EQ(definition->requires_jolt, metadata.requires_jolt) << metadata.idname;
    EXPECT_EQ(definition->future_pure_batchable, metadata.future_pure_batchable)
        << metadata.idname;
    EXPECT_EQ(definition->execution_class, ExecutionClassForMetadata(metadata.execution_class))
        << metadata.idname;

    ExpectPinsMatch(definition->inputs, metadata.inputs, metadata.idname);
    ExpectPinsMatch(definition->outputs, metadata.outputs, metadata.idname);
  }
}

TEST(LN_NodeRegistry, MetadataContractJoinsDescriptorsRegistryAndHandlers)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const std::vector<LN_NodeMetadataContract> &contracts = LN_GetNodeMetadataContracts();

  EXPECT_EQ(contracts.size(), registry.GetNodeDefinitions().size());

  std::unordered_set<std::string> seen_idnames;
  for (const LN_NodeMetadataContract &contract : contracts) {
    EXPECT_TRUE(seen_idnames.insert(contract.idname).second) << contract.idname;

    const LN_NodeDefinition *definition = registry.FindNodeDefinition(contract.idname);
    ASSERT_NE(definition, nullptr) << contract.idname;
    const NodeMetadata *metadata = FindSharedMetadata(contract.idname.c_str());
    ASSERT_NE(metadata, nullptr) << contract.idname;
    const LN_TreeCompiler::CompileHandlerDescriptor *handler_descriptor =
        LN_TreeCompiler::FindCompileHandlerDescriptor(contract.kind);
    ASSERT_NE(handler_descriptor, nullptr) << contract.idname;

    EXPECT_EQ(contract.kind, definition->kind) << contract.idname;
    EXPECT_EQ(contract.category, definition->category) << contract.idname;
    EXPECT_EQ(contract.category, metadata->category) << contract.idname;
    EXPECT_EQ(contract.has_side_effects, definition->has_side_effects) << contract.idname;
    EXPECT_EQ(contract.required_phase, definition->required_phase) << contract.idname;
    EXPECT_EQ(contract.requires_jolt, definition->requires_jolt) << contract.idname;
    EXPECT_EQ(contract.future_pure_batchable, definition->future_pure_batchable)
        << contract.idname;
    EXPECT_EQ(contract.execution_class, definition->execution_class) << contract.idname;

    ExpectPinsMatch(contract.inputs, metadata->inputs, contract.idname.c_str());
    ExpectPinsMatch(contract.outputs, metadata->outputs, contract.idname.c_str());

    ASSERT_NE(handler_descriptor->handler_id, nullptr) << contract.idname;
    EXPECT_EQ(contract.compile_handler_id, handler_descriptor->handler_id) << contract.idname;
    EXPECT_EQ(contract.compile_handler_info.kind, handler_descriptor->info.kind)
        << contract.idname;
    EXPECT_EQ(contract.compile_handler_info.emits_commands,
              handler_descriptor->info.emits_commands)
        << contract.idname;
    EXPECT_EQ(contract.compile_handler_info.requires_flow_input,
              handler_descriptor->info.requires_flow_input)
        << contract.idname;
    EXPECT_EQ(contract.compile_handler_info.has_runtime_implementation,
              handler_descriptor->info.has_runtime_implementation)
        << contract.idname;

    EXPECT_EQ(contract.scheduler_policy,
              ExpectedSchedulerPolicy(*definition, contract.compile_handler_info))
        << contract.idname;
    if (contract.visibility == LN_NodeVisibility::Hidden) {
      EXPECT_FALSE(contract.compile_handler_info.has_runtime_implementation) << contract.idname;
    }
    else {
      EXPECT_TRUE(contract.compile_handler_info.has_runtime_implementation) << contract.idname;
    }
  }
}

TEST(LN_NodeRegistry, MetadataContractValidationAcceptsBuiltinContracts)
{
  std::vector<std::string> errors;
  EXPECT_TRUE(LN_ValidateNodeMetadataContracts(&errors));
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST(LN_NodeRegistry, MetadataContractValidationRejectsSchedulerPolicyDrift)
{
  std::vector<LN_NodeMetadataContract> contracts = LN_GetNodeMetadataContracts();
  bool mutated = false;
  for (LN_NodeMetadataContract &contract : contracts) {
    if (contract.idname == "LogicNativeSetWorldPosition") {
      contract.scheduler_policy = LN_NodeSchedulerPolicy::SnapshotReadOnly;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_ValidateNodeMetadataContracts(contracts, &errors));
  EXPECT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("scheduler policy"), std::string::npos);
}

TEST(LN_NodeRegistry, MetadataContractValidationRejectsCompilerHandlerInfoDrift)
{
  std::vector<LN_NodeMetadataContract> contracts = LN_GetNodeMetadataContracts();
  bool mutated = false;
  for (LN_NodeMetadataContract &contract : contracts) {
    if (contract.idname == "LogicNativeSetWorldPosition") {
      contract.compile_handler_info.emits_commands = false;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_ValidateNodeMetadataContracts(contracts, &errors));
  EXPECT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("compiler handler info"), std::string::npos);
}

TEST(LN_NodeRegistry, MetadataContractValidationRejectsVisibilityDrift)
{
  std::vector<LN_NodeMetadataContract> contracts = LN_GetNodeMetadataContracts();
  bool mutated = false;
  for (LN_NodeMetadataContract &contract : contracts) {
    if (contract.idname == "LogicNativeSetWorldPosition") {
      contract.visibility = LN_NodeVisibility::Hidden;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_ValidateNodeMetadataContracts(contracts, &errors));
  EXPECT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("visibility"), std::string::npos);
}

TEST(LN_NodeRegistry, MetadataContractValidationRejectsDuplicateIdnames)
{
  std::vector<LN_NodeMetadataContract> contracts = LN_GetNodeMetadataContracts();
  ASSERT_GE(contracts.size(), 2u);
  contracts[1].idname = contracts[0].idname;

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_ValidateNodeMetadataContracts(contracts, &errors));
  EXPECT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("idname"), std::string::npos);
}

TEST(LN_NodeRegistry, MetadataContractHidesUnsupportedRuntimeHandlers)
{
  static const char *unsupported_runtime_idnames[] = {
      "LogicNativeSetNodeGroupSocketValue",
      "LogicNativePlayMaterialSequence",
      "LogicNativeLoadFileContent",
      "LogicNativeSetCustomCursor",
  };

  for (const char *idname : unsupported_runtime_idnames) {
    const LN_NodeMetadataContract *contract = LN_FindNodeMetadataContract(idname);
    ASSERT_NE(contract, nullptr) << idname;
    EXPECT_FALSE(contract->compile_handler_info.has_runtime_implementation) << idname;
    EXPECT_EQ(contract->visibility, LN_NodeVisibility::Hidden) << idname;
    EXPECT_EQ(contract->compile_handler_info.kind, LN_TreeCompiler::CompileHandlerKind::Command)
        << idname;
  }

  const LN_NodeMetadataContract *set_position = LN_FindNodeMetadataContract(
      "LogicNativeSetWorldPosition");
  ASSERT_NE(set_position, nullptr);
  EXPECT_TRUE(set_position->compile_handler_info.has_runtime_implementation);
  EXPECT_EQ(set_position->visibility, LN_NodeVisibility::Release);
}

TEST(LN_NodeRegistry, ClassifiesCurrentNodesForCommandBufferExecution)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();

  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    if (definition.has_side_effects) {
      EXPECT_EQ(definition.execution_class, LN_ExecutionClass::CommandEmitting)
          << definition.idname;
    }
    else {
      EXPECT_NE(definition.execution_class, LN_ExecutionClass::CommandEmitting)
          << definition.idname;
    }
  }

  const LN_NodeDefinition *get_position = registry.FindNodeDefinition("LogicNativeGetWorldPosition");
  ASSERT_NE(get_position, nullptr);
  EXPECT_FALSE(get_position->has_side_effects);
  EXPECT_TRUE(get_position->future_pure_batchable);

  const LN_NodeDefinition *set_position = registry.FindNodeDefinition("LogicNativeSetWorldPosition");
  ASSERT_NE(set_position, nullptr);
  EXPECT_TRUE(set_position->has_side_effects);
  EXPECT_EQ(set_position->execution_class, LN_ExecutionClass::CommandEmitting);
}

TEST(LN_NodeRegistry, AllRegisteredNodesHaveCompilerHandlerClassifications)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();

  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    const LN_TreeCompiler::CompileHandlerInfo handler_info =
        LN_TreeCompiler::GetCompileHandlerInfo(definition.kind);
    const LN_TreeCompiler::CompileHandlerDescriptor *handler_descriptor =
      LN_TreeCompiler::FindCompileHandlerDescriptor(definition.kind);
    ASSERT_NE(handler_descriptor, nullptr) << definition.idname;
    ASSERT_NE(handler_descriptor->handler_id, nullptr) << definition.idname;
    EXPECT_NE(handler_info.kind, LN_TreeCompiler::CompileHandlerKind::Unsupported)
        << definition.idname;
    EXPECT_EQ(handler_info.emits_commands, definition.has_side_effects) << definition.idname;
    if (definition.has_side_effects) {
      EXPECT_EQ(handler_info.kind, LN_TreeCompiler::CompileHandlerKind::Command)
          << definition.idname;
    }
  }

  const LN_NodeDefinition *event = registry.FindNodeDefinition("LogicNativeOnInit");
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileHandlerInfo(event->kind).kind,
            LN_TreeCompiler::CompileHandlerKind::EventSource);

  const LN_NodeDefinition *set_position = registry.FindNodeDefinition("LogicNativeSetWorldPosition");
  ASSERT_NE(set_position, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileHandlerInfo(set_position->kind).kind,
            LN_TreeCompiler::CompileHandlerKind::Command);
}

TEST(LN_NodeRegistry, ReceiveEventExposesConditionAndExecutionOutputs)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *receive_event =
      registry.FindNodeDefinition("LogicNativeReceiveEvent");
  ASSERT_NE(receive_event, nullptr);

  const LN_PinDefinition *received = FindPin(receive_event->outputs, "Received");
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->kind, LN_PinKind::Condition);
  EXPECT_EQ(received->value_type, LN_ValueType::Bool);

  const LN_PinDefinition *out = FindPin(receive_event->outputs, "Out");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->kind, LN_PinKind::Execution);
  EXPECT_EQ(out->value_type, LN_ValueType::None);
  EXPECT_EQ(out->socket_idname, "NodeSocketLogicExecution");

  EXPECT_FALSE(receive_event->has_side_effects);
  EXPECT_EQ(receive_event->required_phase, LN_RequiredPhase::FixedUpdate);
}

TEST(LN_NodeRegistry, CompilerHandlerDescriptorTableHasUniqueNodeKinds)
{
  size_t handler_count = 0;
  const LN_TreeCompiler::CompileHandlerDescriptor *descriptors =
      LN_TreeCompiler::GetCompileHandlerDescriptors(handler_count);
  ASSERT_NE(descriptors, nullptr);
  ASSERT_GT(handler_count, 0u);

  std::unordered_set<int> seen_kinds;
  for (size_t index = 0; index < handler_count; index++) {
    const LN_TreeCompiler::CompileHandlerDescriptor &descriptor = descriptors[index];
    EXPECT_TRUE(seen_kinds.insert(int(descriptor.node_kind)).second) << descriptor.handler_id;
    EXPECT_NE(descriptor.handler_id, nullptr);
    EXPECT_NE(descriptor.info.kind, LN_TreeCompiler::CompileHandlerKind::Unsupported)
        << descriptor.handler_id;
  }
}

TEST(LN_NodeRegistry, CompilerHandlerInventoryValidationAcceptsBuiltinHandlers)
{
  std::vector<std::string> errors;
  EXPECT_TRUE(LN_TreeCompiler::ValidateCompileHandlerInventory(&errors));
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST(LN_NodeRegistry, CompilerHandlerDescriptorValidationRejectsDuplicateNodeKinds)
{
  std::vector<LN_TreeCompiler::CompileHandlerDescriptor> descriptors =
      CopyCompileHandlerDescriptors();
  ASSERT_GE(descriptors.size(), 2u);
  descriptors[1].node_kind = descriptors[0].node_kind;

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_TreeCompiler::ValidateCompileHandlerDescriptors(
      descriptors.data(), descriptors.size(), &errors));
  EXPECT_TRUE(HasErrorContaining(errors, "duplicate compiler handler node kind"));
}

TEST(LN_NodeRegistry, CompilerHandlerDescriptorValidationRejectsCommandFlowDrift)
{
  std::vector<LN_TreeCompiler::CompileHandlerDescriptor> descriptors =
      CopyCompileHandlerDescriptors();
  bool mutated = false;
  for (LN_TreeCompiler::CompileHandlerDescriptor &descriptor : descriptors) {
    if (descriptor.node_kind == LN_NodeKind::SetObjectAttribute) {
      descriptor.info.requires_flow_input = false;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_TreeCompiler::ValidateCompileHandlerDescriptors(
      descriptors.data(), descriptors.size(), &errors));
  EXPECT_TRUE(HasErrorContaining(errors, "command handler does not require flow input"));
}

TEST(LN_NodeRegistry, CompilerHandlerDescriptorValidationRejectsCommandEmissionDrift)
{
  std::vector<LN_TreeCompiler::CompileHandlerDescriptor> descriptors =
      CopyCompileHandlerDescriptors();
  bool mutated = false;
  for (LN_TreeCompiler::CompileHandlerDescriptor &descriptor : descriptors) {
    if (descriptor.node_kind == LN_NodeKind::SetObjectAttribute) {
      descriptor.info.emits_commands = false;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_TreeCompiler::ValidateCompileHandlerDescriptors(
      descriptors.data(), descriptors.size(), &errors));
  EXPECT_TRUE(HasErrorContaining(errors, "command handler does not emit commands"));
}

TEST(LN_NodeRegistry, CompilerHandlerDescriptorValidationRejectsUnsupportedExpressionRuntime)
{
  std::vector<LN_TreeCompiler::CompileHandlerDescriptor> descriptors =
      CopyCompileHandlerDescriptors();
  bool mutated = false;
  for (LN_TreeCompiler::CompileHandlerDescriptor &descriptor : descriptors) {
    if (descriptor.node_kind == LN_NodeKind::GetGravity) {
      descriptor.info.has_runtime_implementation = false;
      mutated = true;
      break;
    }
  }
  ASSERT_TRUE(mutated);

  std::vector<std::string> errors;
  EXPECT_FALSE(LN_TreeCompiler::ValidateCompileHandlerDescriptors(
      descriptors.data(), descriptors.size(), &errors));
  EXPECT_TRUE(HasErrorContaining(errors,
                                 "only command handlers may be marked unsupported at runtime"));
}

TEST(LN_NodeRegistry, GetCompileDispatchClassifiesHandlerTable)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();

  const LN_NodeDefinition *on_init = registry.FindNodeDefinition("LogicNativeOnInit");
  ASSERT_NE(on_init, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(on_init->kind),
            LN_TreeCompiler::CompileDispatch::EventSource);

  const LN_NodeDefinition *value_bool = registry.FindNodeDefinition("LogicNativeValueBool");
  ASSERT_NE(value_bool, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(value_bool->kind),
            LN_TreeCompiler::CompileDispatch::ConstantOutput);

  const LN_NodeDefinition *set_position = registry.FindNodeDefinition("LogicNativeSetWorldPosition");
  ASSERT_NE(set_position, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(set_position->kind),
            LN_TreeCompiler::CompileDispatch::CustomCompile);

  const LN_NodeDefinition *branch = registry.FindNodeDefinition("LogicNativeBranch");
  ASSERT_NE(branch, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(branch->kind),
            LN_TreeCompiler::CompileDispatch::CustomCompile);
}

TEST(LN_NodeRegistry, DataContainerNodesUseCustomCompileDispatch)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  static const char *data_container_idnames[] = {
      "LogicNativeListContains",
      "LogicNativeDictHasKey",
      "LogicNativeDictLength",
      "LogicNativeListLength",
      "LogicNativeListGetItem",
      "LogicNativeListDuplicate",
      "LogicNativeDictMerge",
      "LogicNativeListExtend",
      "LogicNativeDictGetKey",
      "LogicNativeEmptyList",
      "LogicNativeEmptyDict",
      "LogicNativeDictGetKeys",
      "LogicNativeMakeDict",
      "LogicNativeMakeList",
  };

  for (const char *idname : data_container_idnames) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(definition->kind),
              LN_TreeCompiler::CompileDispatch::CustomCompile)
        << idname;
  }
}

TEST(LN_NodeRegistry, SnapshotTransformNodesUseCustomCompileDispatch)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  static const char *snapshot_transform_idnames[] = {
      "LogicNativeGetWorldPosition",
      "LogicNativeGetLocalPosition",
      "LogicNativeGetWorldOrientation",
      "LogicNativeGetLocalOrientation",
      "LogicNativeGetWorldScale",
      "LogicNativeGetLocalScale",
      "LogicNativeGetLinearVelocity",
      "LogicNativeGetLocalLinearVelocity",
      "LogicNativeGetAngularVelocity",
      "LogicNativeGetLocalAngularVelocity",
      "LogicNativeGetGravity",
  };

  for (const char *idname : snapshot_transform_idnames) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(definition->kind),
              LN_TreeCompiler::CompileDispatch::CustomCompile)
        << idname;
    EXPECT_NE(LN_TreeCompiler::FindCompileHandlerDescriptor(definition->kind), nullptr)
        << idname;
  }
}

TEST(LN_NodeRegistry, Phase5ContentPipelineNodesAreRegisteredWithHandlers)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  static const char *content_pipeline_idnames[] = {
      "LogicNativePlaySound3D",
      "LogicNativePauseSound",
      "LogicNativeResumeSound",
      "LogicNativeGetBonePoseRotation",
      "LogicNativeSetBonePoseRotation",
      "LogicNativeSetBonePoseTransform",
      "LogicNativeSetBoneConstraintInfluence",
      "LogicNativeSetMaterialSlot",
      "LogicNativeSetMaterialParameter",
      "LogicNativeGetMaterialParameter",
      "LogicNativeEvaluateCurve",
  };

  for (const char *idname : content_pipeline_idnames) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_NE(LN_TreeCompiler::FindCompileHandlerDescriptor(definition->kind), nullptr)
        << idname;
    const LN_TreeCompiler::CompileDispatch dispatch =
        LN_TreeCompiler::GetCompileDispatch(definition->kind);
    if (definition->has_side_effects) {
      EXPECT_EQ(dispatch, LN_TreeCompiler::CompileDispatch::CustomCompile) << idname;
    }
    else {
      EXPECT_TRUE(dispatch == LN_TreeCompiler::CompileDispatch::CustomCompile ||
                  dispatch == LN_TreeCompiler::CompileDispatch::ConstantOutput)
          << idname;
    }
  }
}

TEST(LN_NodeRegistry, ExpressionHandlersUseCustomCompileDispatch)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    const LN_TreeCompiler::CompileHandlerInfo info =
        LN_TreeCompiler::GetCompileHandlerInfo(definition.kind);
    if (info.kind != LN_TreeCompiler::CompileHandlerKind::Expression &&
        info.kind != LN_TreeCompiler::CompileHandlerKind::ValueExpression)
    {
      continue;
    }
    if (definition.has_side_effects) {
      continue;
    }
    const LN_TreeCompiler::CompileDispatch dispatch =
        LN_TreeCompiler::GetCompileDispatch(definition.kind);
    EXPECT_TRUE(dispatch == LN_TreeCompiler::CompileDispatch::CustomCompile ||
                dispatch == LN_TreeCompiler::CompileDispatch::ConstantOutput)
        << definition.idname;
  }
}
