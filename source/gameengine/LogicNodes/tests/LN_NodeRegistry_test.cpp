/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <array>
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
using blender::nodes::logic::AutonomousPhase;
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
    case PinType::Angle:
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
    case RequiredPhase::Update:
      return LN_RequiredPhase::Update;
    case RequiredPhase::FixedUpdate:
      return LN_RequiredPhase::FixedUpdate;
  }
  return LN_RequiredPhase::None;
}

LN_AutonomousPhase AutonomousPhaseForMetadata(const AutonomousPhase phase)
{
  switch (phase) {
    case AutonomousPhase::None:
      return LN_AutonomousPhase::None;
    case AutonomousPhase::Update:
      return LN_AutonomousPhase::Update;
    case AutonomousPhase::FixedUpdate:
      return LN_AutonomousPhase::FixedUpdate;
  }
  return LN_AutonomousPhase::None;
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
    const LN_NodeDefinition &definition, const LN_TreeCompiler::CompileHandlerInfo &handler_info)
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
    const char *expected_socket_idname = blender::nodes::logic::pin_type_socket_idname(
        pin_metadata.type);
    EXPECT_EQ(pin_definition->socket_idname, expected_socket_idname ? expected_socket_idname : "")
        << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->requires_link, pin_metadata.requires_link)
        << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->allows_multiple_links, pin_metadata.allows_multiple_links)
        << node_idname << "." << pin_metadata.identifier;
    EXPECT_EQ(pin_definition->is_variadic, pin_metadata.is_variadic)
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
  const blender::Span<NodeMetadata> metadata_items = blender::nodes::logic::logic_node_metadata();

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
    EXPECT_EQ(definition->autonomous_phase,
              AutonomousPhaseForMetadata(metadata.autonomous_phase))
        << metadata.idname;

    ExpectPinsMatch(definition->inputs, metadata.inputs, metadata.idname);
    ExpectPinsMatch(definition->outputs, metadata.outputs, metadata.idname);
  }
}

TEST(LN_NodeRegistry, SetSaveExistenceHasExactPublicContract)
{
  const LN_NodeDefinition *definition = LN_NodeRegistry::GetBuiltin().FindNodeDefinition(
      "LogicNativeSetSaveExistence");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->ui_name, "Set Save Object State");
  EXPECT_EQ(definition->category, "Game");
  EXPECT_EQ(definition->execution_class, LN_ExecutionClass::CommandEmitting);
  EXPECT_TRUE(definition->has_side_effects);
  ASSERT_EQ(definition->inputs.size(), 3u);
  ASSERT_EQ(definition->outputs.size(), 2u);

  const LN_PinDefinition *flow = FindPin(definition->inputs, "Flow");
  const LN_PinDefinition *object = FindPin(definition->inputs, "Object");
  const LN_PinDefinition *enabled = FindPin(definition->inputs, "Enabled");
  const LN_PinDefinition *done = FindPin(definition->outputs, "Done");
  const LN_PinDefinition *success = FindPin(definition->outputs, "Success");
  ASSERT_NE(flow, nullptr);
  ASSERT_NE(object, nullptr);
  ASSERT_NE(enabled, nullptr);
  ASSERT_NE(done, nullptr);
  ASSERT_NE(success, nullptr);
  EXPECT_EQ(flow->kind, LN_PinKind::Execution);
  EXPECT_TRUE(flow->requires_link);
  EXPECT_EQ(object->value_type, LN_ValueType::ObjectRef);
  EXPECT_EQ(enabled->value_type, LN_ValueType::Bool);
  EXPECT_EQ(done->kind, LN_PinKind::Execution);
  EXPECT_EQ(success->value_type, LN_ValueType::Bool);
}

TEST(LN_NodeRegistry, WaitForNextFrameHasExactPublicContract)
{
  const LN_NodeDefinition *definition = LN_NodeRegistry::GetBuiltin().FindNodeDefinition(
      "LogicNativeOnNextFrame");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->ui_name, "Wait for Next Frame");
  EXPECT_EQ(definition->category, "Time");
  EXPECT_NE(definition->ui_description.find("next Frame Update"), std::string::npos);
  EXPECT_NE(definition->ui_description.find("merge into one"), std::string::npos);
  EXPECT_NE(definition->ui_description.find("does not wait for a Logic + Physics step"),
            std::string::npos);
  EXPECT_EQ(definition->required_phase, LN_RequiredPhase::None);
  EXPECT_EQ(definition->autonomous_phase, LN_AutonomousPhase::Update);
  ASSERT_EQ(definition->inputs.size(), 1u);
  ASSERT_EQ(definition->outputs.size(), 1u);

  const LN_PinDefinition *flow = FindPin(definition->inputs, "Flow");
  const LN_PinDefinition *completed = FindPin(definition->outputs, "Completed");
  ASSERT_NE(flow, nullptr);
  ASSERT_NE(completed, nullptr);
  EXPECT_EQ(flow->kind, LN_PinKind::Execution);
  EXPECT_EQ(completed->kind, LN_PinKind::Execution);
}

