/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler_expression_outputs_by_kind.cc
 *  \ingroup logicnodes
 *
 * Per-kind expression output lowering (registry expression nodes).
 */

#include "../LN_TreeCompiler_internal.hh"

#include <cmath>

#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "SCA_InputEvent.h"
#include "SCA_JoystickSensor.h"

#include "BL_Action.h"

namespace ln_compiler {

static const char *GetObjectAttributeName(const int attribute_type)
{
  switch (attribute_type) {
    case 0:
      return "worldPosition";
    case 1:
      return "localPosition";
    case 2:
      return "visible";
    case 3:
      return "name";
    case 4:
      return "localScale";
    case 5:
      return "worldScale";
    case 6:
      return "color";
    case 7:
      return "localOrientation";
    case 8:
      return "worldOrientation";
    case 9:
      return "worldLinearVelocity";
    case 10:
      return "localLinearVelocity";
    case 11:
      return "worldAngularVelocity";
    case 12:
      return "localAngularVelocity";
    default:
      return "worldPosition";
  }
}

static bool GetObjectAttributeUsesVectorOutput(const int attribute_type)
{
  return attribute_type == 0 || attribute_type == 1 || attribute_type == 4 ||
         attribute_type == 5 || attribute_type == 9 || attribute_type == 10 ||
         attribute_type == 11 || attribute_type == 12;
}

static bool GetObjectAttributeUsesOrientationOutput(const int attribute_type)
{
  return attribute_type == 7 || attribute_type == 8;
}

static bool GetObjectAttributeUsesTransformOutput(const int attribute_type)
{
  return attribute_type == 13 || attribute_type == 14;
}

static bool GetRigidBodyAttributeUsesScalarOutput(const int attribute_type)
{
  return attribute_type == 0 || attribute_type == 1 || attribute_type == 2 ||
         (attribute_type >= 4 && attribute_type <= 8);
}

static bool GetRigidBodyAttributeUsesDampingOutput(const int attribute_type)
{
  return attribute_type == 3;
}

static bool GetRigidBodyAttributeUsesEnabledOutput(const int attribute_type)
{
  return attribute_type == 9 || attribute_type == 12;
}

static bool GetRigidBodyAttributeUsesSleepingOutput(const int attribute_type)
{
  return attribute_type == 10;
}

static bool GetRigidBodyAttributeUsesAxisLocksOutput(const int attribute_type)
{
  return attribute_type == 11;
}

static const char *GetObjectTransformPositionAttributeName(const int attribute_type)
{
  return attribute_type == 14 ? "localPosition" : "worldPosition";
}

static const char *GetObjectTransformOrientationAttributeName(const int attribute_type)
{
  return attribute_type == 14 ? "localOrientation" : "worldOrientation";
}

static const char *GetObjectTransformScaleAttributeName(const int attribute_type)
{
  return attribute_type == 14 ? "localScale" : "worldScale";
}

static bool OutputSocketIsLinked(const blender::bNode &node,
                                 const InputLinkMap &input_links,
                                 const char *socket_name)
{
  const blender::bNodeSocket *socket = FindOutputSocket(node, socket_name);
  if (socket == nullptr) {
    return false;
  }

  for (const auto &item : input_links) {
    for (const ResolvedLink &link : item.second) {
      if (link.fromnode == &node && link.fromsock == socket) {
        return true;
      }
    }
  }
  return false;
}

static std::optional<uint32_t> BuildOptionalFlowCondition(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, "Flow");
  if (socket == nullptr) {
    return AddConstantBoolExpression(program, true);
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter == input_links.end() || links_iter->second.empty()) {
    return AddConstantBoolExpression(program, true);
  }

  return BuildInputExecutionExpression(program,
                                       node,
                                       "Flow",
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       bool_expression_cache);
}

static int CollisionOutputPayloadDetail(const blender::bNode &node,
                                        const InputLinkMap &input_links)
{
  if (OutputSocketIsLinked(node, input_links, "Contact Count") ||
      OutputSocketIsLinked(node, input_links, "Point") ||
      OutputSocketIsLinked(node, input_links, "Points") ||
      OutputSocketIsLinked(node, input_links, "Normal") ||
      OutputSocketIsLinked(node, input_links, "Normals"))
  {
    return 2;
  }
  if (OutputSocketIsLinked(node, input_links, "Collided Object") ||
      OutputSocketIsLinked(node, input_links, "Collided Objects"))
  {
    return 1;
  }
  return 0;
}

static std::optional<uint32_t> BuildGetObjectAttributeObjectExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    BoolExpressionCache &bool_expression_cache,
    IntExpressionCache &int_expression_cache,
    FloatExpressionCache &float_expression_cache,
    StringExpressionCache &string_expression_cache,
    VectorExpressionCache &vector_expression_cache,
    ColorExpressionCache &color_expression_cache,
    ValueExpressionCache &value_expression_cache)
{
  return BuildOptionalObjectTargetExpression(program,
                                             node,
                                             "Object",
                                             node_definitions,
                                             input_links,
                                             value_cache,
                                             bool_expression_cache,
                                             int_expression_cache,
                                             float_expression_cache,
                                             string_expression_cache,
                                             vector_expression_cache,
                                             color_expression_cache,
                                             value_expression_cache);
}

static std::optional<uint32_t> BuildGetObjectAttributeAttributeExpression(
    LN_Program &program,
    const blender::bNode &node)
{
  return AddConstantStringExpression(program, GetObjectAttributeName(node.custom1));
}