TEST(LN_NodeRegistry, ObjectAttributeNodesHaveExactNamesAndCategories)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *get_attribute = registry.FindNodeDefinition(
      "LogicNativeGetObjectAttribute");
  const LN_NodeDefinition *set_attribute = registry.FindNodeDefinition(
      "LogicNativeSetObjectAttribute");
  const LN_NodeDefinition *get_id = registry.FindNodeDefinition("LogicNativeGetObjectID");
  const LN_NodeDefinition *get_axis = registry.FindNodeDefinition("LogicNativeGetAxisVector");
  ASSERT_NE(get_attribute, nullptr);
  ASSERT_NE(set_attribute, nullptr);
  ASSERT_NE(get_id, nullptr);
  ASSERT_NE(get_axis, nullptr);

  EXPECT_EQ(get_attribute->ui_name, "Get Object Attribute");
  EXPECT_EQ(get_attribute->category, "Objects");
  EXPECT_EQ(set_attribute->ui_name, "Set Object Attribute");
  EXPECT_EQ(set_attribute->category, "Objects");
  EXPECT_EQ(get_id->ui_name, "Get Object ID");
  EXPECT_EQ(get_id->category, "Objects/Object Data");
  EXPECT_EQ(get_axis->ui_name, "Get Axis Vector");
  EXPECT_EQ(get_axis->category, "Objects/Object Data");
}

TEST(LN_NodeRegistry, ExecutionFlowNodesHaveExactPublicContracts)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *gate = registry.FindNodeDefinition("LogicNativeFlowGate");
  const LN_NodeDefinition *any_flow = registry.FindNodeDefinition("LogicNativeAnyFlow");
  const LN_NodeDefinition *sequence = registry.FindNodeDefinition("LogicNativeSequence");
  ASSERT_NE(gate, nullptr);
  ASSERT_NE(any_flow, nullptr);
  ASSERT_NE(sequence, nullptr);

  EXPECT_EQ(gate->category, "Flow");
  ASSERT_EQ(gate->inputs.size(), 5u);
  ASSERT_EQ(gate->outputs.size(), 2u);
  for (const char *name : {"Enter", "Toggle", "Open", "Close"}) {
    const LN_PinDefinition *pin = FindPin(gate->inputs, name);
    ASSERT_NE(pin, nullptr) << name;
    EXPECT_EQ(pin->kind, LN_PinKind::Execution) << name;
  }
  const LN_PinDefinition *start_open = FindPin(gate->inputs, "Start Open");
  const LN_PinDefinition *exit = FindPin(gate->outputs, "Exit");
  const LN_PinDefinition *is_open = FindPin(gate->outputs, "Is Open");
  ASSERT_NE(start_open, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(is_open, nullptr);
  EXPECT_EQ(start_open->value_type, LN_ValueType::Bool);
  EXPECT_EQ(exit->kind, LN_PinKind::Execution);
  EXPECT_EQ(is_open->value_type, LN_ValueType::Bool);

  EXPECT_EQ(any_flow->ui_name, "Any Flow");
  ASSERT_EQ(any_flow->inputs.size(), 1u);
  ASSERT_EQ(any_flow->outputs.size(), 1u);
  const LN_PinDefinition *any_in = FindPin(any_flow->inputs, "In");
  ASSERT_NE(any_in, nullptr);
  EXPECT_EQ(any_in->kind, LN_PinKind::Execution);
  EXPECT_TRUE(any_in->allows_multiple_links);
  ASSERT_NE(FindPin(any_flow->outputs, "Out"), nullptr);
  EXPECT_EQ(FindPin(any_flow->outputs, "Out")->kind, LN_PinKind::Execution);

  ASSERT_EQ(sequence->inputs.size(), 1u);
  ASSERT_EQ(sequence->outputs.size(), 1u);
  ASSERT_NE(FindPin(sequence->inputs, "Flow"), nullptr);
  EXPECT_EQ(FindPin(sequence->inputs, "Flow")->kind, LN_PinKind::Execution);
  EXPECT_TRUE(FindPin(sequence->inputs, "Flow")->requires_link);
  const LN_PinDefinition *then_pin = FindPin(sequence->outputs, "Then ");
  ASSERT_NE(then_pin, nullptr);
  EXPECT_EQ(then_pin->kind, LN_PinKind::Execution);
  EXPECT_TRUE(then_pin->is_variadic);
}

TEST(LN_NodeRegistry, BooleanCombineAndInputStatesRemainExplicit)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *boolean_combine = registry.FindNodeDefinition("LogicNativeGate");
  const LN_NodeDefinition *gamepad_button = registry.FindNodeDefinition(
      "LogicNativeGamepadButton");
  ASSERT_NE(boolean_combine, nullptr);
  ASSERT_NE(gamepad_button, nullptr);
  EXPECT_EQ(boolean_combine->ui_name, "Boolean Combine");
  ASSERT_EQ(boolean_combine->inputs.size(), 1u);
  const LN_PinDefinition *conditions = FindPin(boolean_combine->inputs, "Conditions");
  ASSERT_NE(conditions, nullptr);
  EXPECT_EQ(conditions->kind, LN_PinKind::Condition);
  EXPECT_TRUE(conditions->allows_multiple_links);

  const LN_PinDefinition *triggered = FindPin(gamepad_button->outputs, "Out");
  const LN_PinDefinition *active = FindPin(gamepad_button->outputs, "Active");
  ASSERT_NE(triggered, nullptr);
  ASSERT_NE(active, nullptr);
  EXPECT_EQ(triggered->kind, LN_PinKind::Execution);
  EXPECT_EQ(active->kind, LN_PinKind::Condition);
  EXPECT_EQ(gamepad_button->required_phase, LN_RequiredPhase::None);
  EXPECT_EQ(gamepad_button->autonomous_phase, LN_AutonomousPhase::FixedUpdate);
}

TEST(LN_NodeRegistry, WhilePausedDefinesFramePolicyWithoutChangingFixedSimulation)
{
  const LN_NodeDefinition *definition =
      LN_NodeRegistry::GetBuiltin().FindNodeDefinition("LogicNativeWhilePaused");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->required_phase, LN_RequiredPhase::Update);
  EXPECT_EQ(definition->autonomous_phase, LN_AutonomousPhase::None);
  EXPECT_EQ(definition->execution_class, LN_ExecutionClass::SnapshotReadOnly);
  ASSERT_NE(FindPin(definition->inputs, "Object"), nullptr);
  ASSERT_NE(FindPin(definition->inputs, "Collection"), nullptr);
  ASSERT_NE(FindPin(definition->outputs, "Update"), nullptr);
  const LN_PinDefinition *delta_time = FindPin(definition->outputs, "Delta Time");
  ASSERT_NE(delta_time, nullptr);
  EXPECT_EQ(delta_time->value_type, LN_ValueType::Float);
}

TEST(LN_NodeRegistry, ObjectUIFamilyHasCompleteRuntimeContracts)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *ui_event = registry.FindNodeDefinition("LogicNativeOnUIEvent");
  const LN_NodeDefinition *scroll_view = registry.FindNodeDefinition(
      "LogicNativeObjectScrollView");
  const LN_NodeDefinition *grid_view = registry.FindNodeDefinition(
      "LogicNativeObjectGridView");
  const LN_NodeDefinition *open_menu = registry.FindNodeDefinition(
      "LogicNativeOpenObjectMenu");
  const LN_NodeDefinition *close_menu = registry.FindNodeDefinition(
      "LogicNativeCloseObjectMenu");
  const LN_NodeDefinition *set_focus = registry.FindNodeDefinition("LogicNativeSetUIFocus");
  const LN_NodeDefinition *take_control = registry.FindNodeDefinition("LogicNativeTakeUIControl");
  const LN_NodeDefinition *set_pointer = registry.FindNodeDefinition(
      "LogicNativeSetPlayerPointer");
  const LN_NodeDefinition *get_focus = registry.FindNodeDefinition("LogicNativeGetUIFocus");
  const LN_NodeDefinition *screen_anchor = registry.FindNodeDefinition(
      "LogicNativeScreenAnchor");
  ASSERT_NE(ui_event, nullptr);
  ASSERT_NE(scroll_view, nullptr);
  ASSERT_NE(grid_view, nullptr);
  ASSERT_NE(open_menu, nullptr);
  ASSERT_NE(close_menu, nullptr);
  ASSERT_NE(set_focus, nullptr);
  ASSERT_NE(take_control, nullptr);
  ASSERT_NE(set_pointer, nullptr);
  ASSERT_NE(get_focus, nullptr);
  ASSERT_NE(screen_anchor, nullptr);

  EXPECT_EQ(ui_event->execution_class, LN_ExecutionClass::MainThreadOnly);
  EXPECT_EQ(ui_event->autonomous_phase, LN_AutonomousPhase::Update);
  for (const char *output : {"Object", "Target", "Point", "Normal", "Item", "Outcome"}) {
    EXPECT_NE(FindPin(ui_event->outputs, output), nullptr) << output;
  }

  for (const LN_NodeDefinition *view : {scroll_view, grid_view}) {
    EXPECT_EQ(view->execution_class, LN_ExecutionClass::MainThreadOnly) << view->idname;
    EXPECT_EQ(view->autonomous_phase, LN_AutonomousPhase::Update) << view->idname;
  }
  EXPECT_NE(FindPin(scroll_view->inputs, "Viewport Object"), nullptr);
  EXPECT_NE(FindPin(scroll_view->inputs, "Content Object"), nullptr);
  EXPECT_NE(FindPin(grid_view->inputs, "Slots"), nullptr);
  EXPECT_NE(FindPin(grid_view->inputs, "Items"), nullptr);

  for (const LN_NodeDefinition *command :
       {open_menu, close_menu, set_focus, screen_anchor, take_control, set_pointer})
  {
    EXPECT_EQ(command->execution_class, LN_ExecutionClass::CommandEmitting)
        << command->idname;
    EXPECT_EQ(command->required_phase, LN_RequiredPhase::None) << command->idname;
  }
  EXPECT_NE(FindPin(open_menu->outputs, "Opened"), nullptr);
  EXPECT_NE(FindPin(open_menu->outputs, "Failed"), nullptr);
  EXPECT_NE(FindPin(open_menu->outputs, "Menu"), nullptr);
  EXPECT_NE(FindPin(open_menu->outputs, "Result"), nullptr);
  EXPECT_NE(FindPin(close_menu->outputs, "Closed"), nullptr);
  EXPECT_NE(FindPin(set_focus->outputs, "Failed"), nullptr);
  EXPECT_NE(FindPin(take_control->outputs, "Failed"), nullptr);
  EXPECT_NE(FindPin(take_control->outputs, "Result"), nullptr);
  EXPECT_NE(FindPin(set_pointer->inputs, "Surface"), nullptr);
  EXPECT_NE(FindPin(set_pointer->inputs, "Move Action"), nullptr);
  EXPECT_EQ(get_focus->execution_class, LN_ExecutionClass::MainThreadOnly);
  EXPECT_NE(FindPin(get_focus->outputs, "Object"), nullptr);
}