std::optional<uint32_t> TryCompileExpressionBoolOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = bool_expression_cache.find(&output_socket);
  if (cache_iter != bool_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
case LN_NodeKind::GetGamePropertyBool:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        LN_Value default_value;
        default_value.type = LN_ValueType::Bool;
        const uint32_t property_ref_index = AddGamePropertyRef(
            program, name->string_value, LN_ValueType::Bool, default_value);
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::StoreValue:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Done")) {
        break;
      }
      const blender::bNodeSocket *value_socket = FindOutputSocket(node, "Value");
      if (value_socket == nullptr) {
        break;
      }
      const std::optional<uint32_t> value_expr = BuildOutputFloatExpression(program,
                                                                            node,
                                                                            *value_socket,
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            float_expression_cache,
                                                                            &bool_expression_cache,
                                                                            nullptr);
      if (value_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::StoreValueDone;
        expression.float_expr_index = *value_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::GetCharacterInfo:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "On Ground")) {
        IntExpressionCache int_expression_cache_local;
        StringExpressionCache string_expression_cache_local;
        VectorExpressionCache vector_expression_cache_local;
        ColorExpressionCache color_expression_cache_local;
        ValueExpressionCache value_expression_cache_local;
        const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
            program,
            node,
            "Object",
            node_definitions,
            input_links,
            value_cache,
            bool_expression_cache,
            int_expression_cache_local,
            float_expression_cache,
            string_expression_cache_local,
            vector_expression_cache_local,
            color_expression_cache_local,
            value_expression_cache_local);
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::SnapshotCharacterOnGround;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::GetFullscreen:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Fullscreen")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::WindowFullscreen;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::HasProperty:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<LN_Value> name = ReadInputValue(node,
                                                          "Name",
                                                          LN_ValueType::String,
                                                          node_definitions,
                                                          input_links,
                                                          value_cache);
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        LN_Value default_value;
        const uint32_t property_ref_index = AddGamePropertyRef(
            program, name->string_value, LN_ValueType::None, default_value);
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::SnapshotGamePropertyExists;
        expression.property_ref_index = property_ref_index;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Bool, MakeDefaultValue(LN_ValueType::Bool));
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::IsNone:
      r_handled = true;
      case LN_NodeKind::NotNone:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> value_expr = BuildInputValueExpression(program,
                                                                           node,
                                                                           "Value",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           bool_expression_cache,
                                                                           int_expression_cache,
                                                                           float_expression_cache,
                                                                           string_expression_cache,
                                                                           vector_expression_cache,
                                                                           color_expression_cache,
                                                                           value_expression_cache);
      if (value_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ValueIsNone;
        expression.input0 = *value_expr;
        result = program.AddBoolExpression(expression);
        if (definition.kind == LN_NodeKind::NotNone) {
          result = AddNotBoolExpression(program, *result);
        }
      }
      break;
    }case LN_NodeKind::KeyboardKey:
      r_handled = true;
      {
      r_handled = true;
      const bool wants_pulse = NamesMatch(output_socket.name, output_socket.identifier, "Out") ||
                               NamesMatch(output_socket.name,
                                          output_socket.identifier,
                                          "If Pressed");
      const bool wants_active = NamesMatch(output_socket.name,
                                           output_socket.identifier,
                                           "Active");
      if (!wants_pulse && !wants_active) {
        break;
      }
      const std::optional<LN_Value> input_name = ReadInputValue(node,
                                                               "Key",
                                                               LN_ValueType::String,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      if (input_name && input_name->type == LN_ValueType::String &&
          !input_name->string_value.empty())
      {
        const std::string normalized_name = NormalizeInputEventName(input_name->string_value);
        const SCA_IInputDevice::SCA_EnumInputs input_code = InputCodeFromName(normalized_name);
        if (input_code == SCA_IInputDevice::NOKEY) {
          break;
        }
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::InputStatus;
        expression.int_value = int32_t(input_code);
        expression.secondary_int_value = wants_active ? SCA_InputEvent::ACTIVE :
                                                        MouseInputStatusFromCustom(node.custom1);
        expression.string_value = normalized_name;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::MouseButton:
      r_handled = true;
      {
      r_handled = true;
      const bool wants_pulse = NamesMatch(output_socket.name, output_socket.identifier, "Out") ||
                               NamesMatch(output_socket.name,
                                          output_socket.identifier,
                                          "If Pressed");
      const bool wants_active = NamesMatch(output_socket.name,
                                           output_socket.identifier,
                                           "Active");
      if (!wants_pulse && !wants_active) {
        break;
      }
      std::string button_name;
      if (IsInputSocketLinked(node, "Button", input_links)) {
        const std::optional<LN_Value> input_name = ReadInputValue(node,
                                                                  "Button",
                                                                  LN_ValueType::String,
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache);
        if (!input_name || input_name->type != LN_ValueType::String ||
            input_name->string_value.empty())
        {
          break;
        }
        button_name = NormalizeInputEventName(input_name->string_value);
      }
      else {
        button_name = MouseButtonNameFromCustom(node.custom2);
      }

      if (!button_name.empty()) {
        const SCA_IInputDevice::SCA_EnumInputs input_code = InputCodeFromName(button_name);
        if (input_code == SCA_IInputDevice::NOKEY) {
          break;
        }
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::InputStatus;
        expression.int_value = int32_t(input_code);
        expression.secondary_int_value = wants_active ? SCA_InputEvent::ACTIVE :
                                                        MouseInputStatusFromCustom(node.custom1);
        expression.string_value = button_name;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::KeyboardActive:
      r_handled = true;
      {
      r_handled = true;
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::KeyboardActive;
      result = program.AddBoolExpression(expression);
      break;
    }case LN_NodeKind::MouseMoved:
      r_handled = true;
      {
      r_handled = true;
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::MouseMoved;
      expression.bool_value = node.custom1 != 0;
      result = program.AddBoolExpression(expression);
      break;
    }case LN_NodeKind::MouseWheel:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "When Scrolled")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::MouseWheelMoved;
        expression.int_value = int32_t(node.custom1) + 1;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::KeyLogger:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Pressed")) {
        bool only_characters = true;
        const std::optional<LN_Value> only_chars = ReadInputValue(node,
                                                                  "Only Characters",
                                                                  LN_ValueType::Bool,
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache);
        if (only_chars && only_chars->type == LN_ValueType::Bool) {
          only_characters = only_chars->bool_value;
        }
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::KeyLoggerPressed;
        expression.bool_value = only_characters;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::MouseOver:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildMouseOverQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_BoolExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "On Enter") ||
          NamesMatch(output_socket.name, output_socket.identifier, "Entered"))
      {
        expression.kind = LN_BoolExpressionKind::MouseOverEnter;
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "On Over") ||
               NamesMatch(output_socket.name, output_socket.identifier, "Is Over"))
      {
        expression.kind = LN_BoolExpressionKind::MouseOverOver;
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "On Exit") ||
               NamesMatch(output_socket.name, output_socket.identifier, "Exited"))
      {
        expression.kind = LN_BoolExpressionKind::MouseOverExit;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::LogicTreeStatus:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache_local,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> tree_name_expr = BuildInputStringExpression(program,
                                                                                node,
                                                                                "Tree Name",
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                string_expression_cache_local);
      if (!tree_name_expr) {
        break;
      }

      LN_BoolExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Running")) {
        expression.kind = LN_BoolExpressionKind::LogicTreeRunning;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Stopped")) {
        expression.kind = LN_BoolExpressionKind::LogicTreeStopped;
      }
      else {
        break;
      }
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.input1 = *tree_name_expr;
      result = program.AddBoolExpression(expression);
      break;
    }case LN_NodeKind::ActionDone:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      const std::optional<uint32_t> layer_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Layer",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      if (!layer_expr) {
        break;
      }
      if (NamesMatch(output_socket.name, output_socket.identifier, "Done")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ActionDone;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.int_expr_index = *layer_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ObjectsColliding:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_a = BuildOptionalObjectTargetExpression(program,
                                                                                  node,
                                                                                  "Object A",
                                                                                  node_definitions,
                                                                                  input_links,
                                                                                  value_cache,
                                                                                  bool_expression_cache,
                                                                                  int_expression_cache,
                                                                                  float_expression_cache,
                                                                                  string_expression_cache,
                                                                                  vector_expression_cache,
                                                                                  color_expression_cache,
                                                                                  value_expression_cache);
      const std::optional<uint32_t> object_b = BuildInputValueExpression(program,
                                                                        node,
                                                                        "Object B",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        bool_expression_cache,
                                                                        int_expression_cache,
                                                                        float_expression_cache,
                                                                        string_expression_cache,
                                                                        vector_expression_cache,
                                                                        color_expression_cache,
                                                                        value_expression_cache);
      if (!object_b) {
        break;
      }
      if (NamesMatch(output_socket.name, output_socket.identifier, "Colliding")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ObjectsColliding;
        if (object_a) {
          expression.input0 = *object_a;
        }
        expression.input1 = *object_b;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ReceiveEvent:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> subject_expr = BuildInputStringExpression(program,
                                                                              node,
                                                                              "Subject",
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              string_expression_cache);
      if (!subject_expr) {
        break;
      }
      const bool use_target = node.custom1 != 0;
      const std::optional<uint32_t> target_expr = use_target ?
                                                      BuildInputValueExpression(program,
                                                                                node,
                                                                                "Target",
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                bool_expression_cache,
                                                                                int_expression_cache,
                                                                                float_expression_cache,
                                                                                string_expression_cache,
                                                                                vector_expression_cache,
                                                                                color_expression_cache,
                                                                                value_expression_cache) :
                                                      std::nullopt;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Received") ||
          NamesMatch(output_socket.name, output_socket.identifier, "Out"))
      {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::EventReceived;
        expression.input0 = *subject_expr;
        expression.bool_value = use_target;
        if (use_target && target_expr) {
          expression.input1 = *target_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Content")) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::EventContent;
        expression.input0 = *subject_expr;
        expression.value.exists = true;
        expression.value.bool_value = use_target;
        if (use_target && target_expr) {
          expression.input1 = *target_expr;
        }
        result = program.AddValueExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Messenger")) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::EventMessenger;
        expression.input0 = *subject_expr;
        expression.value.exists = true;
        expression.value.bool_value = use_target;
        if (use_target && target_expr) {
          expression.input1 = *target_expr;
        }
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::AnimationStatus:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      const std::optional<uint32_t> layer_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Layer",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      if (!layer_expr) {
        break;
      }
      if (NamesMatch(output_socket.name, output_socket.identifier, "Is Playing")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::AnimationPlaying;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.int_expr_index = *layer_expr;
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Action Name")) {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::AnimationActionName;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.int_expr_index = *layer_expr;
        result = program.AddStringExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Action Frame")) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::AnimationFrame;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.input1 = *layer_expr;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::Collision:
    case LN_NodeKind::OnCollision:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache,
                                                                            int_expression_cache,
                                                                            float_expression_cache,
                                                                            string_expression_cache,
                                                                            vector_expression_cache,
                                                                            color_expression_cache,
                                                                            value_expression_cache);
      const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Property",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache);
      const std::optional<uint32_t> material_expr = BuildInputValueExpression(program,
                                                                              node,
                                                                              "Material",
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              bool_expression_cache,
                                                                              int_expression_cache,
                                                                              float_expression_cache,
                                                                              string_expression_cache,
                                                                              vector_expression_cache,
                                                                              color_expression_cache,
                                                                              value_expression_cache);
      const int event_payload_detail = CollisionOutputPayloadDetail(node, input_links);
      if (NamesMatch(output_socket.name, output_socket.identifier, "Colliding") ||
          NamesMatch(output_socket.name, output_socket.identifier, "On Collision"))
      {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::CollisionDetected;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "On Enter")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::CollisionEnter;
        expression.int_value = event_payload_detail;
        expression.bool_value = event_payload_detail == 2;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "On Stay")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::CollisionStay;
        expression.int_value = event_payload_detail;
        expression.bool_value = event_payload_detail == 2;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "On Exit")) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::CollisionExit;
        expression.int_value = event_payload_detail;
        expression.bool_value = event_payload_detail == 2;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddBoolExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Collided Object")) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::CollisionHitObject;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddValueExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Collided Objects") ||
               NamesMatch(output_socket.name, output_socket.identifier, "Points") ||
               NamesMatch(output_socket.name, output_socket.identifier, "Normals"))
      {
        LN_ValueExpression expression;
        if (NamesMatch(output_socket.name, output_socket.identifier, "Collided Objects")) {
          expression.kind = LN_ValueExpressionKind::CollisionHitObjects;
        }
        else if (NamesMatch(output_socket.name, output_socket.identifier, "Points")) {
          expression.kind = LN_ValueExpressionKind::CollisionHitPoints;
        }
        else {
          expression.kind = LN_ValueExpressionKind::CollisionHitNormals;
        }
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddValueExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Contact Count")) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::CollisionContactCount;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddIntExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Point")) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::CollisionHitPoint;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normal")) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::CollisionHitNormal;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        if (property_expr) {
          expression.input1 = *property_expr;
        }
        if (material_expr) {
          expression.input2 = *material_expr;
        }
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::GetRigidBodyAttribute:
      r_handled = true;
      {
      const int attribute_type = node.custom1;
      const bool valid_output = NamesMatch(output_socket.name, output_socket.identifier, "Valid");
      const bool enabled_output = NamesMatch(output_socket.name, output_socket.identifier, "Enabled") &&
                                  GetRigidBodyAttributeUsesEnabledOutput(attribute_type);
      const bool sleeping_output = NamesMatch(output_socket.name,
                                              output_socket.identifier,
                                              "Allow Sleeping") &&
                                   GetRigidBodyAttributeUsesSleepingOutput(attribute_type);
      if (!valid_output && !enabled_output && !sleeping_output) {
        break;
      }

      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::RigidBodyAttribute;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.int_value = attribute_type;
      expression.secondary_int_value = valid_output ? 0 : (enabled_output ? 1 : 2);
      result = program.AddBoolExpression(expression);
      break;
    }case LN_NodeKind::GetObjectAttribute:
      r_handled = true;
      {
      r_handled = true;
      if (node.custom1 != 2 ||
          !NamesMatch(output_socket.name, output_socket.identifier, "Visible"))
      {
        break;
      }
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::SnapshotVisibility;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddBoolExpression(expression);
      break;
    }case LN_NodeKind::GamepadActive:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      const std::optional<uint32_t> index_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Index",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      if (index_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::GamepadActive;
        expression.input0 = *index_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::GamepadButton:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Out") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Pressed"))
      {
        break;
      }
      IntExpressionCache int_expression_cache;
      const std::optional<uint32_t> index_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Index",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      const GamepadButtonTarget target = GamepadButtonTargetFromNode(
          node, node_definitions, input_links, value_cache);
      if (index_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::GamepadButton;
        expression.input0 = *index_expr;
        expression.int_value = target.index;
        expression.bool_value = target.is_trigger;
        LN_IntExpression input_type_expr;
        input_type_expr.kind = LN_IntExpressionKind::Constant;
        input_type_expr.int_value = MouseInputStatusFromCustom(node.custom1);
        expression.int_expr_index = program.AddIntExpression(input_type_expr);
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::Raycast:
      r_handled = true;
      {
      r_handled = true;
      const bool has_result_output = NamesMatch(output_socket.name,
                                                output_socket.identifier,
                                                "Has Result");
      const bool blocked_output = NamesMatch(output_socket.name,
                                             output_socket.identifier,
                                             "Blocked");
      const bool has_uv_output = NamesMatch(output_socket.name, output_socket.identifier, "Has UV");
      if (NamesMatch(output_socket.name, output_socket.identifier, "Done")) {
        const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_expression_cache);
        if (query_expr) {
          LN_BoolExpression expression;
          expression.kind = LN_BoolExpressionKind::PhysicsQueryDone;
          expression.input0 = *query_expr;
          result = program.AddBoolExpression(expression);
        }
        break;
      }
      if (!has_result_output && !blocked_output && !has_uv_output) {
        break;
      }

      const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                             node,
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             float_expression_cache,
                                                                             bool_expression_cache);
      if (query_expr) {
        if (has_uv_output) {
          program.AddRayQueryDetailRequirement(
              node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        }
        LN_BoolExpression expression;
        expression.kind = has_uv_output ? LN_BoolExpressionKind::PhysicsQueryHasUV :
                          blocked_output ? LN_BoolExpressionKind::PhysicsQueryBlocked :
                                           LN_BoolExpressionKind::PhysicsQueryHit;
        expression.input0 = *query_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::RaycastAll:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Done")) {
        const std::optional<uint32_t> query_expr = BuildRaycastAllQueryExpression(program,
                                                                                  node,
                                                                                  node_definitions,
                                                                                  input_links,
                                                                                  value_cache,
                                                                                  float_expression_cache,
                                                                                  bool_expression_cache);
        if (query_expr) {
          LN_BoolExpression expression;
          expression.kind = LN_BoolExpressionKind::PhysicsQueryDone;
          expression.input0 = *query_expr;
          result = program.AddBoolExpression(expression);
        }
        break;
      }
      const bool has_result_output = NamesMatch(output_socket.name,
                                                output_socket.identifier,
                                                "Has Result");
      const bool blocked_output = NamesMatch(output_socket.name,
                                             output_socket.identifier,
                                             "Blocked");
      if (!has_result_output && !blocked_output) {
        break;
      }

      const std::optional<uint32_t> query_expr = BuildRaycastAllQueryExpression(program,
                                                                                node,
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                float_expression_cache,
                                                                                bool_expression_cache);
      if (query_expr) {
        LN_BoolExpression expression;
        expression.kind = blocked_output ? LN_BoolExpressionKind::PhysicsQueryBlocked :
                                           LN_BoolExpressionKind::PhysicsQueryHit;
        expression.input0 = *query_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCast:
      r_handled = true;
      {
      const bool done = NamesMatch(output_socket.name, output_socket.identifier, "Done");
      const bool hit = NamesMatch(output_socket.name, output_socket.identifier, "Has Result");
      const bool blocked = NamesMatch(output_socket.name, output_socket.identifier, "Blocked");
      const bool overlapping = NamesMatch(
          output_socket.name, output_socket.identifier, "Started Overlapping");
      const bool has_uv = NamesMatch(output_socket.name, output_socket.identifier, "Has UV");
      if (!done && !hit && !blocked && !overlapping && !has_uv) {
        break;
      }
      const std::optional<uint32_t> query_expr = BuildShapeCastQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_expression_cache);
      if (query_expr) {
        if (has_uv) {
          program.AddRayQueryDetailRequirement(
              node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        }
        LN_BoolExpression expression;
        expression.kind = done ? LN_BoolExpressionKind::PhysicsQueryDone :
                          has_uv ? LN_BoolExpressionKind::PhysicsQueryHasUV :
                          blocked ? LN_BoolExpressionKind::PhysicsQueryBlocked :
                          overlapping ?
                              LN_BoolExpressionKind::PhysicsQueryStartedOverlapping :
                              LN_BoolExpressionKind::PhysicsQueryHit;
        expression.input0 = *query_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCastAll:
      r_handled = true;
      {
      const bool done = NamesMatch(output_socket.name, output_socket.identifier, "Done");
      const bool hit = NamesMatch(output_socket.name, output_socket.identifier, "Has Result");
      const bool blocked = NamesMatch(output_socket.name, output_socket.identifier, "Blocked");
      if (!done && !hit && !blocked) {
        break;
      }
      const std::optional<uint32_t> query_expr = BuildShapeCastAllQueryExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          bool_expression_cache);
      if (query_expr) {
        LN_BoolExpression expression;
        expression.kind = done ? LN_BoolExpressionKind::PhysicsQueryDone :
                          blocked ? LN_BoolExpressionKind::PhysicsQueryBlocked :
                                    LN_BoolExpressionKind::PhysicsQueryHit;
        expression.input0 = *query_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::MouseRay:
    case LN_NodeKind::CameraRay:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Has Result")) {
        break;
      }

      const std::optional<uint32_t> query_expr =
          definition.kind == LN_NodeKind::MouseRay ?
              BuildMouseRayQueryExpression(program,
                                           node,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_expression_cache) :
              BuildCameraRayQueryExpression(program,
                                            node,
                                            node_definitions,
                                            input_links,
                                            value_cache,
                                            float_expression_cache,
                                            bool_expression_cache);
      if (query_expr) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::PhysicsQueryHit;
        expression.input0 = *query_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::Gate:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> input0 = BuildInputBoolExpression(program,
                                                                      node,
                                                                      "Condition A",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
      if (!input0) {
        break;
      }

      if (node.custom1 == 3) {
        result = AddNotBoolExpression(program, *input0);
        break;
      }

      const std::optional<uint32_t> input1 = BuildInputBoolExpression(program,
                                                                      node,
                                                                      "Condition B",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
      if (input1) {
        result = AddLogicGateBoolExpression(program, node.custom1, *input0, *input1);
      }
      break;
    }case LN_NodeKind::GateList:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> input0 = BuildInputBoolExpression(program,
                                                                      node,
                                                                      "Condition A",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
      if (!input0) {
        break;
      }

      if (node.custom1 == 3) {
        result = AddNotBoolExpression(program, *input0);
        break;
      }

      result = input0;
      const char *input_names[] = {"Condition B", "Condition C", "Condition D"};
      for (const char *input_name : input_names) {
        const std::optional<uint32_t> next = BuildInputBoolExpression(program,
                                                                      node,
                                                                      input_name,
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
        if (!next) {
          result.reset();
          break;
        }
        result = AddLogicGateBoolExpression(program, node.custom1, *result, *next);
        if (!result) {
          break;
        }
      }
      break;
    }case LN_NodeKind::OnNextFrame:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> condition =
          BuildPrimaryExecutionExpression(program,
                                                            node,
                                                            definition,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache,
                                                            float_expression_cache,
                                                            bool_expression_cache);
      if (condition) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::OnNextTick;
        expression.input0 = *condition;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ValueChanged:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "If Changed") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Out"))
      {
        break;
      }
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> value = BuildInputValueExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      bool_expression_cache,
                                                                      int_expression_cache,
                                                                      float_expression_cache,
                                                                      string_expression_cache,
                                                                      vector_expression_cache,
                                                                      color_expression_cache,
                                                                      value_expression_cache);
      if (value) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ValueChanged;
        expression.input0 = *value;
        expression.bool_value = node.custom1 != 0;
        result = program.AddBoolExpression(expression);
      }
      break;
    }case LN_NodeKind::ValueChangedTo:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Result") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Out"))
      {
        break;
      }
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> value = BuildInputValueExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      bool_expression_cache,
                                                                      int_expression_cache,
                                                                      float_expression_cache,
                                                                      string_expression_cache,
                                                                      vector_expression_cache,
                                                                      color_expression_cache,
                                                                      value_expression_cache);
      const std::optional<uint32_t> target = BuildInputValueExpression(program,
                                                                       node,
                                                                       "Target",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       bool_expression_cache,
                                                                       int_expression_cache,
                                                                       float_expression_cache,
                                                                       string_expression_cache,
                                                                       vector_expression_cache,
                                                                       color_expression_cache,
                                                                       value_expression_cache);
      if (value && target) {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::ValueChangedTo;
        expression.input0 = *value;
        expression.input1 = *target;
        result = program.AddBoolExpression(expression);
      }
      break;
    }
    default:
      break;
  }

  if (r_handled) {
    bool_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionIntOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
  IntExpressionCache &int_expression_cache,
  BoolExpressionCache *bool_expression_cache,
  FloatExpressionCache *float_expression_cache,
  StringExpressionCache *string_expression_cache,
  VectorExpressionCache *vector_expression_cache,
  ColorExpressionCache *color_expression_cache,
  ValueExpressionCache *value_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = int_expression_cache.find(&output_socket);
  if (cache_iter != int_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
case LN_NodeKind::GetGamePropertyInt:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        LN_Value default_value;
        default_value.type = LN_ValueType::Int;
        const uint32_t property_ref_index = AddGamePropertyRef(
            program, name->string_value, LN_ValueType::Int, default_value);
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Int, MakeDefaultValue(LN_ValueType::Int));
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::AssignGeometryNodesModifier:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Modifier ID")) {
        break;
      }
      const uint32_t property_ref_index = AddTreePropertyRef(
          program,
          AssignGeometryNodesModifierResultPropertyName(node),
          LN_ValueType::Int,
          MakeDefaultValue(LN_ValueType::Int));
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::RuntimeTreeProperty;
      expression.property_ref_index = property_ref_index;
      result = program.AddIntExpression(expression);
      break;
    }case LN_NodeKind::GetCollisionGroup:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      FloatExpressionCache &float_cache = float_expression_cache ? *float_expression_cache :
                                                                float_expression_cache_local;
      StringExpressionCache &string_cache = string_expression_cache ? *string_expression_cache :
                                                                    string_expression_cache_local;
      VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                    vector_expression_cache_local;
      ColorExpressionCache &color_cache = color_expression_cache ? *color_expression_cache :
                                                                  color_expression_cache_local;
      ValueExpressionCache &value_cache_local = value_expression_cache ? *value_expression_cache :
                                                                        value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_cache,
          int_expression_cache,
          float_cache,
          string_cache,
          vector_cache,
          color_cache,
          value_cache_local);
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::SnapshotCollisionGroup;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddIntExpression(expression);
      break;
    }case LN_NodeKind::GetCharacterInfo:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      FloatExpressionCache &float_cache = float_expression_cache ? *float_expression_cache :
                                                                float_expression_cache_local;
      StringExpressionCache &string_cache = string_expression_cache ? *string_expression_cache :
                                                                    string_expression_cache_local;
      VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                    vector_expression_cache_local;
      ColorExpressionCache &color_cache = color_expression_cache ? *color_expression_cache :
                                                                  color_expression_cache_local;
      ValueExpressionCache &value_cache_local = value_expression_cache ? *value_expression_cache :
                                                                        value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_cache,
          int_expression_cache,
          float_cache,
          string_cache,
          vector_cache,
          color_cache,
          value_cache_local);
      LN_IntExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Max Jumps")) {
        expression.kind = LN_IntExpressionKind::SnapshotCharacterMaxJumps;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Current Jump Count")) {
        expression.kind = LN_IntExpressionKind::SnapshotCharacterJumpCount;
      }
      else {
        break;
      }
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddIntExpression(expression);
      break;
    }case LN_NodeKind::GetResolution:
      r_handled = true;
      {
      r_handled = true;
      LN_IntExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Width")) {
        expression.kind = LN_IntExpressionKind::WindowResolutionWidth;
        result = program.AddIntExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Height")) {
        expression.kind = LN_IntExpressionKind::WindowResolutionHeight;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::GetVSync:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Mode")) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::WindowVSyncMode;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::KeyCode:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<LN_Value> key_name = ReadInputValue(node,
                                                              "Key",
                                                              LN_ValueType::String,
                                                              node_definitions,
                                                              input_links,
                                                              value_cache);
      if (key_name && key_name->type == LN_ValueType::String) {
        result = AddConstantIntExpression(program, int32_t(InputCodeFromName(key_name->string_value)));
      }
      break;
    }case LN_NodeKind::MouseWheel:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Difference")) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::MouseWheelDelta;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::KeyLogger:
      r_handled = true;
      {
      r_handled = true;
      bool only_characters = true;
      const std::optional<LN_Value> only_chars = ReadInputValue(node,
                                                                "Only Characters",
                                                                LN_ValueType::Bool,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (only_chars && only_chars->type == LN_ValueType::Bool) {
        only_characters = only_chars->bool_value;
      }
      LN_StringExpression expression;
      expression.int_expr_index = AddConstantIntExpression(program, only_characters ? 1 : 0);
      if (NamesMatch(output_socket.name, output_socket.identifier, "Character")) {
        expression.kind = LN_StringExpressionKind::KeyLoggerCharacter;
        result = program.AddStringExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Keycode")) {
        expression.kind = LN_StringExpressionKind::KeyLoggerKeycode;
        result = program.AddStringExpression(expression);
      }
      break;
    }case LN_NodeKind::Raycast:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Face Index")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      FloatExpressionCache &ray_float_cache = float_expression_cache ? *float_expression_cache :
                                                                    float_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                             node,
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             ray_float_cache,
                                                                             bool_expression_cache_local);
      if (query_expr) {
        program.AddRayQueryDetailRequirement(node.identifier,
                                             LN_RAY_QUERY_DETAIL_FACE_INDEX);
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::PhysicsQueryFaceIndex;
        expression.input0 = *query_expr;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::RaycastAll:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Hit Count")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      FloatExpressionCache &ray_float_cache = float_expression_cache ? *float_expression_cache :
                                                                    float_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastAllQueryExpression(program,
                                                                                node,
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                ray_float_cache,
                                                                                bool_expression_cache_local);
      if (query_expr) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::PhysicsQueryHitCount;
        expression.input0 = *query_expr;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCastAll:
      r_handled = true;
      {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Hit Count")) {
        break;
      }
      BoolExpressionCache bool_cache;
      FloatExpressionCache local_float_cache;
      FloatExpressionCache &shape_float_cache = float_expression_cache ?
                                                     *float_expression_cache :
                                                     local_float_cache;
      const std::optional<uint32_t> query_expr = BuildShapeCastAllQueryExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          shape_float_cache,
          bool_cache);
      if (query_expr) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::PhysicsQueryHitCount;
        expression.input0 = *query_expr;
        result = program.AddIntExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCast:
      r_handled = true;
      {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Face Index")) {
        break;
      }
      BoolExpressionCache bool_cache;
      FloatExpressionCache local_float_cache;
      FloatExpressionCache &shape_float_cache = float_expression_cache ?
                                                     *float_expression_cache :
                                                     local_float_cache;
      const std::optional<uint32_t> query_expr = BuildShapeCastQueryExpression(program,
                                                                                node,
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                shape_float_cache,
                                                                                bool_cache);
      if (query_expr) {
        program.AddRayQueryDetailRequirement(node.identifier,
                                             LN_RAY_QUERY_DETAIL_FACE_INDEX);
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::PhysicsQueryFaceIndex;
        expression.input0 = *query_expr;
        result = program.AddIntExpression(expression);
      }
      break;
    }    default:
      break;
  }

  if (r_handled) {
    int_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionFloatOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache *bool_expression_cache,
    VectorExpressionCache *vector_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  VectorExpressionCache local_vector_expression_cache;
  VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                 local_vector_expression_cache;

  const auto cache_iter = float_expression_cache.find(&output_socket);
  if (cache_iter != float_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
case LN_NodeKind::LimitRange:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache);
      const std::optional<uint32_t> min_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Min",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
      const std::optional<uint32_t> max_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Max",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
      if (value && min_value && max_value) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::Clamp;
        expression.input0 = *value;
        expression.input1 = *min_value;
        expression.input2 = *max_value;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::RandomValue:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> min_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Min",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
      const std::optional<uint32_t> max_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Max",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
      if (min_value && max_value) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::Random;
        expression.input0 = *min_value;
        expression.input1 = *max_value;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::ValueSwitch:
      r_handled = true;
      {
      r_handled = true;
      if (bool_expression_cache == nullptr) {
        break;
      }
      const std::optional<uint32_t> condition = BuildInputBoolExpression(program,
                                                                         node,
                                                                         "Condition",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         *bool_expression_cache);
      const std::optional<uint32_t> true_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "True",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
      const std::optional<uint32_t> false_value = BuildInputFloatExpression(program,
                                                                           node,
                                                                           "False",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           float_expression_cache);
      if (condition && true_value && false_value) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::Select;
        expression.bool_expr_index = *condition;
        expression.input0 = *true_value;
        expression.input1 = *false_value;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::StoreValue:
      r_handled = true;
      {
      r_handled = true;
      if (bool_expression_cache == nullptr) {
        break;
      }
      const std::optional<uint32_t> condition =
          BuildPrimaryExecutionExpression(program,
                                                            node,
                                                            definition,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache,
                                                            float_expression_cache,
                                                            *bool_expression_cache);
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache);
      if (condition && value) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::StoreValue;
        expression.bool_expr_index = *condition;
        expression.input0 = *value;
        expression.bool_value = node.custom1 == 0;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::GetGamePropertyFloat:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<LN_Value> name = ReadInputValue(node,
                                                          "Property",
                                                          LN_ValueType::String,
                                                          node_definitions,
                                                          input_links,
                                                          value_cache);
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        LN_Value default_value;
        default_value.type = LN_ValueType::Float;
        const uint32_t property_ref_index = AddGamePropertyRef(
            program, name->string_value, LN_ValueType::Float, default_value);
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::GetBoneLength:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Length")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache_local,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> bone_name_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Bone Name",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache_local);
      if (bone_name_expr) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::BoneLength;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.input1 = *bone_name_expr;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Float, MakeDefaultValue(LN_ValueType::Float));
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTimescale:
      r_handled = true;
      {
      r_handled = true;
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::SnapshotTimeScale;
      result = program.AddFloatExpression(expression);
      break;
    }case LN_NodeKind::Time:
      r_handled = true;
      {
      r_handled = true;
      LN_FloatExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Time")) {
        expression.kind = LN_FloatExpressionKind::SnapshotElapsedTime;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Delta")) {
        expression.kind = LN_FloatExpressionKind::SnapshotFrameDelta;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "FPS")) {
        expression.kind = LN_FloatExpressionKind::SnapshotFPS;
      }
      else {
        break;
      }
      result = program.AddFloatExpression(expression);
      break;
    }case LN_NodeKind::DeltaFactor:
      r_handled = true;
      {
      r_handled = true;
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::SnapshotDeltaFactor;
      result = program.AddFloatExpression(expression);
      break;
    }case LN_NodeKind::GetRigidBodyAttribute:
      r_handled = true;
      {
      const int attribute_type = node.custom1;
      const bool value_output = NamesMatch(output_socket.name, output_socket.identifier, "Value") &&
                                GetRigidBodyAttributeUsesScalarOutput(attribute_type);
      const bool linear_damping_output = NamesMatch(output_socket.name,
                                                    output_socket.identifier,
                                                    "Linear Damping") &&
                                         GetRigidBodyAttributeUsesDampingOutput(attribute_type);
      const bool angular_damping_output = NamesMatch(output_socket.name,
                                                     output_socket.identifier,
                                                     "Angular Damping") &&
                                          GetRigidBodyAttributeUsesDampingOutput(attribute_type);
      if (!value_output && !linear_damping_output && !angular_damping_output) {
        break;
      }

      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_cache,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache_local,
          color_expression_cache_local,
          value_expression_cache_local);
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::RigidBodyAttribute;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.property_ref_index = uint32_t(attribute_type);
      expression.component_index = angular_damping_output ? 1 : 0;
      result = program.AddFloatExpression(expression);
      break;
    }case LN_NodeKind::GetLightPower:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      StringExpressionCache &string_cache = string_expression_cache_local;
      VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                    vector_expression_cache_local;
      ColorExpressionCache &color_cache = color_expression_cache_local;
      ValueExpressionCache &value_cache_local = value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_cache,
          int_expression_cache_local,
          float_expression_cache,
          string_cache,
          vector_cache,
          color_cache,
          value_cache_local);
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::SnapshotLightPower;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddFloatExpression(expression);
      break;
    }
    case LN_NodeKind::GetDistance: {
      r_handled = true;
      const bool distance = NamesMatch(output_socket.name, output_socket.identifier, "Distance");
      const bool fraction = NamesMatch(output_socket.name, output_socket.identifier, "Fraction");
      if (!distance && !fraction) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                    vector_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_cache,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> target_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Target",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_cache,
                                                                            int_expression_cache_local,
                                                                            float_expression_cache,
                                                                            string_expression_cache_local,
                                                                            vector_cache,
                                                                            color_expression_cache_local,
                                                                            value_expression_cache_local);
      if (target_expr) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::ObjectDistance;
        if (object_expr) {
          expression.input0 = *object_expr;
        }
        expression.input1 = *target_expr;
        result = program.AddFloatExpression(expression);
      }
      break;
    }
    case LN_NodeKind::CursorPosition:
      r_handled = true;
      case LN_NodeKind::CursorMovement:
      r_handled = true;
      case LN_NodeKind::GamepadStick:
      r_handled = true;
      {
      r_handled = true;
      const char *vector_socket_name = "Vector";
      if (definition.kind == LN_NodeKind::CursorPosition) {
        vector_socket_name = "Position";
      }
      else if (definition.kind == LN_NodeKind::CursorMovement) {
        vector_socket_name = "Movement";
      }

      const blender::bNodeSocket *vector_socket = FindOutputSocket(node, vector_socket_name);
      if (vector_socket == nullptr) {
        break;
      }

      const std::optional<uint32_t> vector_expr = BuildOutputVectorExpression(program,
                                                                              node,
                                                                              *vector_socket,
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              float_expression_cache,
                                                                              vector_cache);
      if (!vector_expr) {
        break;
      }

      if (NamesMatch(output_socket.name, output_socket.identifier, "X")) {
        result = AddVectorComponentFloatExpression(program, *vector_expr, 0);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Y")) {
        result = AddVectorComponentFloatExpression(program, *vector_expr, 1);
      }
      break;
    }case LN_NodeKind::GamepadButton:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Strength")) {
        break;
      }
      IntExpressionCache int_expression_cache;
      const std::optional<uint32_t> index_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Index",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      const GamepadButtonTarget target = GamepadButtonTargetFromNode(
          node, node_definitions, input_links, value_cache);
      if (index_expr && target.is_trigger) {
        LN_FloatExpression expression;
        expression.kind = LN_FloatExpressionKind::GamepadButtonStrength;
        expression.input0 = *index_expr;
        expression.input2 = uint32_t(target.index);
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::Raycast:
      r_handled = true;
      {
      r_handled = true;
      const bool distance = NamesMatch(output_socket.name, output_socket.identifier, "Distance");
      const bool fraction = NamesMatch(output_socket.name, output_socket.identifier, "Fraction");
      if (!distance && !fraction) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                              bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                             node,
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             float_expression_cache,
                                                                             bool_cache);
      if (query_expr) {
        LN_FloatExpression expression;
        expression.kind = distance ? LN_FloatExpressionKind::PhysicsQueryDistance :
                                     LN_FloatExpressionKind::PhysicsQueryFraction;
        expression.input0 = *query_expr;
        result = program.AddFloatExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCast:
      r_handled = true;
      {
      const bool distance = NamesMatch(output_socket.name, output_socket.identifier, "Distance");
      const bool fraction = NamesMatch(output_socket.name, output_socket.identifier, "Fraction");
      const bool penetration = NamesMatch(
          output_socket.name, output_socket.identifier, "Penetration Depth");
      if (!distance && !fraction && !penetration) {
        break;
      }
      BoolExpressionCache local_bool_cache;
      BoolExpressionCache &shape_bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                                     local_bool_cache;
      const std::optional<uint32_t> query_expr = BuildShapeCastQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               shape_bool_cache);
      if (query_expr) {
        LN_FloatExpression expression;
        expression.kind = distance ? LN_FloatExpressionKind::PhysicsQueryDistance :
                          fraction ? LN_FloatExpressionKind::PhysicsQueryFraction :
                                     LN_FloatExpressionKind::PhysicsQueryPenetrationDepth;
        expression.input0 = *query_expr;
        result = program.AddFloatExpression(expression);
      }
      break;
    }    default:
      break;
  }

  if (r_handled) {
    float_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionStringOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    StringExpressionCache &string_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = string_expression_cache.find(&output_socket);
  if (cache_iter != string_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
case LN_NodeKind::GetGamePropertyString:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<LN_Value> name = ReadInputValue(node,
                                                          "Property",
                                                          LN_ValueType::String,
                                                          node_definitions,
                                                          input_links,
                                                          value_cache);
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        LN_Value default_value;
        default_value.type = LN_ValueType::String;
        const uint32_t property_ref_index = AddGamePropertyRef(
            program, name->string_value, LN_ValueType::String, default_value);
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::SnapshotGameProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddStringExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program,
            name->string_value,
            LN_ValueType::String,
            MakeDefaultValue(LN_ValueType::String));
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddStringExpression(expression);
      }
      break;
    }case LN_NodeKind::GetObjectAttribute:
      r_handled = true;
      {
      r_handled = true;
      if (node.custom1 != 3 || !NamesMatch(output_socket.name, output_socket.identifier, "Name")) {
        break;
      }
      BoolExpressionCache bool_expression_cache;
      IntExpressionCache int_expression_cache;
      FloatExpressionCache float_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      const std::optional<uint32_t> attribute_expr =
          BuildGetObjectAttributeAttributeExpression(program, node);
      if (!attribute_expr) {
        break;
      }
      LN_ValueExpression value_expression;
      value_expression.kind = LN_ValueExpressionKind::ObjectAttribute;
      if (object_expr) {
        value_expression.input0 = *object_expr;
      }
      value_expression.input1 = *attribute_expr;
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind::FromGenericValue;
      expression.value_expr_index = program.AddValueExpression(value_expression);
      result = program.AddStringExpression(expression);
      break;
    }    default:
      break;
  }

  if (r_handled) {
    string_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionVectorOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = vector_expression_cache.find(&output_socket);
  if (cache_iter != vector_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
case LN_NodeKind::GetBoneHeadWorld:
      r_handled = true;
      case LN_NodeKind::GetBoneTailWorld:
      r_handled = true;
      case LN_NodeKind::GetBoneCenterWorld:
      r_handled = true;
      case LN_NodeKind::GetBoneHeadPoseWorld:
      r_handled = true;
      case LN_NodeKind::GetBoneTailPoseWorld:
      r_handled = true;
      case LN_NodeKind::GetBoneCenterPoseWorld:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> bone_name_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Bone Name",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache_local);
      if (!bone_name_expr || !NamesMatch(output_socket.name, output_socket.identifier, "Position")) {
        break;
      }
      LN_VectorExpression expression;
      if (definition.kind == LN_NodeKind::GetBoneTailWorld) {
        expression.kind = LN_VectorExpressionKind::BoneTailWorld;
      }
      else if (definition.kind == LN_NodeKind::GetBoneTailPoseWorld) {
        expression.kind = LN_VectorExpressionKind::BoneTailPoseWorld;
      }
      else if (definition.kind == LN_NodeKind::GetBoneCenterWorld) {
        expression.kind = LN_VectorExpressionKind::BoneCenterWorld;
      }
      else if (definition.kind == LN_NodeKind::GetBoneHeadPoseWorld) {
        expression.kind = LN_VectorExpressionKind::BoneHeadPoseWorld;
      }
      else if (definition.kind == LN_NodeKind::GetBoneCenterPoseWorld) {
        expression.kind = LN_VectorExpressionKind::BoneCenterPoseWorld;
      }
      else {
        expression.kind = LN_VectorExpressionKind::BoneHeadWorld;
      }
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.input1 = *bone_name_expr;
      if (definition.kind == LN_NodeKind::GetBoneHeadPoseWorld ||
          definition.kind == LN_NodeKind::GetBoneTailPoseWorld ||
          definition.kind == LN_NodeKind::GetBoneCenterPoseWorld)
      {
        expression.property_ref_index = uint32_t(std::clamp(int(node.custom1),
                                                            int(LN_BonePosePositionSpace::World),
                                                            int(LN_BonePosePositionSpace::Armature)));
      }
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::GetBonePoseScale:
    case LN_NodeKind::GetBonePoseTransform: {
      r_handled = true;
      const bool is_transform = definition.kind == LN_NodeKind::GetBonePoseTransform;
      if ((!is_transform && !NamesMatch(output_socket.name, output_socket.identifier, "Scale")) ||
          (is_transform && !NamesMatch(output_socket.name, output_socket.identifier, "Location") &&
           !NamesMatch(output_socket.name, output_socket.identifier, "Scale")))
      {
        break;
      }

      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> bone_name_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Bone Name",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache_local);
      if (!bone_name_expr) {
        break;
      }

      LN_VectorExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Scale")) {
        expression.kind = LN_VectorExpressionKind::BonePoseScale;
      }
      else {
        expression.kind = LN_VectorExpressionKind::BonePoseLocation;
        expression.property_ref_index = uint32_t(std::clamp(
            int(node.custom1),
            int(LN_BonePoseRotationSpace::PoseChannel),
            int(LN_BonePoseRotationSpace::World)));
      }
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.input1 = *bone_name_expr;
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::EvaluateCurve: {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Point")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> curve_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Curve",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> factor_expr = BuildInputFloatExpression(program,
                                                                            node,
                                                                            "Factor",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            float_expression_cache);
      if (!factor_expr) {
        break;
      }
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::EvaluateCurveAtFactor;
      if (curve_expr) {
        expression.input0 = *curve_expr;
      }
      expression.float_expr_index = *factor_expr;
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::GetRigidBodyAttribute:
      r_handled = true;
      {
      const int attribute_type = node.custom1;
      const bool lock_translation_output = NamesMatch(output_socket.name,
                                                      output_socket.identifier,
                                                      "Lock Translation") &&
                                           GetRigidBodyAttributeUsesAxisLocksOutput(attribute_type);
      const bool lock_rotation_output = NamesMatch(output_socket.name,
                                                   output_socket.identifier,
                                                   "Lock Rotation") &&
                                        GetRigidBodyAttributeUsesAxisLocksOutput(attribute_type);
      if (!lock_translation_output && !lock_rotation_output) {
        break;
      }

      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::RigidBodyAttribute;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      expression.property_ref_index = uint32_t(attribute_type);
      expression.bool_value = lock_rotation_output;
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::GetObjectAttribute:
      r_handled = true;
      {
      r_handled = true;
      const bool vector_output = NamesMatch(output_socket.name, output_socket.identifier, "Vector");
      const bool position_output = NamesMatch(output_socket.name,
                                              output_socket.identifier,
                                              "Position");
      const bool orientation_output = NamesMatch(output_socket.name,
                                                 output_socket.identifier,
                                                 "Orientation");
      const bool scale_output = NamesMatch(output_socket.name, output_socket.identifier, "Scale");
      const bool transform_output = GetObjectAttributeUsesTransformOutput(node.custom1);
      const char *attribute_name = nullptr;
      if (vector_output && GetObjectAttributeUsesVectorOutput(node.custom1)) {
        attribute_name = GetObjectAttributeName(node.custom1);
      }
      else if (orientation_output && GetObjectAttributeUsesOrientationOutput(node.custom1)) {
        attribute_name = GetObjectAttributeName(node.custom1);
      }
      else if (transform_output && position_output) {
        attribute_name = GetObjectTransformPositionAttributeName(node.custom1);
      }
      else if (transform_output && orientation_output) {
        attribute_name = GetObjectTransformOrientationAttributeName(node.custom1);
      }
      else if (transform_output && scale_output) {
        attribute_name = GetObjectTransformScaleAttributeName(node.custom1);
      }
      if (attribute_name == nullptr) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const uint32_t attribute_expr = AddConstantStringExpression(program, attribute_name);
      LN_ValueExpression value_expression;
      value_expression.kind = LN_ValueExpressionKind::ObjectAttribute;
      if (object_expr) {
        value_expression.input0 = *object_expr;
      }
      value_expression.input1 = attribute_expr;
      const uint32_t value_expr_index = program.AddValueExpression(value_expression);
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::FromGenericValue;
      expression.input0 = value_expr_index;
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::GetGroupCenterPosition: {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Center")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> objects_expr = BuildInputValueExpression(
          program,
          node,
          "Objects",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> collection_expr = BuildInputValueExpression(
          program,
          node,
          "Collection",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      if (objects_expr || collection_expr) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::GroupCenterPosition;
        if (objects_expr) {
          expression.input0 = *objects_expr;
        }
        if (collection_expr) {
          expression.input1 = *collection_expr;
        }
        result = program.AddVectorExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetCharacterInfo:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache,
          string_expression_cache_local,
          vector_expression_cache,
          color_expression_cache_local,
          value_expression_cache_local);
      LN_VectorExpression expression;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Gravity")) {
        expression.kind = LN_VectorExpressionKind::SnapshotCharacterGravity;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Walk Direction")) {
        expression.kind = node.custom1 != 0 ?
                              LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection :
                              LN_VectorExpressionKind::SnapshotCharacterWalkDirection;
      }
      else {
        break;
      }
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddVectorExpression(expression);
      break;
    }case LN_NodeKind::GetResolution:
      r_handled = true;
      {
      r_handled = true;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Resolution")) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::WindowResolution;
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Vector, MakeDefaultValue(LN_ValueType::Vector));
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::CursorPosition:
      r_handled = true;
      {
      r_handled = true;
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::CursorPosition;
      expression.property_ref_index = uint32_t(node.custom1 & 1);
      result = program.AddVectorExpression(expression);
      break;
    }case LN_NodeKind::CursorMovement:
      r_handled = true;
      {
      r_handled = true;
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::CursorMovement;
      result = program.AddVectorExpression(expression);
      break;
    }case LN_NodeKind::WorldToScreen:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> point_expr = BuildInputVectorExpression(program,
                                                                            node,
                                                                            "Point",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            float_expression_cache,
                                                                            vector_expression_cache);
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> camera_expr = BuildInputOrActiveCameraExpression(
          program,
          node,
          "Camera",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache_local);
      if (point_expr && camera_expr && NamesMatch(output_socket.name, output_socket.identifier, "On Screen")) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::WorldToScreen;
        expression.input0 = *point_expr;
        expression.input1 = *camera_expr;
        expression.vector_value = VectorExpressionConstantFallback(program, *point_expr);
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::ScreenToWorld:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> screen_x_expr = BuildInputFloatExpression(
          program,
          node,
          "Screen X",
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache);
      const std::optional<uint32_t> screen_y_expr = BuildInputFloatExpression(
          program,
          node,
          "Screen Y",
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache);
      const std::optional<uint32_t> depth_expr = BuildInputFloatExpression(program,
                                                                           node,
                                                                           "Depth",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           float_expression_cache);
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      ColorExpressionCache color_expression_cache;
      ValueExpressionCache value_expression_cache_local;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> camera_expr = BuildInputOrActiveCameraExpression(
          program,
          node,
          "Camera",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache_local);
      if (screen_x_expr && screen_y_expr && depth_expr && camera_expr &&
          NamesMatch(output_socket.name, output_socket.identifier, "World Position"))
      {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::ScreenToWorld;
        expression.input0 = *camera_expr;
        expression.input1 = *screen_x_expr;
        expression.input2 = *screen_y_expr;
        expression.float_expr_index = *depth_expr;
        expression.vector_value = MT_Vector3(FloatExpressionConstantFallback(program, *screen_x_expr),
                                             FloatExpressionConstantFallback(program, *screen_y_expr),
                                             FloatExpressionConstantFallback(program, *depth_expr));
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::GamepadStick:
      r_handled = true;
      {
      r_handled = true;
      IntExpressionCache int_expression_cache;
      const std::optional<uint32_t> index_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Index",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      const std::optional<uint32_t> sensitivity_expr = BuildInputFloatExpression(
          program,
          node,
          "Sensitivity",
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache);
      const std::optional<uint32_t> threshold_expr = BuildInputFloatExpression(program,
                                                                               node,
                                                                               "Threshold",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache);
      const std::optional<LN_Value> invert_x = ReadInputValue(node,
                                                              "Invert X",
                                                              LN_ValueType::Bool,
                                                              node_definitions,
                                                              input_links,
                                                              value_cache);
      const std::optional<LN_Value> invert_y = ReadInputValue(node,
                                                              "Invert Y",
                                                              LN_ValueType::Bool,
                                                              node_definitions,
                                                              input_links,
                                                              value_cache);
      if (index_expr && sensitivity_expr && threshold_expr)
      {
        int32_t invert_flags = 0;
        if (invert_x && invert_x->type == LN_ValueType::Bool && invert_x->bool_value) {
          invert_flags |= 1;
        }
        if (invert_y && invert_y->type == LN_ValueType::Bool && invert_y->bool_value) {
          invert_flags |= 2;
        }
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::GamepadStick;
        expression.input0 = *index_expr;
        expression.input1 = *threshold_expr;
        expression.input2 = uint32_t(node.custom1);
        expression.property_ref_index = uint32_t(invert_flags);
        expression.float_expr_index = *sensitivity_expr;
        expression.float_value = FloatExpressionConstantFallback(program, *sensitivity_expr);
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::MouseOver:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildMouseOverQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_VectorExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Point")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normal")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::Raycast:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                             node,
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             float_expression_cache,
                                                                             bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_VectorExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Point")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normal")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Direction")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryDirection;
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "UV")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_VectorExpressionKind::PhysicsQueryUV;
        result = program.AddVectorExpression(expression);
      }
      break;
    }case LN_NodeKind::RaycastAll:
      r_handled = true;
      {
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastAllQueryExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_VectorExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Direction")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryDirection;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "End Point")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryEndPoint;
      }
      else {
        break;
      }
      result = program.AddVectorExpression(expression);
      break;
    }case LN_NodeKind::ShapeCast:
      r_handled = true;
      {
      BoolExpressionCache bool_cache;
      const std::optional<uint32_t> query_expr = BuildShapeCastQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_cache);
      if (!query_expr) {
        break;
      }
      LN_VectorExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Point")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normal")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Cast Position")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryCastPosition;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Direction")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryDirection;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "UV")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_VectorExpressionKind::PhysicsQueryUV;
      }
      else {
        break;
      }
      result = program.AddVectorExpression(expression);
      break;
    }case LN_NodeKind::MouseRay:
    case LN_NodeKind::CameraRay:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr =
          definition.kind == LN_NodeKind::MouseRay ?
              BuildMouseRayQueryExpression(program,
                                           node,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_expression_cache_local) :
              BuildCameraRayQueryExpression(program,
                                            node,
                                            node_definitions,
                                            input_links,
                                            value_cache,
                                            float_expression_cache,
                                            bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_VectorExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Picked Point")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
        result = program.AddVectorExpression(expression);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Picked Normal")) {
        expression.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
        result = program.AddVectorExpression(expression);
      }
      break;
    }
    case LN_NodeKind::VectorMath:
      r_handled = true;
      {
        const std::optional<uint32_t> input0 = BuildInputVectorExpression(program,
                                                                          node,
                                                                          "A",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          vector_expression_cache);
        if (!input0) {
          break;
        }

        LN_VectorExpression expression;
        expression.input0 = *input0;
        switch (node.custom1) {
          case blender::NODE_VECTOR_MATH_ADD:
          case blender::NODE_VECTOR_MATH_SUBTRACT:
          case blender::NODE_VECTOR_MATH_MULTIPLY:
          case blender::NODE_VECTOR_MATH_DIVIDE:
          case blender::NODE_VECTOR_MATH_MINIMUM:
          case blender::NODE_VECTOR_MATH_MAXIMUM: {
            const std::optional<uint32_t> input1 = BuildInputVectorExpression(
                program,
                node,
                "B",
                node_definitions,
                input_links,
                value_cache,
                float_expression_cache,
                vector_expression_cache);
            if (!input1) {
              break;
            }
            expression.input1 = *input1;
            switch (node.custom1) {
              case blender::NODE_VECTOR_MATH_ADD:
                expression.kind = LN_VectorExpressionKind::Add;
                break;
              case blender::NODE_VECTOR_MATH_SUBTRACT:
                expression.kind = LN_VectorExpressionKind::Subtract;
                break;
              case blender::NODE_VECTOR_MATH_MULTIPLY:
                expression.kind = LN_VectorExpressionKind::Multiply;
                break;
              case blender::NODE_VECTOR_MATH_DIVIDE:
                expression.kind = LN_VectorExpressionKind::Divide;
                break;
              case blender::NODE_VECTOR_MATH_MINIMUM:
                expression.kind = LN_VectorExpressionKind::Minimum;
                break;
              case blender::NODE_VECTOR_MATH_MAXIMUM:
                expression.kind = LN_VectorExpressionKind::Maximum;
                break;
            }
            result = program.AddVectorExpression(expression);
            break;
          }
          case blender::NODE_VECTOR_MATH_ABSOLUTE:
            expression.kind = LN_VectorExpressionKind::Absolute;
            result = program.AddVectorExpression(expression);
            break;
          case blender::NODE_VECTOR_MATH_SCALE: {
            const std::optional<uint32_t> scale = BuildInputFloatExpression(program,
                                                                            node,
                                                                            "Scale",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            float_expression_cache);
            if (!scale) {
              break;
            }
            expression.kind = LN_VectorExpressionKind::Scale;
            expression.float_expr_index = *scale;
            expression.float_value = FloatExpressionConstantFallback(program, *scale);
            result = program.AddVectorExpression(expression);
            break;
          }
          case blender::NODE_VECTOR_MATH_NORMALIZE:
            expression.kind = LN_VectorExpressionKind::Normalize;
            result = program.AddVectorExpression(expression);
            break;
          default:
            break;
        }
        break;
      }
    default:
      break;
  }

  if (r_handled) {
    vector_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionColorOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    ColorExpressionCache &color_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = color_expression_cache.find(&output_socket);
  if (cache_iter != color_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
    case LN_NodeKind::GetLightColor:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind::SnapshotLightColor;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddColorExpression(expression);
      break;
    }
    case LN_NodeKind::GetObjectAttribute:
      r_handled = true;
      {
      r_handled = true;
      if (node.custom1 != 6 ||
          !NamesMatch(output_socket.name, output_socket.identifier, "Color"))
      {
        break;
      }
      BoolExpressionCache bool_expression_cache;
      IntExpressionCache int_expression_cache;
      StringExpressionCache string_expression_cache;
      VectorExpressionCache vector_expression_cache;
      ValueExpressionCache value_expression_cache;
      const std::optional<uint32_t> object_expr = BuildGetObjectAttributeObjectExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache,
          int_expression_cache,
          float_expression_cache,
          string_expression_cache,
          vector_expression_cache,
          color_expression_cache,
          value_expression_cache);
      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind::SnapshotObjectColor;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddColorExpression(expression);
      break;
    }
    case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Color, MakeDefaultValue(LN_ValueType::Color));
        LN_ColorExpression expression;
        expression.kind = LN_ColorExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddColorExpression(expression);
      }
      break;
    }    default:
      break;
  }

  if (r_handled) {
    color_expression_cache[&output_socket] = result;
  }
  return result;
}