TEST(LN_NodeRegistry, AutonomousEventCadenceIsExplicitAndIndependentFromConstraints)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  /* UI resolves once per rendered frame, so UI events also run while paused. */
  for (const char *idname : {"LogicNativeOnGameLoaded",
                             "LogicNativeOnPlayerJoin",
                             "LogicNativeOnNextFrame",
                             "LogicNativeOnUIEvent",
                             "LogicNativeOnUISound",
                             "LogicNativeObjectScrollView",
                             "LogicNativeObjectGridView"})
  {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_EQ(definition->autonomous_phase, LN_AutonomousPhase::Update) << idname;
  }

  for (const char *idname : {"LogicNativeBooleanEdge",
                             "LogicNativeValueChanged",
                             "LogicNativeValueChangedTo",
                             "LogicNativeReceiveEvent",
                             "LogicNativeOnCollision",
                             "LogicNativeOnConstraintBreak",
                             "LogicNativeTweenValue",
                             "LogicNativeKeyboardKey",
                             "LogicNativeMouseButton",
                             "LogicNativeMouseOver",
                             "LogicNativeGamepadButton",
                             "LogicNativeMouseMoved",
                             "LogicNativeMouseWheel",
                             "LogicNativeInputAction"})
  {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_EQ(definition->autonomous_phase, LN_AutonomousPhase::FixedUpdate) << idname;
  }

  size_t update_count = 0;
  size_t fixed_count = 0;
  for (const LN_NodeDefinition &definition : registry.GetNodeDefinitions()) {
    update_count += definition.autonomous_phase == LN_AutonomousPhase::Update ? 1u : 0u;
    fixed_count += definition.autonomous_phase == LN_AutonomousPhase::FixedUpdate ? 1u : 0u;
  }
  EXPECT_EQ(update_count, 7u);
  EXPECT_EQ(fixed_count, 14u);
}

TEST(LN_NodeRegistry, SimulationMutationsRequireFixedLogicPhysicsFlow)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const char *idname : {"LogicNativeSetGravity",
                             "LogicNativeSetCollisionGroup",
                             "LogicNativeSetPhysics",
                             "LogicNativeSetDynamics",
                             "LogicNativeRebuildCollisionShape",
                             "LogicNativeSetRigidBodyAttribute",
                             "LogicNativeCharacterJump",
                             "LogicNativeSetCharacterGravity",
                             "LogicNativeSetCharacterJumpSpeed",
                             "LogicNativeSetCharacterMaxJumps",
                             "LogicNativeSetCharacterWalkDirection",
                             "LogicNativeSetCharacterVelocity",
                             "LogicNativeVehicleControl",
                             "LogicNativeVehicleAccelerate",
                             "LogicNativeVehicleBrake",
                             "LogicNativeVehicleSteer",
                             "LogicNativeVehicleSetAttributes",
                             "LogicNativeApplyImpulse",
                             "LogicNativeApplyForce",
                             "LogicNativeApplyForceToTarget",
                             "LogicNativeApplyTorque",
                             "LogicNativeMoveToward",
                             "LogicNativeNavigate",
                             "LogicNativeFollowPath",
                             "LogicNativeTranslate",
                             "LogicNativeSlowFollow",
                             "LogicNativeAlignAxisToVector",
                             "LogicNativeRotateToward",
                             "LogicNativeAddPhysicsConstraint",
                             "LogicNativeRemovePhysicsConstraint"})
  {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_TRUE(definition->has_side_effects) << idname;
    EXPECT_EQ(definition->required_phase, LN_RequiredPhase::FixedUpdate) << idname;
  }

  /* Pause control must remain callable while fixed simulation is intentionally stopped. */
  const LN_NodeDefinition *physics_pause = registry.FindNodeDefinition(
      "LogicNativeSetPhysicsPaused");
  ASSERT_NE(physics_pause, nullptr);
  EXPECT_EQ(physics_pause->required_phase, LN_RequiredPhase::None);
}

TEST(LN_NodeRegistry, RelativeVisualTransformsInheritTheirExecutionPhase)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const char *idname : {"LogicNativeApplyMovement", "LogicNativeApplyRotation"}) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_TRUE(definition->has_side_effects) << idname;
    EXPECT_EQ(definition->required_phase, LN_RequiredPhase::None) << idname;
  }
}

TEST(LN_NodeRegistry, AllNativeNodePhasesMatchAuditedExecutionModel)
{
  const std::unordered_set<std::string> expected_fixed = {
      "LogicNativeAddObject",
      "LogicNativeAddPhysicsConstraint",
      "LogicNativeAlignAxisToVector",
      "LogicNativeApplyForce",
      "LogicNativeApplyForceToTarget",
      "LogicNativeApplyImpulse",
      "LogicNativeApplyTorque",
      "LogicNativeCharacterJump",
      "LogicNativeFollowPath",
      "LogicNativeGamepadLook",
      "LogicNativeMouseLook",
      "LogicNativeMoveToward",
      "LogicNativeNavigate",
      "LogicNativeOnCollision",
      "LogicNativeOnConstraintBreak",
      "LogicNativeOnFixedUpdate",
      "LogicNativeProjectileRay",
      "LogicNativeRebuildCollisionShape",
      "LogicNativeRemoveObject",
      "LogicNativeRemoveParent",
      "LogicNativeRemovePhysicsConstraint",
      "LogicNativeRotateToward",
      "LogicNativeSetCharacterGravity",
      "LogicNativeSetCharacterJumpSpeed",
      "LogicNativeSetCharacterMaxJumps",
      "LogicNativeSetCharacterVelocity",
      "LogicNativeSetCharacterWalkDirection",
      "LogicNativeSetCollisionGroup",
      "LogicNativeSetDynamics",
      "LogicNativeSetGravity",
      "LogicNativeSetParent",
      "LogicNativeSetPhysics",
      "LogicNativeSetRigidBodyAttribute",
      "LogicNativeSlowFollow",
      "LogicNativeTranslate",
      "LogicNativeVehicleAccelerate",
      "LogicNativeVehicleBrake",
      "LogicNativeVehicleControl",
      "LogicNativeVehicleSetAttributes",
      "LogicNativeVehicleSteer",
  };
  const std::unordered_set<std::string> expected_update = {"LogicNativeOnGameLoaded",
                                                           "LogicNativeOnPlayerJoin",
                                                           "LogicNativeOnUpdate",
                                                           "LogicNativeWhilePaused"};
  const std::unordered_set<std::string> expected_on_init = {"LogicNativeOnInit"};

  std::unordered_set<std::string> actual_fixed;
  std::unordered_set<std::string> actual_update;
  std::unordered_set<std::string> actual_on_init;
  for (const LN_NodeDefinition &definition :
       LN_NodeRegistry::GetBuiltin().GetNodeDefinitions())
  {
    switch (definition.required_phase) {
      case LN_RequiredPhase::FixedUpdate:
        actual_fixed.insert(definition.idname);
        break;
      case LN_RequiredPhase::Update:
        actual_update.insert(definition.idname);
        break;
      case LN_RequiredPhase::OnInit:
        actual_on_init.insert(definition.idname);
        break;
      case LN_RequiredPhase::None:
        break;
    }
  }

  EXPECT_EQ(actual_fixed, expected_fixed);
  EXPECT_EQ(actual_update, expected_update);
  EXPECT_EQ(actual_on_init, expected_on_init);
}

TEST(LN_NodeRegistry, MixValueHasExactGenericInterpolationContract)
{
  const LN_NodeDefinition *definition = LN_NodeRegistry::GetBuiltin().FindNodeDefinition(
      "LogicNativeMixValue");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->kind, LN_NodeKind::MixValue);
  EXPECT_EQ(definition->ui_name, "Mix Value");
  EXPECT_EQ(definition->category, "Values");
  EXPECT_EQ(definition->execution_class, LN_ExecutionClass::SnapshotReadOnly);
  EXPECT_FALSE(definition->has_side_effects);
  ASSERT_EQ(definition->inputs.size(), 3u);
  ASSERT_EQ(definition->outputs.size(), 1u);

  const LN_PinDefinition *a = FindPin(definition->inputs, "A");
  const LN_PinDefinition *b = FindPin(definition->inputs, "B");
  const LN_PinDefinition *factor = FindPin(definition->inputs, "Factor");
  const LN_PinDefinition *result = FindPin(definition->outputs, "Result");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(factor, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(a->value_type, LN_ValueType::Generic);
  EXPECT_EQ(b->value_type, LN_ValueType::Generic);
  EXPECT_TRUE(a->requires_link);
  EXPECT_TRUE(b->requires_link);
  EXPECT_EQ(factor->value_type, LN_ValueType::Float);
  EXPECT_FALSE(factor->requires_link);
  EXPECT_EQ(result->value_type, LN_ValueType::Generic);
}