std::optional<uint32_t> TryCompileExpressionValueOutput(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    BoolExpressionCache &bool_expression_cache,
    IntExpressionCache &int_expression_cache,
    FloatExpressionCache &float_expression_cache,
    StringExpressionCache &string_expression_cache,
    VectorExpressionCache &vector_expression_cache,
    ColorExpressionCache &color_expression_cache,
    ValueExpressionCache &value_expression_cache,
    bool &r_handled)
{
  r_handled = false;

  const auto cache_iter = value_expression_cache.find(&output_socket);
  if (cache_iter != value_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
    switch (definition.kind) {
    case LN_NodeKind::GetTreeProperty:
      r_handled = true;
      {
      r_handled = true;
      std::optional<LN_Value> name = ReadInputValue(node,
                                                    "Property",
                                                    LN_ValueType::String,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache);
      if (!name || name->string_value.empty()) {
        name = ReadInputValue(
            node, "Name", LN_ValueType::String, node_definitions, input_links, value_cache);
      }
      if (name && name->type == LN_ValueType::String && !name->string_value.empty()) {
        const uint32_t property_ref_index = AddTreePropertyRef(
            program, name->string_value, LN_ValueType::Float, MakeDefaultValue(LN_ValueType::Float));
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
        expression.property_ref_index = property_ref_index;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::GetActiveCamera:
      r_handled = true;
      {
      r_handled = true;
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ActiveCamera;
      result = program.AddValueExpression(expression);
      break;
    }case LN_NodeKind::GetParent:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Child Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache,
                                                                            int_expression_cache,
                                                                            float_expression_cache,
                                                                            string_expression_cache,
                                                                            vector_expression_cache,
                                                                            color_expression_cache,
                                                                            value_expression_cache);
      if (object_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::ObjectParent;
        expression.input0 = *object_expr;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::AddObject:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Added Object") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Object"))
      {
        break;
      }
      const uint32_t property_ref_index = AddTreePropertyRef(
          program,
          AddObjectResultPropertyName(node),
          LN_ValueType::ObjectRef,
          MakeDefaultValue(LN_ValueType::ObjectRef));
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::RuntimeTreeProperty;
      expression.property_ref_index = property_ref_index;
      result = program.AddValueExpression(expression);
      break;
    }
    case LN_NodeKind::RemoveParent:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Parent") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Parent Object"))
      {
        break;
      }
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Child Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache,
                                                                            int_expression_cache,
                                                                            float_expression_cache,
                                                                            string_expression_cache,
                                                                            vector_expression_cache,
                                                                            color_expression_cache,
                                                                            value_expression_cache);
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::ObjectParent;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      else {
        LN_ValueExpression owner_expression;
        owner_expression.kind = LN_ValueExpressionKind::OwnerObject;
        expression.input0 = program.AddValueExpression(owner_expression);
      }
      result = program.AddValueExpression(expression);
      break;
    }case LN_NodeKind::GetChild:
      r_handled = true;
      {
      r_handled = true;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Parent Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache,
                                                                            int_expression_cache,
                                                                            float_expression_cache,
                                                                            string_expression_cache,
                                                                            vector_expression_cache,
                                                                            color_expression_cache,
                                                                            value_expression_cache);
      const std::optional<uint32_t> index_expr = BuildInputIntExpression(program,
                                                                         node,
                                                                         "Index",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         int_expression_cache);
      if (object_expr && index_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::ObjectChild;
        expression.input0 = *object_expr;
        expression.input1 = *index_expr;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::MakeLightUnique:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Light")) {
        break;
      }
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache,
                                                                            int_expression_cache,
                                                                            float_expression_cache,
                                                                            string_expression_cache,
                                                                            vector_expression_cache,
                                                                            color_expression_cache,
                                                                            value_expression_cache);
      if (object_expr) {
        result = *object_expr;
      }
      else {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::OwnerObject;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::Raycast:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Picked Object") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Hit Object"))
      {
        break;
      }

      const std::optional<uint32_t> query_expr = BuildRaycastQueryExpression(program,
                                                                             node,
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             float_expression_cache,
                                                                             bool_expression_cache);
      if (query_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::PhysicsQueryObject;
        expression.input0 = *query_expr;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::RaycastAll:
      r_handled = true;
      {
      r_handled = true;
      BoolExpressionCache bool_expression_cache_local;
      const std::optional<uint32_t> query_expr = BuildRaycastAllQueryExpression(program,
                                                                                node,
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                float_expression_cache,
                                                                                bool_expression_cache_local);
      if (!query_expr) {
        break;
      }

      LN_ValueExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Hit Objects")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryObjects;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Points")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryPoints;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normals")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryNormals;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Distances")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryDistances;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Fractions")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryFractions;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Face Indices")) {
        program.AddRayQueryDetailRequirement(node.identifier,
                                             LN_RAY_QUERY_DETAIL_FACE_INDEX);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryFaceIndices;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Has UVs")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryHasUVs;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "UVs")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryUVs;
      }
      else {
        break;
      }
      result = program.AddValueExpression(expression);
      break;
    }case LN_NodeKind::ShapeCast:
      r_handled = true;
      {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Hit Object")) {
        break;
      }
      const std::optional<uint32_t> query_expr = BuildShapeCastQueryExpression(program,
                                                                               node,
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               float_expression_cache,
                                                                               bool_expression_cache);
      if (query_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::PhysicsQueryObject;
        expression.input0 = *query_expr;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::ShapeCastAll:
      r_handled = true;
      {
      BoolExpressionCache bool_cache;
      const std::optional<uint32_t> query_expr = BuildShapeCastAllQueryExpression(
          program,
          node,
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          bool_cache);
      if (!query_expr) {
        break;
      }
      LN_ValueExpression expression;
      expression.input0 = *query_expr;
      if (NamesMatch(output_socket.name, output_socket.identifier, "Hit Objects")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryObjects;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Points")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryPoints;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Normals")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryNormals;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Cast Positions")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryCastPositions;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Distances")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryDistances;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Fractions")) {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryFractions;
      }
      else if (NamesMatch(output_socket.name,
                          output_socket.identifier,
                          "Started Overlapping"))
      {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList;
      }
      else if (NamesMatch(output_socket.name,
                          output_socket.identifier,
                          "Penetration Depths"))
      {
        expression.kind = LN_ValueExpressionKind::PhysicsQueryPenetrationDepths;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Face Indices")) {
        program.AddRayQueryDetailRequirement(node.identifier,
                                             LN_RAY_QUERY_DETAIL_FACE_INDEX);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryFaceIndices;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Has UVs")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryHasUVs;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "UVs")) {
        program.AddRayQueryDetailRequirement(
            node.identifier, LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
        expression.kind = LN_ValueExpressionKind::PhysicsQueryUVs;
      }
      else {
        break;
      }
      result = program.AddValueExpression(expression);
      break;
    }case LN_NodeKind::MouseRay:
    case LN_NodeKind::CameraRay:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Picked Object")) {
        break;
      }

      const std::optional<uint32_t> query_expr =
          definition.kind == LN_NodeKind::MouseRay ?
              BuildMouseRayQueryExpression(program,
                                           node,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_expression_cache) :
              BuildCameraRayQueryExpression(program,
                                            node,
                                            node_definitions,
                                            input_links,
                                            value_cache,
                                            float_expression_cache,
                                            bool_expression_cache);
      if (query_expr) {
        LN_ValueExpression expression;
        expression.kind = LN_ValueExpressionKind::PhysicsQueryObject;
        expression.input0 = *query_expr;
        result = program.AddValueExpression(expression);
      }
      break;
    }case LN_NodeKind::ValueChanged:
      r_handled = true;
      {
      r_handled = true;
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Old") &&
          !NamesMatch(output_socket.name, output_socket.identifier, "New"))
      {
        break;
      }

      const blender::bNodeSocket *changed_socket = nullptr;
      for (const blender::bNodeSocket *sock =
               static_cast<const blender::bNodeSocket *>(node.outputs.first);
           sock != nullptr;
           sock = sock->next)
      {
        if (NamesMatch(sock->name, sock->identifier, "If Changed") ||
            STREQ(sock->identifier, "Out"))
        {
          changed_socket = sock;
          break;
        }
      }
      if (changed_socket == nullptr) {
        break;
      }

      bool changed_handled = false;
      const std::optional<uint32_t> changed_expr = TryCompileExpressionBoolOutput(
          program,
          node,
          *changed_socket,
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          bool_expression_cache,
          changed_handled);
      if (!changed_expr) {
        break;
      }

      LN_ValueExpression expression;
      expression.kind = NamesMatch(output_socket.name, output_socket.identifier, "Old") ?
                             LN_ValueExpressionKind::ValueChangedOld :
                             LN_ValueExpressionKind::ValueChangedNew;
      expression.input0 = *changed_expr;
      result = program.AddValueExpression(expression);
      break;
    }    default:
      break;
  }

  if (r_handled) {
    value_expression_cache[&output_socket] = result;
  }
  return result;
}