TEST(LN_NodeRegistry, RotationComponentNodesHaveExactPublicContracts)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *combine = registry.FindNodeDefinition("LogicNativeEuler");
  const LN_NodeDefinition *separate = registry.FindNodeDefinition("LogicNativeSeparateEuler");
  ASSERT_NE(combine, nullptr);
  ASSERT_NE(separate, nullptr);

  EXPECT_EQ(combine->kind, LN_NodeKind::Euler);
  EXPECT_EQ(combine->ui_name, "Combine Rotation");
  EXPECT_EQ(combine->category, "Values/Rotation");
  EXPECT_EQ(combine->execution_class, LN_ExecutionClass::SnapshotReadOnly);
  EXPECT_FALSE(combine->has_side_effects);
  ASSERT_EQ(combine->inputs.size(), 3u);
  ASSERT_EQ(combine->outputs.size(), 1u);
  for (const char *name : {"X", "Y", "Z"}) {
    const LN_PinDefinition *angle = FindPin(combine->inputs, name);
    ASSERT_NE(angle, nullptr) << name;
    EXPECT_EQ(angle->value_type, LN_ValueType::Float) << name;
    EXPECT_EQ(angle->socket_idname, "NodeSocketFloatAngle") << name;
  }
  const LN_PinDefinition *combined_rotation = FindPin(combine->outputs, "Rotation");
  ASSERT_NE(combined_rotation, nullptr);
  EXPECT_EQ(combined_rotation->value_type, LN_ValueType::Rotation);
  EXPECT_EQ(combined_rotation->socket_idname, "NodeSocketLogicRotation");

  EXPECT_EQ(separate->kind, LN_NodeKind::SeparateEuler);
  EXPECT_EQ(separate->ui_name, "Separate Rotation");
  EXPECT_EQ(separate->category, "Values/Rotation");
  EXPECT_EQ(separate->execution_class, LN_ExecutionClass::SnapshotReadOnly);
  EXPECT_FALSE(separate->has_side_effects);
  ASSERT_EQ(separate->inputs.size(), 1u);
  ASSERT_EQ(separate->outputs.size(), 3u);
  const LN_PinDefinition *source_rotation = FindPin(separate->inputs, "Rotation");
  ASSERT_NE(source_rotation, nullptr);
  EXPECT_EQ(source_rotation->value_type, LN_ValueType::Rotation);
  for (const char *name : {"X", "Y", "Z"}) {
    const LN_PinDefinition *angle = FindPin(separate->outputs, name);
    ASSERT_NE(angle, nullptr) << name;
    EXPECT_EQ(angle->value_type, LN_ValueType::Float) << name;
    EXPECT_EQ(angle->socket_idname, "NodeSocketFloatAngle") << name;
  }

  const LN_NodeDefinition *vector_to_rotation = registry.FindNodeDefinition(
      "LogicNativeVectorToRotation");
  const LN_NodeDefinition *bone_to_object = registry.FindNodeDefinition(
      "LogicNativeBoneRotationToObjectRotation");
  ASSERT_NE(vector_to_rotation, nullptr);
  ASSERT_NE(bone_to_object, nullptr);
  EXPECT_EQ(vector_to_rotation->category, "Values/Rotation");
  EXPECT_EQ(bone_to_object->category, "Values/Rotation");
}

TEST(LN_NodeRegistry, FloatCurveHasExactPureMappingContract)
{
  const LN_NodeDefinition *definition = LN_NodeRegistry::GetBuiltin().FindNodeDefinition(
      "LogicNativeFloatCurve");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->kind, LN_NodeKind::FloatCurve);
  EXPECT_EQ(definition->ui_name, "Float Curve");
  EXPECT_EQ(definition->category, "Math");
  EXPECT_EQ(definition->execution_class, LN_ExecutionClass::SnapshotReadOnly);
  EXPECT_FALSE(definition->has_side_effects);
  ASSERT_EQ(definition->inputs.size(), 2u);
  ASSERT_EQ(definition->outputs.size(), 1u);

  const LN_PinDefinition *factor = FindPin(definition->inputs, "Factor");
  const LN_PinDefinition *value = FindPin(definition->inputs, "Value");
  const LN_PinDefinition *result = FindPin(definition->outputs, "Value");
  ASSERT_NE(factor, nullptr);
  ASSERT_NE(value, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(factor->value_type, LN_ValueType::Float);
  EXPECT_EQ(value->value_type, LN_ValueType::Float);
  EXPECT_EQ(result->value_type, LN_ValueType::Float);
  EXPECT_FALSE(factor->requires_link);
  EXPECT_FALSE(value->requires_link);
  EXPECT_FALSE(result->requires_link);
}

TEST(LN_NodeRegistry, RaycastFromMeshHasExactBatchQueryContract)
{
  const LN_NodeDefinition *definition = LN_NodeRegistry::GetBuiltin().FindNodeDefinition(
      "LogicNativeRaycastFromMesh");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->kind, LN_NodeKind::RaycastFromMesh);
  EXPECT_EQ(definition->ui_name, "Raycast from Mesh");
  EXPECT_EQ(definition->category, "Physics");
  EXPECT_EQ(definition->execution_class, LN_ExecutionClass::MainThreadOnly);
  EXPECT_TRUE(definition->requires_jolt);
  EXPECT_FALSE(definition->has_side_effects);

  const std::array<std::pair<const char *, LN_ValueType>, 13> expected_inputs = {{
      {"Flow", LN_ValueType::None},
      {"Source Object", LN_ValueType::ObjectRef},
      {"Ignore Object", LN_ValueType::ObjectRef},
      {"Ignore Objects", LN_ValueType::List},
      {"Start Offset", LN_ValueType::Float},
      {"Max Distance", LN_ValueType::Float},
      {"Max Rays", LN_ValueType::Int},
      {"Property", LN_ValueType::String},
      {"X-Ray", LN_ValueType::Bool},
      {"Mask", LN_ValueType::Int},
      {"Include Sensors", LN_ValueType::Bool},
      {"Hit Backfaces", LN_ValueType::Bool},
      {"Visualize", LN_ValueType::Bool},
  }};
  const std::array<std::pair<const char *, LN_ValueType>, 18> expected_outputs = {{
      {"Done", LN_ValueType::None},
      {"Any Hit", LN_ValueType::Bool},
      {"All Hit", LN_ValueType::Bool},
      {"Hit Count", LN_ValueType::Int},
      {"Ray Count", LN_ValueType::Int},
      {"Hits", LN_ValueType::List},
      {"Blocked", LN_ValueType::List},
      {"Hit Objects", LN_ValueType::List},
      {"Points", LN_ValueType::List},
      {"Normals", LN_ValueType::List},
      {"Distances", LN_ValueType::List},
      {"Fractions", LN_ValueType::List},
      {"Face Indices", LN_ValueType::List},
      {"Has UVs", LN_ValueType::List},
      {"UVs", LN_ValueType::List},
      {"Ray Origins", LN_ValueType::List},
      {"Ray Directions", LN_ValueType::List},
      {"End Points", LN_ValueType::List},
  }};
  ASSERT_EQ(definition->inputs.size(), expected_inputs.size());
  ASSERT_EQ(definition->outputs.size(), expected_outputs.size());
  for (const auto &[name, type] : expected_inputs) {
    const LN_PinDefinition *pin = FindPin(definition->inputs, name);
    ASSERT_NE(pin, nullptr) << name;
    EXPECT_EQ(pin->value_type, type) << name;
  }
  for (const auto &[name, type] : expected_outputs) {
    const LN_PinDefinition *pin = FindPin(definition->outputs, name);
    ASSERT_NE(pin, nullptr) << name;
    EXPECT_EQ(pin->value_type, type) << name;
  }
}