bool TryCompileExpressionOutputByKind(LN_Program &program,
                                      const blender::bNode &node,
                                      const blender::bNodeSocket &output_socket,
                                      const LN_NodeDefinition &definition,
                                      const NodeDefinitionMap &node_definitions,
                                      const InputLinkMap &input_links,
                                      ValueCache &value_cache,
                                      FloatExpressionCache &float_expression_cache,
                                      BoolExpressionCache &bool_expression_cache,
                                      IntExpressionCache &int_expression_cache,
                                      StringExpressionCache &string_expression_cache,
                                      VectorExpressionCache &vector_expression_cache,
                                      ColorExpressionCache &color_expression_cache,
                                      ValueExpressionCache &value_expression_cache,
                                      std::optional<uint32_t> &r_result)
{
  bool handled = false;
  switch (output_socket.type) {
    case blender::SOCK_BOOLEAN:
      r_result = TryCompileExpressionBoolOutput(program,
                                              node,
                                              output_socket,
                                              node_definitions,
                                              input_links,
                                              value_cache,
                                              float_expression_cache,
                                              bool_expression_cache,
                                              handled);
      return handled;
    case blender::SOCK_INT:
      r_result = TryCompileExpressionIntOutput(program,
                                             node,
                                             output_socket,
                                             node_definitions,
                                             input_links,
                                             value_cache,
                                             int_expression_cache,
                                             &bool_expression_cache,
                                             &float_expression_cache,
                                             &string_expression_cache,
                                             &vector_expression_cache,
                                             &color_expression_cache,
                                             &value_expression_cache,
                                             handled);
      return handled;
    case blender::SOCK_FLOAT:
      r_result = TryCompileExpressionFloatOutput(program,
                                               node,
                                               output_socket,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               float_expression_cache,
                                               &bool_expression_cache,
                                               &vector_expression_cache,
                                               handled);
      return handled;
    case blender::SOCK_STRING:
      r_result = TryCompileExpressionStringOutput(program,
                                                  node,
                                                  output_socket,
                                                  node_definitions,
                                                  input_links,
                                                  value_cache,
                                                  string_expression_cache,
                                                  handled);
      return handled;
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION:
    case blender::SOCK_RGBA:
      if (output_socket.type != blender::SOCK_RGBA) {
        r_result = TryCompileExpressionVectorOutput(program,
                                                    node,
                                                    output_socket,
                                                    node_definitions,
                                                    input_links,
                                                    value_cache,
                                                    float_expression_cache,
                                                    vector_expression_cache,
                                                    handled);
      }
      else {
        r_result = TryCompileExpressionColorOutput(program,
                                                   node,
                                                   output_socket,
                                                   node_definitions,
                                                   input_links,
                                                   value_cache,
                                                   float_expression_cache,
                                                   color_expression_cache,
                                                   handled);
      }
      return handled;
    default:
      r_result = TryCompileExpressionValueOutput(program,
                                               node,
                                               output_socket,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               bool_expression_cache,
                                               int_expression_cache,
                                               float_expression_cache,
                                               string_expression_cache,
                                               vector_expression_cache,
                                               color_expression_cache,
                                               value_expression_cache,
                                               handled);
      return handled;
  }
  return false;
}

bool TryCompileRegisteredExpressionOutput(LN_Program &program,
                                            const blender::bNode &node,
                                            const blender::bNodeSocket &output_socket,
                                            const LN_NodeDefinition &definition,
                                            const NodeDefinitionMap &node_definitions,
                                            const InputLinkMap &input_links,
                                            ValueCache &value_cache,
                                            FloatExpressionCache &float_expression_cache,
                                            BoolExpressionCache &bool_expression_cache,
                                            IntExpressionCache &int_expression_cache,
                                            StringExpressionCache &string_expression_cache,
                                            VectorExpressionCache &vector_expression_cache,
                                            ColorExpressionCache &color_expression_cache,
                                            ValueExpressionCache &value_expression_cache,
                                            std::optional<uint32_t> &r_result)
{
  return TryCompileExpressionOutputByKind(program,
                                          node,
                                          output_socket,
                                          definition,
                                          node_definitions,
                                          input_links,
                                          value_cache,
                                          float_expression_cache,
                                          bool_expression_cache,
                                          int_expression_cache,
                                          string_expression_cache,
                                          vector_expression_cache,
                                          color_expression_cache,
                                          value_expression_cache,
                                          r_result);
}

}  // namespace ln_compiler