TEST(LN_NodeRegistry, PhysicsCastNodesExposeUnifiedIgnoreListContract)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  for (const char *idname : {"LogicNativeRaycast",
                             "LogicNativeRaycastAll",
                             "LogicNativeRaycastFromMesh",
                             "LogicNativeShapeCast",
                             "LogicNativeShapeCastAll"})
  {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    const LN_PinDefinition *single = FindPin(definition->inputs, "Ignore Object");
    const LN_PinDefinition *multiple = FindPin(definition->inputs, "Ignore Objects");
    ASSERT_NE(single, nullptr) << idname;
    ASSERT_NE(multiple, nullptr) << idname;
    EXPECT_EQ(single->value_type, LN_ValueType::ObjectRef) << idname;
    EXPECT_EQ(multiple->value_type, LN_ValueType::List) << idname;
    EXPECT_FALSE(single->requires_link) << idname;
    EXPECT_FALSE(multiple->requires_link) << idname;
  }

  const LN_NodeDefinition *raycast = registry.FindNodeDefinition("LogicNativeRaycast");
  ASSERT_NE(raycast, nullptr);
  const LN_PinDefinition *hit_point = FindPin(raycast->outputs, "Point");
  const LN_PinDefinition *hit_normal = FindPin(raycast->outputs, "Normal");
  const LN_PinDefinition *end_point = FindPin(raycast->outputs, "End Point");
  ASSERT_NE(hit_point, nullptr);
  ASSERT_NE(hit_normal, nullptr);
  ASSERT_NE(end_point, nullptr);
  EXPECT_EQ(hit_point->value_type, LN_ValueType::Vector);
  EXPECT_EQ(hit_normal->value_type, LN_ValueType::Vector);
  EXPECT_EQ(end_point->value_type, LN_ValueType::Vector);
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
    EXPECT_EQ(contract.autonomous_phase, definition->autonomous_phase) << contract.idname;

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
    if (contract.idname == "LogicNativeSetObjectAttribute") {
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
    if (contract.idname == "LogicNativeSetObjectAttribute") {
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
    if (contract.idname == "LogicNativeSetObjectAttribute") {
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
  };

  for (const char *idname : unsupported_runtime_idnames) {
    const LN_NodeMetadataContract *contract = LN_FindNodeMetadataContract(idname);
    ASSERT_NE(contract, nullptr) << idname;
    EXPECT_FALSE(contract->compile_handler_info.has_runtime_implementation) << idname;
    EXPECT_EQ(contract->visibility, LN_NodeVisibility::Hidden) << idname;
    EXPECT_EQ(contract->compile_handler_info.kind, LN_TreeCompiler::CompileHandlerKind::Command)
        << idname;
  }

  const LN_NodeMetadataContract *set_attribute = LN_FindNodeMetadataContract(
      "LogicNativeSetObjectAttribute");
  ASSERT_NE(set_attribute, nullptr);
  EXPECT_TRUE(set_attribute->compile_handler_info.has_runtime_implementation);
  EXPECT_EQ(set_attribute->visibility, LN_NodeVisibility::Release);
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

  const LN_NodeDefinition *get_attribute = registry.FindNodeDefinition(
      "LogicNativeGetObjectAttribute");
  ASSERT_NE(get_attribute, nullptr);
  EXPECT_FALSE(get_attribute->has_side_effects);
  EXPECT_EQ(get_attribute->execution_class, LN_ExecutionClass::MainThreadOnly);

  const LN_NodeDefinition *set_attribute = registry.FindNodeDefinition(
      "LogicNativeSetObjectAttribute");
  ASSERT_NE(set_attribute, nullptr);
  EXPECT_TRUE(set_attribute->has_side_effects);
  EXPECT_EQ(set_attribute->execution_class, LN_ExecutionClass::CommandEmitting);
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

  const LN_NodeDefinition *set_attribute = registry.FindNodeDefinition(
      "LogicNativeSetObjectAttribute");
  ASSERT_NE(set_attribute, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileHandlerInfo(set_attribute->kind).kind,
            LN_TreeCompiler::CompileHandlerKind::Command);
}

TEST(LN_NodeRegistry, ReceiveEventExposesConditionAndExecutionOutputs)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  const LN_NodeDefinition *receive_event = registry.FindNodeDefinition("LogicNativeReceiveEvent");
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
  EXPECT_EQ(receive_event->required_phase, LN_RequiredPhase::None);
  EXPECT_EQ(receive_event->autonomous_phase, LN_AutonomousPhase::FixedUpdate);
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
  EXPECT_TRUE(
      HasErrorContaining(errors, "only command handlers may be marked unsupported at runtime"));
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

  const LN_NodeDefinition *set_attribute = registry.FindNodeDefinition(
      "LogicNativeSetObjectAttribute");
  ASSERT_NE(set_attribute, nullptr);
  EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(set_attribute->kind),
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

TEST(LN_NodeRegistry, ObjectAttributeAndGravityNodesUseCustomCompileDispatch)
{
  const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin();
  static const char *object_attribute_idnames[] = {
      "LogicNativeGetObjectAttribute",
      "LogicNativeSetObjectAttribute",
      "LogicNativeGetGravity",
  };

  for (const char *idname : object_attribute_idnames) {
    const LN_NodeDefinition *definition = registry.FindNodeDefinition(idname);
    ASSERT_NE(definition, nullptr) << idname;
    EXPECT_EQ(LN_TreeCompiler::GetCompileDispatch(definition->kind),
              LN_TreeCompiler::CompileDispatch::CustomCompile)
        << idname;
    EXPECT_NE(LN_TreeCompiler::FindCompileHandlerDescriptor(definition->kind), nullptr) << idname;
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
      "LogicNativeBoneRotationToObjectRotation",
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
    EXPECT_NE(LN_TreeCompiler::FindCompileHandlerDescriptor(definition->kind), nullptr) << idname;
    const LN_TreeCompiler::CompileDispatch dispatch = LN_TreeCompiler::GetCompileDispatch(
        definition->kind);
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
    const LN_TreeCompiler::CompileHandlerInfo info = LN_TreeCompiler::GetCompileHandlerInfo(
        definition.kind);
    if (info.kind != LN_TreeCompiler::CompileHandlerKind::Expression &&
        info.kind != LN_TreeCompiler::CompileHandlerKind::ValueExpression)
    {
      continue;
    }
    if (definition.has_side_effects) {
      continue;
    }
    const LN_TreeCompiler::CompileDispatch dispatch = LN_TreeCompiler::GetCompileDispatch(
        definition.kind);
    EXPECT_TRUE(dispatch == LN_TreeCompiler::CompileDispatch::CustomCompile ||
                dispatch == LN_TreeCompiler::CompileDispatch::ConstantOutput)
        << definition.idname;
  }
}
