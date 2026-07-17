/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler_output_expressions.cc
 *  \ingroup logicnodes
 *
 * Generic expression output lowering: TryCompile delegate, constant/math/loop
 * switches, EvaluateOutputValue, and BuildInput* helpers. Per-kind expression
 * nodes live in LN_TreeCompiler_expression_outputs_by_kind.cc.
 */

#include "../LN_TreeCompiler_internal.hh"

#include <algorithm>
#include <cmath>

#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "SCA_InputEvent.h"
#include "SCA_JoystickSensor.h"

#include "BL_Action.h"

namespace ln_compiler {

static constexpr int material_node_value_per_object_only = 1 << 0;

static bool IsSupportedBoneAttribute(const int attribute)
{
  switch (attribute) {
    case 0:
    case 1:
    case 2:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
      return true;
    default:
      return false;
  }
}

static std::optional<LN_Value> ReadGenericSocketDefault(const blender::bNodeSocket &socket)
{
  switch (socket.type) {
    case blender::SOCK_BOOLEAN:
      return ReadSocketDefault(socket, LN_ValueType::Bool);
    case blender::SOCK_INT:
      return ReadSocketDefault(socket, LN_ValueType::Int);
    case blender::SOCK_FLOAT:
      return ReadSocketDefault(socket, LN_ValueType::Float);
    case blender::SOCK_VECTOR:
      return ReadSocketDefault(socket, LN_ValueType::Vector);
    case blender::SOCK_ROTATION:
      return ReadSocketDefault(socket, LN_ValueType::Rotation);
    case blender::SOCK_RGBA:
      return ReadSocketDefault(socket, LN_ValueType::Color);
    case blender::SOCK_STRING:
      return ReadSocketDefault(socket, LN_ValueType::String);
    case blender::SOCK_OBJECT:
      return ReadSocketDefault(socket, LN_ValueType::ObjectRef);
    default:
      return std::nullopt;
  }
}

static std::optional<uint32_t> BuildBoneAttributeValueExpression(
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
  const int attribute = int(node.custom1);
  if (!IsSupportedBoneAttribute(attribute)) {
    return std::nullopt;
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
  const std::optional<uint32_t> bone_name_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Bone Name",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  if (!bone_name_expr) {
    return std::nullopt;
  }
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::BoneAttribute;
  expression.property_ref_index = uint32_t(attribute);
  if ((node.custom2 & 1) != 0) {
    expression.property_ref_index |= LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG;
  }
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.string_expr_index = *bone_name_expr;
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildBonePoseRotationValueExpression(
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
  const std::optional<uint32_t> bone_name_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Bone Name",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  if (!bone_name_expr) {
    return std::nullopt;
  }
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::BonePoseRotation;
  expression.property_ref_index = uint32_t(std::clamp(int(node.custom1),
                                                      int(LN_BonePoseRotationSpace::PoseChannel),
                                                      int(LN_BonePoseRotationSpace::World)));
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.string_expr_index = *bone_name_expr;
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildMaterialSlotValueExpression(
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
  const std::optional<uint32_t> slot_expr = BuildInputIntExpression(program,
                                                                    node,
                                                                    "Slot",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    int_expression_cache);
  if (!slot_expr) {
    return std::nullopt;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::MaterialSlot;
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.input1 = *slot_expr;
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildMaterialSlotFoundExpression(
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
  const std::optional<uint32_t> slot_expr = BuildInputIntExpression(program,
                                                                    node,
                                                                    "Slot",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    int_expression_cache);
  if (!slot_expr) {
    return std::nullopt;
  }

  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::MaterialSlotFound;
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.input1 = *slot_expr;
  return program.AddBoolExpression(expression);
}

static std::optional<uint32_t> BuildMaterialNodeValueExpression(
    LN_Program &program,
    const blender::bNode &node,
    const bool material_parameter,
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
  using blender::ID_Type;

  const std::optional<uint32_t> node_name_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Node Name",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  const std::optional<uint32_t> socket_name_expr = BuildInputStringExpression(program,
                                                                             node,
                                                                             "Socket",
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             string_expression_cache);
  if (!node_name_expr || !socket_name_expr) {
    return std::nullopt;
  }

  const bool per_object_only = material_parameter ||
                               ((node.custom2 & material_node_value_per_object_only) != 0);
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::MaterialNodeValue;
  expression.property_ref_index = material_parameter ? 2u : (per_object_only ? 4u : 3u);
  expression.string_expr_index = *node_name_expr;
  expression.input2 = *socket_name_expr;

  if (per_object_only) {
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
    const std::optional<uint32_t> slot_expr = BuildInputIntExpression(program,
                                                                      node,
                                                                      "Slot",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      int_expression_cache);
    if (!slot_expr) {
      return std::nullopt;
    }
    if (object_expr) {
      expression.input0 = *object_expr;
    }
    expression.input1 = *slot_expr;
  }
  else {
    std::optional<uint32_t> material_expr = BuildInputValueExpression(program,
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
    if (!material_expr && node.id != nullptr && GS(node.id->name) == blender::ID_MA) {
      LN_Value material_value;
      material_value.type = LN_ValueType::DatablockRef;
      material_value.exists = true;
      material_value.reference_name = node.id->name + 2;
      material_expr = AddConstantValueExpression(program, material_value);
    }
    if (!material_expr) {
      return std::nullopt;
    }
    expression.input0 = *material_expr;
  }

  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildMaterialNodeValueFoundExpression(
    LN_Program &program,
    const blender::bNode &node,
    const bool material_parameter,
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
  using blender::ID_Type;

  const std::optional<uint32_t> node_name_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Node Name",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  const std::optional<uint32_t> socket_name_expr = BuildInputStringExpression(program,
                                                                             node,
                                                                             "Socket",
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             string_expression_cache);
  if (!node_name_expr || !socket_name_expr) {
    return std::nullopt;
  }

  const bool per_object_only = material_parameter ||
                               ((node.custom2 & material_node_value_per_object_only) != 0);
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::MaterialNodeValueFound;
  expression.property_ref_index = material_parameter ? 2u : (per_object_only ? 4u : 3u);
  expression.input2 = *node_name_expr;
  expression.int_expr_index = *socket_name_expr;

  if (per_object_only) {
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
    const std::optional<uint32_t> slot_expr = BuildInputIntExpression(program,
                                                                      node,
                                                                      "Slot",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      int_expression_cache);
    if (!slot_expr) {
      return std::nullopt;
    }
    if (object_expr) {
      expression.input0 = *object_expr;
    }
    expression.input1 = *slot_expr;
  }
  else {
    std::optional<uint32_t> material_expr = BuildInputValueExpression(program,
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
    if (!material_expr && node.id != nullptr && GS(node.id->name) == blender::ID_MA) {
      LN_Value material_value;
      material_value.type = LN_ValueType::DatablockRef;
      material_value.exists = true;
      material_value.reference_name = node.id->name + 2;
      material_expr = AddConstantValueExpression(program, material_value);
    }
    if (!material_expr) {
      return std::nullopt;
    }
    expression.input0 = *material_expr;
  }

  return program.AddBoolExpression(expression);
}

enum class EditorNodeValueTarget : uint32_t {
  GeometryModifier = 1,
  CompositorScene = 2,
  CompositorTree = 3,
};

struct EditorNodeValueInputs {
  EditorNodeValueTarget target = EditorNodeValueTarget::GeometryModifier;
  uint32_t object_expr = LN_INVALID_INDEX;
  uint32_t target_expr = LN_INVALID_INDEX;
  uint32_t node_expr = LN_INVALID_INDEX;
  uint32_t socket_expr = LN_INVALID_INDEX;
};

static std::optional<EditorNodeValueInputs> BuildEditorNodeValueInputs(
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
  using blender::ID_Type;

  EditorNodeValueInputs inputs;
  const std::optional<uint32_t> node_expr = BuildInputStringExpression(program,
                                                                       node,
                                                                       "Node Name",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       string_expression_cache);
  const std::optional<uint32_t> socket_expr = BuildInputStringExpression(program,
                                                                         node,
                                                                         "Socket",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         string_expression_cache);
  if (!node_expr || !socket_expr) {
    return std::nullopt;
  }
  inputs.node_expr = *node_expr;
  inputs.socket_expr = *socket_expr;

  if (node.custom1 == 2) {
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
    const std::optional<uint32_t> modifier_expr = BuildInputStringExpression(program,
                                                                             node,
                                                                             "Modifier",
                                                                             node_definitions,
                                                                             input_links,
                                                                             value_cache,
                                                                             string_expression_cache);
    if (!modifier_expr) {
      return std::nullopt;
    }
    inputs.target = EditorNodeValueTarget::GeometryModifier;
    inputs.object_expr = object_expr.value_or(LN_INVALID_INDEX);
    inputs.target_expr = *modifier_expr;
    return inputs;
  }

  if (node.custom1 != 3 || node.id == nullptr ||
      !ELEM(GS(node.id->name), blender::ID_SCE, blender::ID_NT) ||
      (node.id->tag & blender::ID_TAG_MISSING) != 0)
  {
    return std::nullopt;
  }
  const blender::bNodeTree *tree = GS(node.id->name) == blender::ID_SCE ?
                                       reinterpret_cast<const blender::Scene *>(node.id)
                                           ->compositing_node_group :
                                       reinterpret_cast<const blender::bNodeTree *>(node.id);
  if (tree == nullptr || tree->type != blender::NTREE_COMPOSIT) {
    return std::nullopt;
  }
  inputs.target = GS(node.id->name) == blender::ID_SCE ?
                      EditorNodeValueTarget::CompositorScene :
                      EditorNodeValueTarget::CompositorTree;
  inputs.target_expr = AddConstantStringExpression(program, node.id->name + 2);
  return inputs;
}

static std::optional<uint32_t> BuildEditorNodeValueExpression(
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
  const std::optional<EditorNodeValueInputs> inputs = BuildEditorNodeValueInputs(
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
  if (!inputs) {
    return std::nullopt;
  }
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::EditorNodeValue;
  expression.property_ref_index = uint32_t(inputs->target);
  expression.input0 = inputs->object_expr;
  expression.input1 = inputs->target_expr;
  expression.string_expr_index = inputs->node_expr;
  expression.input2 = inputs->socket_expr;
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildEditorNodeValueFoundExpression(
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
  const std::optional<EditorNodeValueInputs> inputs = BuildEditorNodeValueInputs(
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
  if (!inputs) {
    return std::nullopt;
  }
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::EditorNodeValueFound;
  expression.property_ref_index = uint32_t(inputs->target);
  expression.input0 = inputs->object_expr;
  expression.input1 = inputs->target_expr;
  expression.input2 = inputs->node_expr;
  expression.int_expr_index = inputs->socket_expr;
  return program.AddBoolExpression(expression);
}

static std::optional<uint32_t> BuildCombineVector4ValueExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    BoolExpressionCache &bool_expression_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache)
{
  const std::optional<uint32_t> x = BuildInputFloatExpression(program,
                                                              node,
                                                              "X",
                                                              node_definitions,
                                                              input_links,
                                                              value_cache,
                                                              float_expression_cache,
                                                              &bool_expression_cache,
                                                              &vector_expression_cache);
  const std::optional<uint32_t> y = BuildInputFloatExpression(program,
                                                              node,
                                                              "Y",
                                                              node_definitions,
                                                              input_links,
                                                              value_cache,
                                                              float_expression_cache,
                                                              &bool_expression_cache,
                                                              &vector_expression_cache);
  const std::optional<uint32_t> z = BuildInputFloatExpression(program,
                                                              node,
                                                              "Z",
                                                              node_definitions,
                                                              input_links,
                                                              value_cache,
                                                              float_expression_cache,
                                                              &bool_expression_cache,
                                                              &vector_expression_cache);
  const std::optional<uint32_t> w = BuildInputFloatExpression(program,
                                                              node,
                                                              "W",
                                                              node_definitions,
                                                              input_links,
                                                              value_cache,
                                                              float_expression_cache,
                                                              &bool_expression_cache,
                                                              &vector_expression_cache);
  if (!x || !y || !z || !w) {
    return std::nullopt;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::CombineVector4;
  expression.input_indices = {*x, *y, *z, *w};
  expression.value.type = LN_ValueType::Vector4;
  expression.value.exists = true;
  expression.value.vector4_value = MT_Vector4(FloatExpressionConstantFallback(program, *x),
                                              FloatExpressionConstantFallback(program, *y),
                                              FloatExpressionConstantFallback(program, *z),
                                              FloatExpressionConstantFallback(program, *w));
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildResizeVectorValueExpression(
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
  const std::optional<uint32_t> vector = BuildInputValueExpression(program,
                                                                  node,
                                                                  "Vector",
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
  if (!vector) {
    return std::nullopt;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ResizeVectorValue;
  expression.input0 = *vector;
  expression.property_ref_index = uint32_t(std::clamp(int(node.custom1) + 2, 2, 4));
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildEulerToMatrixValueExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache)
{
  const std::optional<uint32_t> xyz = BuildInputVectorExpression(program,
                                                                node,
                                                                "XYZ",
                                                                node_definitions,
                                                                input_links,
                                                                value_cache,
                                                                float_expression_cache,
                                                                vector_expression_cache);
  if (!xyz) {
    return std::nullopt;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::EulerToMatrix;
  expression.input0 = *xyz;
  expression.property_ref_index = 0;
  return program.AddValueExpression(expression);
}

static std::optional<uint32_t> BuildMatrixToEulerValueExpression(
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
  const std::optional<uint32_t> matrix = BuildInputValueExpression(program,
                                                                  node,
                                                                  "Matrix",
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
  if (!matrix) {
    return std::nullopt;
  }

  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::MatrixToEuler;
  expression.input0 = *matrix;
  expression.property_ref_index = uint32_t(std::clamp(int(node.custom2), 0, 5));
  expression.value.type = node.custom1 == 1 ? LN_ValueType::Rotation : LN_ValueType::Vector;
  expression.value.exists = true;
  return program.AddValueExpression(expression);
}

std::optional<uint32_t> BuildOutputBoolExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const auto cache_iter = bool_expression_cache.find(&output_socket);
  if (cache_iter != bool_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    bool_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  IntExpressionCache try_int_cache;
  StringExpressionCache try_string_cache;
  VectorExpressionCache try_vector_cache;
  ColorExpressionCache try_color_cache;
  ValueExpressionCache try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            bool_expression_cache,
            try_int_cache,
            float_expression_cache,
            try_string_cache,
            try_vector_cache,
            try_color_cache,
            try_value_cache))
    {
      LN_BoolExpression expression;
      expression.kind = LN_BoolExpressionKind::FromGenericValue;
      expression.input0 = *value_expr;
      result = program.AddBoolExpression(expression);
    }
    bool_expression_cache[&output_socket] = result;
    return result;
  }
  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_expression_cache,
                                           try_int_cache,
                                           try_string_cache,
                                           try_vector_cache,
                                           try_color_cache,
                                           try_value_cache,
                                           try_result))
  {
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::ValueBool: {
      const std::optional<LN_Value> value = ReadSocketDefault(output_socket, LN_ValueType::Bool);
      if (value && value->type == LN_ValueType::Bool) {
        result = AddConstantBoolExpression(program, value->bool_value);
      }
      break;
    }
    case LN_NodeKind::Loop:
    case LN_NodeKind::LoopFromList:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Loop")) {
        if (const auto cached = bool_expression_cache.find(&output_socket);
            cached != bool_expression_cache.end())
        {
          result = cached->second;
        }
      }
      break;
    case LN_NodeKind::GetBoneAttribute: {
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      if (const std::optional<uint32_t> value_expr = BuildBoneAttributeValueExpression(
              program,
              node,
              node_definitions,
              input_links,
              value_cache,
              bool_expression_cache,
              int_expression_cache_local,
              float_expression_cache,
              string_expression_cache_local,
              vector_expression_cache_local,
              color_expression_cache_local,
              value_expression_cache_local))
      {
        LN_BoolExpression expression;
        expression.kind = LN_BoolExpressionKind::FromGenericValue;
        expression.input0 = *value_expr;
        result = program.AddBoolExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetMaterialFromSlot:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Found")) {
        result = BuildMaterialSlotFoundExpression(program,
                                                  node,
                                                  node_definitions,
                                                  input_links,
                                                  value_cache,
                                                  bool_expression_cache,
                                                  try_int_cache,
                                                  float_expression_cache,
                                                  try_string_cache,
                                                  try_vector_cache,
                                                  try_color_cache,
                                                  try_value_cache);
      }
      break;
    case LN_NodeKind::GetMaterialParameter:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Found")) {
        result = BuildMaterialNodeValueFoundExpression(program,
                                                       node,
                                                       definition.kind ==
                                                           LN_NodeKind::GetMaterialParameter,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache,
                                                       bool_expression_cache,
                                                       try_int_cache,
                                                       float_expression_cache,
                                                       try_string_cache,
                                                       try_vector_cache,
                                                       try_color_cache,
                                                       try_value_cache);
      }
      break;
    case LN_NodeKind::GetEditorNodeValue:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Found")) {
        result = node.custom1 == 1 ?
                     BuildMaterialNodeValueFoundExpression(program,
                                                           node,
                                                           false,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache,
                                                           bool_expression_cache,
                                                           try_int_cache,
                                                           float_expression_cache,
                                                           try_string_cache,
                                                           try_vector_cache,
                                                           try_color_cache,
                                                           try_value_cache) :
                     BuildEditorNodeValueFoundExpression(program,
                                                         node,
                                                         node_definitions,
                                                         input_links,
                                                         value_cache,
                                                         bool_expression_cache,
                                                         try_int_cache,
                                                         float_expression_cache,
                                                         try_string_cache,
                                                         try_vector_cache,
                                                         try_color_cache,
                                                         try_value_cache);
      }
      break;
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::Bool) {
        result = AddConstantBoolExpression(program, value->bool_value);
      }
      break;
    }
  }

  bool_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildOutputIntExpression(
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
  ValueExpressionCache *value_expression_cache)
{
  const auto cache_iter = int_expression_cache.find(&output_socket);
  if (cache_iter != int_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    int_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  FloatExpressionCache try_float_cache;
  BoolExpressionCache try_bool_cache;
  StringExpressionCache try_string_cache;
  VectorExpressionCache try_vector_cache;
  ColorExpressionCache try_color_cache;
  ValueExpressionCache try_value_cache;
  std::optional<uint32_t> try_result;
  FloatExpressionCache &float_cache = float_expression_cache ? *float_expression_cache :
                                                              try_float_cache;
  BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                            try_bool_cache;
  StringExpressionCache &string_cache = string_expression_cache ? *string_expression_cache :
                                                                  try_string_cache;
  VectorExpressionCache &vector_cache = vector_expression_cache ? *vector_expression_cache :
                                                                  try_vector_cache;
  ColorExpressionCache &color_cache = color_expression_cache ? *color_expression_cache :
                                                               try_color_cache;
  ValueExpressionCache &value_expr_cache = value_expression_cache ? *value_expression_cache :
                                                                    try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            bool_cache,
            int_expression_cache,
            float_cache,
            string_cache,
            vector_cache,
            color_cache,
            value_expr_cache))
    {
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::FromGenericValue;
      expression.input0 = *value_expr;
      result = program.AddIntExpression(expression);
    }
    int_expression_cache[&output_socket] = result;
    return result;
  }
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_cache,
                                           bool_cache,
                                           int_expression_cache,
                                           string_cache,
                                           vector_cache,
                                           color_cache,
                                           value_expr_cache,
                                           try_result))
  {
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::ValueInt: {
      const std::optional<LN_Value> value = ReadSocketDefault(output_socket, LN_ValueType::Int);
      if (value && value->type == LN_ValueType::Int) {
        result = AddConstantIntExpression(program, value->int_value);
      }
      break;
    }
    case LN_NodeKind::StringOperation: {
      if (node.custom1 != LN_STRING_OP_COUNT ||
          !NamesMatch(output_socket.name, output_socket.identifier, "Count"))
      {
        break;
      }

      StringExpressionCache string_expression_cache;
      const std::optional<uint32_t> input0 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "String",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input1 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "Substring",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      if (input0 && input1) {
        LN_IntExpression expression;
        expression.kind = LN_IntExpressionKind::StringCount;
        expression.input0 = *input0;
        expression.input1 = *input1;
        result = program.AddIntExpression(expression);
      }
      break;
    }
    case LN_NodeKind::Loop:
    case LN_NodeKind::LoopFromList:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Index") &&
          g_active_loop_frame_cache != nullptr)
      {
        if (const auto iter = g_active_loop_frame_cache->index_expressions.find(&node);
            iter != g_active_loop_frame_cache->index_expressions.end())
        {
          result = iter->second;
        }
      }
      break;
    case LN_NodeKind::Collision:
    case LN_NodeKind::OnCollision: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Contact Count")) {
        break;
      }
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_cache,
                                                                            int_expression_cache,
                                                                            float_cache,
                                                                            try_string_cache,
                                                                            try_vector_cache,
                                                                            try_color_cache,
                                                                            try_value_cache);
      const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Property",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               try_string_cache);
      const std::optional<uint32_t> material_expr = BuildInputValueExpression(program,
                                                                              node,
                                                                              "Material",
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              bool_cache,
                                                                              int_expression_cache,
                                                                              float_cache,
                                                                              try_string_cache,
                                                                              try_vector_cache,
                                                                              try_color_cache,
                                                                              try_value_cache);
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
      break;
    }
    case LN_NodeKind::GetMaterialSlotCount: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Count")) {
        break;
      }
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
          try_string_cache,
          try_vector_cache,
          try_color_cache,
          try_value_cache);
      LN_IntExpression expression;
      expression.kind = LN_IntExpressionKind::MaterialSlotCount;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddIntExpression(expression);
      break;
    }
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::Int) {
        result = AddConstantIntExpression(program, value->int_value);
      }
      break;
    }
  }

  int_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildInputIntExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    IntExpressionCache &int_expression_cache,
    BoolExpressionCache *bool_expression_cache,
    FloatExpressionCache *float_expression_cache,
    StringExpressionCache *string_expression_cache,
    VectorExpressionCache *vector_expression_cache,
    ColorExpressionCache *color_expression_cache,
    ValueExpressionCache *value_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputIntExpression(program,
                                    *link.fromnode,
                                    *link.fromsock,
                                    node_definitions,
                                    input_links,
                                    value_cache,
                                    int_expression_cache,
                                    bool_expression_cache,
                                    float_expression_cache,
                                    string_expression_cache,
                                    vector_expression_cache,
                                    color_expression_cache,
                                    value_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Int);
  if (!default_value || default_value->type != LN_ValueType::Int) {
    return std::nullopt;
  }
  return AddConstantIntExpression(program, default_value->int_value);
}

std::optional<uint32_t> BuildInputFloatExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache *bool_expression_cache,
    VectorExpressionCache *vector_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputFloatExpression(program,
                                      *link.fromnode,
                                      *link.fromsock,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      bool_expression_cache,
                                      vector_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Float);
  if (!default_value || default_value->type != LN_ValueType::Float) {
    return std::nullopt;
  }
  return AddConstantFloatExpression(program, default_value->float_value);
}

std::optional<uint32_t> BuildOutputFloatExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache *bool_expression_cache,
    VectorExpressionCache *vector_expression_cache)
{
  BoolExpressionCache local_bool_expression_cache;
  BoolExpressionCache &bool_cache = bool_expression_cache ? *bool_expression_cache :
                                                           local_bool_expression_cache;
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
    float_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  IntExpressionCache try_int_cache;
  StringExpressionCache try_string_cache;
  ColorExpressionCache try_color_cache;
  ValueExpressionCache try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            bool_cache,
            try_int_cache,
            float_expression_cache,
            try_string_cache,
            vector_cache,
            try_color_cache,
            try_value_cache))
    {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::FromGenericValue;
      expression.input0 = *value_expr;
      result = program.AddFloatExpression(expression);
    }
    float_expression_cache[&output_socket] = result;
    return result;
  }
  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           bool_cache,
                                           try_int_cache,
                                           try_string_cache,
                                           vector_cache,
                                           try_color_cache,
                                           try_value_cache,
                                           try_result))
  {
    if (output_socket.type != blender::SOCK_FLOAT && try_result) {
      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::FromGenericValue;
      expression.input0 = *try_result;
      try_result = program.AddFloatExpression(expression);
    }
    float_expression_cache.emplace(&output_socket, try_result);
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::ValueFloat: {
      const std::optional<LN_Value> value = ReadSocketDefault(output_socket,
                                                              LN_ValueType::Float);
      if (value && value->type == LN_ValueType::Float) {
        result = AddConstantFloatExpression(program, value->float_value);
      }
      break;
    }
    case LN_NodeKind::InvertValue: {
      const std::optional<uint32_t> input = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &bool_cache,
                                                                      &vector_cache);
      if (!input) {
        break;
      }

      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::Negate;
      expression.input0 = *input;
      result = program.AddFloatExpression(expression);
      break;
    }
    case LN_NodeKind::ClampValue: {
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &bool_cache,
                                                                      &vector_cache);
      const std::optional<uint32_t> min_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Min",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          &bool_cache,
                                                                          &vector_cache);
      const std::optional<uint32_t> max_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Max",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          &bool_cache,
                                                                          &vector_cache);
      if (!value || !min_value || !max_value) {
        break;
      }

      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::Clamp;
      expression.input0 = *value;
      expression.input1 = *min_value;
      expression.input2 = *max_value;
      result = program.AddFloatExpression(expression);
      break;
    }
    case LN_NodeKind::MapRange: {
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &bool_cache,
                                                                      &vector_cache);
      const std::optional<uint32_t> from_min = BuildInputFloatExpression(program,
                                                                         node,
                                                                         "From Min",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         &bool_cache,
                                                                         &vector_cache);
      const std::optional<uint32_t> from_max = BuildInputFloatExpression(program,
                                                                         node,
                                                                         "From Max",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         &bool_cache,
                                                                         &vector_cache);
      const std::optional<uint32_t> to_min = BuildInputFloatExpression(program,
                                                                       node,
                                                                       "To Min",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       &bool_cache,
                                                                       &vector_cache);
      const std::optional<uint32_t> to_max = BuildInputFloatExpression(program,
                                                                       node,
                                                                       "To Max",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       &bool_cache,
                                                                       &vector_cache);
      if (!value || !from_min || !from_max || !to_min || !to_max) {
        break;
      }

      auto add_binary_expression = [&](const LN_FloatExpressionKind kind,
                                       const uint32_t input0,
                                       const uint32_t input1) -> uint32_t {
        LN_FloatExpression expression;
        expression.kind = kind;
        expression.input0 = input0;
        expression.input1 = input1;
        return program.AddFloatExpression(expression);
      };

      const uint32_t shifted_value = add_binary_expression(
          LN_FloatExpressionKind::Subtract, *value, *from_min);
      const uint32_t source_range = add_binary_expression(
          LN_FloatExpressionKind::Subtract, *from_max, *from_min);
      const uint32_t normalized_value = add_binary_expression(
          LN_FloatExpressionKind::Divide, shifted_value, source_range);
      const uint32_t target_range = add_binary_expression(
          LN_FloatExpressionKind::Subtract, *to_max, *to_min);
      const uint32_t scaled_value = add_binary_expression(
          LN_FloatExpressionKind::Multiply, normalized_value, target_range);
      uint32_t mapped_value = add_binary_expression(
          LN_FloatExpressionKind::Add, scaled_value, *to_min);

      if (node.custom1 != 0) {
        LN_FloatExpression clamp_expression;
        clamp_expression.kind = LN_FloatExpressionKind::Clamp;
        clamp_expression.input0 = mapped_value;
        clamp_expression.input1 = *to_min;
        clamp_expression.input2 = *to_max;
        mapped_value = program.AddFloatExpression(clamp_expression);
      }

      result = mapped_value;
      break;
    }
    case LN_NodeKind::Math: {
      const std::optional<uint32_t> input0 = BuildInputFloatExpression(program,
                                                                       node,
                                                                       "A",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       &bool_cache,
                                                                       &vector_cache);
      const std::optional<uint32_t> input1 = BuildInputFloatExpression(program,
                                                                       node,
                                                                       "B",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       &bool_cache,
                                                                       &vector_cache);
      if (!input0 || !input1) {
        break;
      }

      LN_FloatExpression expression;
      expression.input0 = *input0;
      expression.input1 = *input1;
      switch (node.custom1) {
        case blender::NODE_MATH_ADD:
          expression.kind = LN_FloatExpressionKind::Add;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_SUBTRACT:
          expression.kind = LN_FloatExpressionKind::Subtract;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_MULTIPLY:
          expression.kind = LN_FloatExpressionKind::Multiply;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_DIVIDE:
          expression.kind = LN_FloatExpressionKind::Divide;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_POWER:
          expression.kind = LN_FloatExpressionKind::Power;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_MINIMUM:
          expression.kind = LN_FloatExpressionKind::Minimum;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_MAXIMUM:
          expression.kind = LN_FloatExpressionKind::Maximum;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_ABSOLUTE:
          expression.kind = LN_FloatExpressionKind::Absolute;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_SIGN:
          expression.kind = LN_FloatExpressionKind::Sign;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_ROUND:
          expression.kind = LN_FloatExpressionKind::Round;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_FLOOR:
          expression.kind = LN_FloatExpressionKind::Floor;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_CEIL:
          expression.kind = LN_FloatExpressionKind::Ceil;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_TRUNC:
          expression.kind = LN_FloatExpressionKind::Truncate;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_FRACTION:
          expression.kind = LN_FloatExpressionKind::Fraction;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_MODULO:
          expression.kind = LN_FloatExpressionKind::Modulo;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_SINE:
          expression.kind = LN_FloatExpressionKind::Sine;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_COSINE:
          expression.kind = LN_FloatExpressionKind::Cosine;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_RADIANS:
          expression.kind = LN_FloatExpressionKind::Radians;
          result = program.AddFloatExpression(expression);
          break;
        case blender::NODE_MATH_DEGREES:
          expression.kind = LN_FloatExpressionKind::Degrees;
          result = program.AddFloatExpression(expression);
          break;
        default:
          break;
      }
      break;
    }
    case LN_NodeKind::SeparateXY:
    case LN_NodeKind::SeparateXYZ:
    case LN_NodeKind::SeparateEuler: {
      const std::optional<uint32_t> vector =
          definition.kind == LN_NodeKind::SeparateEuler ?
              BuildInputRotationVectorExpression(program,
                                                 node,
                                                 "Rotation",
                                                 node_definitions,
                                                 input_links,
                                                 value_cache,
                                                 float_expression_cache,
                                                 vector_cache) :
              BuildInputVectorExpression(program,
                                         node,
                                         "Vector",
                                         node_definitions,
                                         input_links,
                                         value_cache,
                                         float_expression_cache,
                                         vector_cache);
      if (!vector) {
        break;
      }

      if (NamesMatch(output_socket.name, output_socket.identifier, "X")) {
        result = AddVectorComponentFloatExpression(program, *vector, 0);
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Y")) {
        result = AddVectorComponentFloatExpression(program, *vector, 1);
      }
      else if ((definition.kind == LN_NodeKind::SeparateXYZ ||
                definition.kind == LN_NodeKind::SeparateEuler) &&
               NamesMatch(output_socket.name, output_socket.identifier, "Z"))
      {
        result = AddVectorComponentFloatExpression(program, *vector, 2);
      }
      break;
    }
    case LN_NodeKind::Threshold: {
      const std::optional<uint32_t> else_zero = BuildInputBoolExpression(program,
                                                                         node,
                                                                         "Else 0",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         bool_cache);
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &bool_cache,
                                                                      &vector_cache);
      const std::optional<uint32_t> threshold = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Threshold",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          &bool_cache,
                                                                          &vector_cache);
      const std::optional<LN_ThresholdOperation> operation = ThresholdOperationFromCustom1(
          node.custom1);
      if (!else_zero || !value || !threshold || !operation) {
        break;
      }

      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::Threshold;
      expression.input0 = *value;
      expression.input1 = *threshold;
      expression.bool_expr_index = *else_zero;
      expression.threshold_operation = *operation;
      expression.bool_value = true;
      result = program.AddFloatExpression(expression);
      break;
    }
    case LN_NodeKind::RangedThreshold: {
      const std::optional<uint32_t> value = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Value",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &bool_cache,
                                                                      &vector_cache);
      const std::optional<uint32_t> min_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Min",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          &bool_cache,
                                                                          &vector_cache);
      const std::optional<uint32_t> max_value = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Max",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache,
                                                                          &bool_cache,
                                                                          &vector_cache);
      const std::optional<LN_RangeOperation> operation = RangeOperationFromCustom1(node.custom1);
      if (!value || !min_value || !max_value || !operation) {
        break;
      }

      LN_FloatExpression expression;
      expression.kind = LN_FloatExpressionKind::RangedThreshold;
      expression.input0 = *value;
      expression.input1 = *min_value;
      expression.input2 = *max_value;
      expression.range_operation = *operation;
      result = program.AddFloatExpression(expression);
      break;
    }
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::Float) {
        result = AddConstantFloatExpression(program, value->float_value);
      }
      break;
    }
  }

  float_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildInputStringExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    StringExpressionCache &string_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputStringExpression(program,
                                       *link.fromnode,
                                       *link.fromsock,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       string_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket,
                                                                  LN_ValueType::String);
  if (!default_value || default_value->type != LN_ValueType::String) {
    return std::nullopt;
  }
  return AddConstantStringExpression(program, default_value->string_value);
}

std::optional<uint32_t> BuildOutputStringExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    StringExpressionCache &string_expression_cache)
{
  const auto cache_iter = string_expression_cache.find(&output_socket);
  if (cache_iter != string_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    string_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  FloatExpressionCache try_float_cache;
  BoolExpressionCache try_bool_cache;
  IntExpressionCache try_int_cache;
  VectorExpressionCache try_vector_cache;
  ColorExpressionCache try_color_cache;
  ValueExpressionCache try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            try_bool_cache,
            try_int_cache,
            try_float_cache,
            string_expression_cache,
            try_vector_cache,
            try_color_cache,
            try_value_cache))
    {
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind::FromGenericValue;
      expression.value_expr_index = *value_expr;
      result = program.AddStringExpression(expression);
    }
    string_expression_cache[&output_socket] = result;
    return result;
  }
  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           try_float_cache,
                                           try_bool_cache,
                                           try_int_cache,
                                           string_expression_cache,
                                           try_vector_cache,
                                           try_color_cache,
                                           try_value_cache,
                                           try_result))
  {
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::Typecast: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "String")) {
        break;
      }
      FloatExpressionCache float_expression_cache_local;
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      if (const std::optional<uint32_t> value_expr = BuildInputValueExpression(
              program,
              node,
              "Value",
              node_definitions,
              input_links,
              value_cache,
              bool_expression_cache_local,
              int_expression_cache_local,
              float_expression_cache_local,
              string_expression_cache,
              vector_expression_cache_local,
              color_expression_cache_local,
              value_expression_cache_local))
      {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::FromGenericValue;
        expression.value_expr_index = *value_expr;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetBoneAttribute: {
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      if (const std::optional<uint32_t> value_expr = BuildBoneAttributeValueExpression(
              program,
              node,
              node_definitions,
              input_links,
              value_cache,
              bool_expression_cache_local,
              int_expression_cache_local,
              float_expression_cache_local,
              string_expression_cache,
              vector_expression_cache_local,
              color_expression_cache_local,
              value_expression_cache_local))
      {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::FromGenericValue;
        expression.value_expr_index = *value_expr;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    case LN_NodeKind::FilePath: {
      result = BuildInputStringExpression(program,
                                          node,
                                          "Value",
                                          node_definitions,
                                          input_links,
                                          value_cache,
                                          string_expression_cache);
      break;
    }
    case LN_NodeKind::GetMasterFolder: {
      const std::optional<uint32_t> name = BuildInputStringExpression(program,
                                                                      node,
                                                                      "Name",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      string_expression_cache);
      if (name) {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::MasterFolder;
        expression.input0 = *name;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetObjectID: {
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
                                                                            node,
                                                                            "Object",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            bool_expression_cache_local,
                                                                            int_expression_cache_local,
                                                                            float_expression_cache_local,
                                                                            string_expression_cache,
                                                                            vector_expression_cache_local,
                                                                            color_expression_cache_local,
                                                                            value_expression_cache_local);
      LN_StringExpression expression;
      expression.kind = LN_StringExpressionKind::ObjectID;
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddStringExpression(expression);
      break;
    }
    case LN_NodeKind::GetMaterialName: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "Name")) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      FloatExpressionCache float_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> material_expr = BuildInputValueExpression(program,
                                                                              node,
                                                                              "Material",
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              bool_expression_cache_local,
                                                                              int_expression_cache_local,
                                                                              float_expression_cache_local,
                                                                              string_expression_cache,
                                                                              vector_expression_cache_local,
                                                                              color_expression_cache_local,
                                                                              value_expression_cache_local);
      if (material_expr) {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::MaterialName;
        expression.input0 = *material_expr;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    case LN_NodeKind::JoinPath: {
      const std::optional<uint32_t> input0 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "Path",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input1 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "Path_001",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      if (input0 && input1) {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::Join;
        expression.input0 = *input0;
        expression.input1 = *input1;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    case LN_NodeKind::StringOperation: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "String")) {
        break;
      }

      const std::optional<uint32_t> input0 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "String",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      if (!input0) {
        break;
      }

      LN_StringExpression expression;
      expression.input0 = *input0;
      switch (node.custom1) {
        case LN_STRING_OP_JOIN: {
          const std::optional<uint32_t> input1 = BuildInputStringExpression(program,
                                                                            node,
                                                                            "Substring",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            string_expression_cache);
          if (!input1) {
            break;
          }
          expression.kind = LN_StringExpressionKind::Join;
          expression.input1 = *input1;
          result = program.AddStringExpression(expression);
          break;
        }
        case LN_STRING_OP_REPLACE: {
          const std::optional<uint32_t> input1 = BuildInputStringExpression(program,
                                                                            node,
                                                                            "Substring",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            string_expression_cache);
          const std::optional<uint32_t> input2 = BuildInputStringExpression(program,
                                                                            node,
                                                                            "Replacement",
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            string_expression_cache);
          if (!input1 || !input2) {
            break;
          }
          expression.kind = LN_StringExpressionKind::Replace;
          expression.input1 = *input1;
          expression.input2 = *input2;
          result = program.AddStringExpression(expression);
          break;
        }
        case LN_STRING_OP_UPPER:
          expression.kind = LN_StringExpressionKind::ToUppercase;
          result = program.AddStringExpression(expression);
          break;
        case LN_STRING_OP_LOWER:
          expression.kind = LN_StringExpressionKind::ToLowercase;
          result = program.AddStringExpression(expression);
          break;
        case LN_STRING_OP_ZFILL: {
          IntExpressionCache int_expression_cache;
          const std::optional<uint32_t> width = BuildInputIntExpression(program,
                                                                        node,
                                                                        "Length",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        int_expression_cache);
          if (!width) {
            break;
          }
          expression.kind = LN_StringExpressionKind::ZeroFill;
          expression.int_expr_index = *width;
          result = program.AddStringExpression(expression);
          break;
        }
        default:
          break;
      }
      break;
    }
    case LN_NodeKind::FormattedString: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "String")) {
        break;
      }

      const std::optional<uint32_t> format = BuildInputStringExpression(program,
                                                                        node,
                                                                        "Format String",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input1 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "A",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input2 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "B",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input3 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "C",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      const std::optional<uint32_t> input4 = BuildInputStringExpression(program,
                                                                        node,
                                                                        "D",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        string_expression_cache);
      if (format && input1 && input2 && input3 && input4) {
        LN_StringExpression expression;
        expression.kind = LN_StringExpressionKind::Format;
        expression.input0 = *format;
        expression.input1 = *input1;
        expression.input2 = *input2;
        expression.input3 = *input3;
        expression.input4 = *input4;
        result = program.AddStringExpression(expression);
      }
      break;
    }
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::String) {
        result = AddConstantStringExpression(program, value->string_value);
      }
      break;
    }
  }

  string_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildInputVectorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputVectorExpression(program,
                                       *link.fromnode,
                                       *link.fromsock,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       vector_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Vector);
  if (!default_value || default_value->type != LN_ValueType::Vector) {
    return std::nullopt;
  }
  return AddConstantVectorExpression(program, default_value->vector_value);
}

std::optional<uint32_t> BuildInputRotationVectorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputVectorExpression(program,
                                       *link.fromnode,
                                       *link.fromsock,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       vector_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Rotation);
  if (!default_value || default_value->type != LN_ValueType::Rotation) {
    return std::nullopt;
  }
  return AddConstantVectorExpression(program, default_value->rotation_euler_value);
}

std::optional<uint32_t> BuildOutputVectorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache)
{
  const auto cache_iter = vector_expression_cache.find(&output_socket);
  if (cache_iter != vector_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    vector_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  BoolExpressionCache try_bool_cache;
  IntExpressionCache try_int_cache;
  StringExpressionCache try_string_cache;
  ColorExpressionCache try_color_cache;
  ValueExpressionCache try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            try_bool_cache,
            try_int_cache,
            float_expression_cache,
            try_string_cache,
            vector_expression_cache,
            try_color_cache,
            try_value_cache))
    {
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::FromGenericValue;
      expression.input0 = *value_expr;
      result = program.AddVectorExpression(expression);
    }
    vector_expression_cache[&output_socket] = result;
    return result;
  }
  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           try_bool_cache,
                                           try_int_cache,
                                           try_string_cache,
                                           vector_expression_cache,
                                           try_color_cache,
                                           try_value_cache,
                                           try_result))
  {
    if (output_socket.type != blender::SOCK_VECTOR &&
        output_socket.type != blender::SOCK_ROTATION && try_result)
    {
      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::FromGenericValue;
      expression.input0 = *try_result;
      try_result = program.AddVectorExpression(expression);
    }
    vector_expression_cache.emplace(&output_socket, try_result);
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::Collision:
    case LN_NodeKind::OnCollision: {
      const bool point_output = NamesMatch(output_socket.name, output_socket.identifier, "Point");
      const bool normal_output = NamesMatch(output_socket.name, output_socket.identifier, "Normal");
      if (!point_output && !normal_output) {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(
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
      const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Property",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache_local);
      const std::optional<uint32_t> material_expr = BuildInputValueExpression(
          program,
          node,
          "Material",
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
      expression.kind = point_output ? LN_VectorExpressionKind::CollisionHitPoint :
                                      LN_VectorExpressionKind::CollisionHitNormal;
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
      break;
    }
    case LN_NodeKind::GetBoneAttribute: {
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      if (const std::optional<uint32_t> value_expr = BuildBoneAttributeValueExpression(
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
              value_expression_cache_local))
      {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::FromGenericValue;
        expression.input0 = *value_expr;
        result = program.AddVectorExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetBonePoseRotation:
    case LN_NodeKind::GetBonePoseTransform: {
      if (definition.kind == LN_NodeKind::GetBonePoseTransform &&
          !NamesMatch(output_socket.name, output_socket.identifier, "Rotation"))
      {
        break;
      }
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      if (const std::optional<uint32_t> value_expr = BuildBonePoseRotationValueExpression(
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
              value_expression_cache_local))
      {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::FromGenericValue;
        expression.input0 = *value_expr;
        result = program.AddVectorExpression(expression);
      }
      break;
    }
    case LN_NodeKind::ValueVector: {
      const std::optional<LN_Value> value = ReadSocketDefault(output_socket,
                                                              LN_ValueType::Vector);
      if (value && value->type == LN_ValueType::Vector) {
        result = AddConstantVectorExpression(program, value->vector_value);
      }
      break;
    }
    case LN_NodeKind::CombineXY:
    case LN_NodeKind::CombineXYZ:
    case LN_NodeKind::CombineXYZW:
    case LN_NodeKind::Euler: {
      const std::optional<uint32_t> x = BuildInputFloatExpression(program,
                                                                  node,
                                                                  "X",
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache,
                                                                  &try_bool_cache,
                                                                  &vector_expression_cache);
      const std::optional<uint32_t> y = BuildInputFloatExpression(program,
                                                                  node,
                                                                  "Y",
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache,
                                                                  &try_bool_cache,
                                                                  &vector_expression_cache);
      std::optional<uint32_t> z;
      if (definition.kind == LN_NodeKind::CombineXYZ ||
          definition.kind == LN_NodeKind::CombineXYZW || definition.kind == LN_NodeKind::Euler)
      {
        z = BuildInputFloatExpression(program,
                                      node,
                                      "Z",
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      &try_bool_cache,
                                      &vector_expression_cache);
      }
      if (!x || !y ||
          ((definition.kind == LN_NodeKind::CombineXYZ ||
            definition.kind == LN_NodeKind::CombineXYZW || definition.kind == LN_NodeKind::Euler) &&
           !z))
      {
        break;
      }

      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::Combine;
      expression.input0 = *x;
      expression.input1 = *y;
      expression.input2 = z.value_or(LN_INVALID_INDEX);
      expression.vector_value = MT_Vector3(FloatExpressionConstantFallback(program, *x),
                                           FloatExpressionConstantFallback(program, *y),
                                           z ? FloatExpressionConstantFallback(program, *z) : 0.0f);
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::ResizeVector: {
      const std::optional<uint32_t> vector = BuildInputVectorExpression(program,
                                                                        node,
                                                                        "Vector",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        float_expression_cache,
                                                                        vector_expression_cache);
      if (!vector) {
        break;
      }

      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::Resize;
      expression.input0 = *vector;
      expression.float_value = float(std::clamp(int(node.custom1) + 2, 2, 4));
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::MatrixToXYZ: {
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> value_expr = BuildMatrixToEulerValueExpression(
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
      if (value_expr) {
        LN_VectorExpression expression;
        expression.kind = LN_VectorExpressionKind::FromGenericValue;
        expression.input0 = *value_expr;
        result = program.AddVectorExpression(expression);
      }
      break;
    }
    case LN_NodeKind::GetAxisVector: {
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(program,
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
      expression.kind = LN_VectorExpressionKind::AxisVector;
      expression.property_ref_index = uint32_t(std::max(0, int(node.custom1)));
      if (object_expr) {
        expression.input0 = *object_expr;
      }
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::VectorToRotation: {
      const std::optional<uint32_t> direction = BuildInputVectorExpression(
          program,
          node,
          "Direction",
          node_definitions,
          input_links,
          value_cache,
          float_expression_cache,
          vector_expression_cache);
      const std::optional<uint32_t> up =
          (node.custom2 & 1) != 0 ?
              BuildInputVectorExpression(program,
                                         node,
                                         "Up",
                                         node_definitions,
                                         input_links,
                                         value_cache,
                                         float_expression_cache,
                                         vector_expression_cache) :
              std::optional<uint32_t>(AddConstantVectorExpression(
                  program, MT_Vector3(0.0f, 0.0f, 0.0f)));
      if (!direction || !up) {
        break;
      }

      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::VectorToRotation;
      expression.input0 = *direction;
      expression.input1 = *up;
      expression.property_ref_index = uint32_t(std::clamp(int(node.custom1), 0, 5));
      result = program.AddVectorExpression(expression);
      break;
    }
    case LN_NodeKind::VectorRotate: {
      const std::optional<uint32_t> origin = BuildInputVectorExpression(program,
                                                                        node,
                                                                        "Origin",
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        float_expression_cache,
                                                                        vector_expression_cache);
      const std::optional<uint32_t> pivot = BuildInputVectorExpression(program,
                                                                       node,
                                                                       "Pivot",
                                                                       node_definitions,
                                                                       input_links,
                                                                       value_cache,
                                                                       float_expression_cache,
                                                                       vector_expression_cache);
        const std::optional<uint32_t> axis = node.custom1 == 3 ?
          BuildInputRotationVectorExpression(program,
                                             node,
                                             "Euler",
                                             node_definitions,
                                             input_links,
                                             value_cache,
                                             float_expression_cache,
                                             vector_expression_cache) :
          BuildInputVectorExpression(program,
                       node,
                       "Axis",
                       node_definitions,
                       input_links,
                       value_cache,
                       float_expression_cache,
                       vector_expression_cache);
      const std::optional<uint32_t> angle = BuildInputFloatExpression(program,
                                                                      node,
                                                                      "Angle",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      &try_bool_cache,
                                                                      &vector_expression_cache);
      if (!origin || !pivot || !axis || !angle) {
        break;
      }

      LN_VectorExpression expression;
      expression.kind = LN_VectorExpressionKind::RotateAroundAxis;
      expression.input0 = *origin;
      expression.input1 = *pivot;
      expression.input2 = *axis;
      expression.float_expr_index = *angle;
      expression.property_ref_index = uint32_t(std::max(0, int(node.custom1)) * 4 +
                       std::max(0, int(node.custom2)));
      result = program.AddVectorExpression(expression);
      break;
    }
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::Vector) {
        result = AddConstantVectorExpression(program, value->vector_value);
      }
      break;
    }
  }

  vector_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildInputColorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    ColorExpressionCache &color_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputColorExpression(program,
                                      *link.fromnode,
                                      *link.fromsock,
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache,
                                      color_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Color);
  if (!default_value || default_value->type != LN_ValueType::Color) {
    return std::nullopt;
  }
  return AddConstantColorExpression(program, default_value->color_value);
}

std::optional<uint32_t> BuildOutputColorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    ColorExpressionCache &color_expression_cache)
{
  const auto cache_iter = color_expression_cache.find(&output_socket);
  if (cache_iter != color_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    color_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  BoolExpressionCache try_bool_cache;
  IntExpressionCache try_int_cache;
  StringExpressionCache try_string_cache;
  VectorExpressionCache try_vector_cache;
  ValueExpressionCache try_value_cache;
  if (const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      pin != nullptr && pin->value_type == LN_ValueType::Generic)
  {
    if (const std::optional<uint32_t> value_expr = BuildOutputValueExpression(
            program,
            node,
            output_socket,
            node_definitions,
            input_links,
            value_cache,
            try_bool_cache,
            try_int_cache,
            float_expression_cache,
            try_string_cache,
            try_vector_cache,
            color_expression_cache,
            try_value_cache))
    {
      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind::FromGenericValue;
      expression.input0 = *value_expr;
      result = program.AddColorExpression(expression);
    }
    color_expression_cache[&output_socket] = result;
    return result;
  }
  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
                                           node,
                                           output_socket,
                                           definition,
                                           node_definitions,
                                           input_links,
                                           value_cache,
                                           float_expression_cache,
                                           try_bool_cache,
                                           try_int_cache,
                                           try_string_cache,
                                           try_vector_cache,
                                           color_expression_cache,
                                           try_value_cache,
                                           try_result))
  {
    return try_result;
  }

  switch (definition.kind) {
    case LN_NodeKind::ValueColor: {
      const std::optional<LN_Value> value = ReadSocketDefault(output_socket,
                                                              LN_ValueType::Color);
      if (value && value->type == LN_ValueType::Color) {
        result = AddConstantColorExpression(program, value->color_value);
      }
      break;
    }
    case LN_NodeKind::ColorRGB:
    case LN_NodeKind::ColorRGBA: {
      const std::optional<uint32_t> r = BuildInputFloatExpression(program,
                                                                  node,
                                                                  "R",
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache);
      const std::optional<uint32_t> g = BuildInputFloatExpression(program,
                                                                  node,
                                                                  "G",
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache);
      const std::optional<uint32_t> b = BuildInputFloatExpression(program,
                                                                  node,
                                                                  "B",
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache);
      std::optional<uint32_t> a;
      if (definition.kind == LN_NodeKind::ColorRGBA) {
        a = BuildInputFloatExpression(program,
                                      node,
                                      "A",
                                      node_definitions,
                                      input_links,
                                      value_cache,
                                      float_expression_cache);
      }
      if (!r || !g || !b || (definition.kind == LN_NodeKind::ColorRGBA && !a)) {
        break;
      }

      LN_ColorExpression expression;
      expression.kind = LN_ColorExpressionKind::Combine;
      expression.input0 = *r;
      expression.input1 = *g;
      expression.input2 = *b;
      expression.input3 = a.value_or(AddConstantFloatExpression(program, 1.0f));
      expression.color_value = MT_Vector4(FloatExpressionConstantFallback(program, *r),
                                          FloatExpressionConstantFallback(program, *g),
                                          FloatExpressionConstantFallback(program, *b),
                                          a ? FloatExpressionConstantFallback(program, *a) : 1.0f);
      result = program.AddColorExpression(expression);
      break;
    }
    default: {
      const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                output_socket,
                                                                node_definitions,
                                                                input_links,
                                                                value_cache);
      if (value && value->type == LN_ValueType::Color) {
        result = AddConstantColorExpression(program, value->color_value);
      }
      break;
    }
  }

  color_expression_cache[&output_socket] = result;
  return result;
}

std::optional<uint32_t> BuildInputValueExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
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
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return BuildOutputValueExpression(program,
                                      *link.fromnode,
                                      *link.fromsock,
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

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter != node_definitions.end()) {
    if (const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->inputs, *socket)) {
      if (pin->value_type == LN_ValueType::Generic) {
        const std::optional<LN_Value> default_value = ReadGenericSocketDefault(*socket);
        if (default_value) {
          return AddConstantValueExpression(program, *default_value);
        }
      }
      else if (pin->value_type != LN_ValueType::None) {
        const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, pin->value_type);
        if (default_value) {
          return AddConstantValueExpression(program, *default_value);
        }
      }
    }
  }

  return AddConstantValueExpression(program, MakeNoneValue());
}

std::optional<uint32_t> BuildOptionalObjectTargetExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
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
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    return BuildInputValueExpression(program,
                                     node,
                                     socket_name,
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

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return std::nullopt;
  }

  const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->inputs, *socket);
  if (pin == nullptr || pin->value_type != LN_ValueType::ObjectRef) {
    return std::nullopt;
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::ObjectRef);
  if (!default_value || !default_value->exists) {
    return std::nullopt;
  }

  return AddConstantValueExpression(program, *default_value);
}

std::optional<uint32_t> BuildInputOrActiveCameraExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
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
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    return BuildInputValueExpression(program,
                                     node,
                                     socket_name,
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

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter != node_definitions.end()) {
    if (const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->inputs, *socket)) {
      if (pin->value_type == LN_ValueType::ObjectRef) {
        const std::optional<LN_Value> default_value = ReadSocketDefault(*socket,
                                                                        LN_ValueType::ObjectRef);
        if (default_value && default_value->exists) {
          return AddConstantValueExpression(program, *default_value);
        }
      }
    }
  }

  return AddActiveCameraValueExpression(program);
}

std::optional<uint32_t> BuildOutputValueExpression(
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
    ValueExpressionCache &value_expression_cache)
{
  const auto cache_iter = value_expression_cache.find(&output_socket);
  if (cache_iter != value_expression_cache.end()) {
    return cache_iter->second;
  }

  std::optional<uint32_t> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    value_expression_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;

  if ((definition.kind == LN_NodeKind::Collision || definition.kind == LN_NodeKind::OnCollision) &&
      (NamesMatch(output_socket.name, output_socket.identifier, "Colliding") ||
       NamesMatch(output_socket.name, output_socket.identifier, "On Collision")))
  {
    const std::optional<uint32_t> bool_expr = BuildOutputBoolExpression(program,
                                                                        node,
                                                                        output_socket,
                                                                        node_definitions,
                                                                        input_links,
                                                                        value_cache,
                                                                        float_expression_cache,
                                                                        bool_expression_cache);
    if (bool_expr) {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::FromBool;
      expression.input0 = *bool_expr;
      result = program.AddValueExpression(expression);
    }
    value_expression_cache[&output_socket] = result;
    return result;
  }

  if ((definition.kind == LN_NodeKind::Collision || definition.kind == LN_NodeKind::OnCollision) &&
      NamesMatch(output_socket.name, output_socket.identifier, "Contact Count"))
  {
    const std::optional<uint32_t> int_expr = BuildOutputIntExpression(program,
                                                                      node,
                                                                      output_socket,
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      int_expression_cache);
    if (int_expr) {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::FromInt;
      expression.input0 = *int_expr;
      result = program.AddValueExpression(expression);
    }
    value_expression_cache[&output_socket] = result;
    return result;
  }

  if ((definition.kind == LN_NodeKind::Collision || definition.kind == LN_NodeKind::OnCollision) &&
      (NamesMatch(output_socket.name, output_socket.identifier, "Point") ||
       NamesMatch(output_socket.name, output_socket.identifier, "Normal")))
  {
    const std::optional<uint32_t> vector_expr = BuildOutputVectorExpression(program,
                                                                            node,
                                                                            output_socket,
                                                                            node_definitions,
                                                                            input_links,
                                                                            value_cache,
                                                                            float_expression_cache,
                                                                            vector_expression_cache);
    if (vector_expr) {
      LN_ValueExpression expression;
      expression.kind = LN_ValueExpressionKind::FromVector;
      expression.input0 = *vector_expr;
      result = program.AddValueExpression(expression);
    }
    value_expression_cache[&output_socket] = result;
    return result;
  }

  std::optional<uint32_t> try_result;
  if (TryCompileRegisteredExpressionOutput(program,
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
                                           try_result))
  {
    if (try_result) {
      const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      if (pin != nullptr) {
        LN_ValueExpression expression;
        bool wrap_typed_output = true;
        switch (pin->value_type) {
          case LN_ValueType::Bool:
            expression.kind = LN_ValueExpressionKind::FromBool;
            break;
          case LN_ValueType::Int:
            expression.kind = LN_ValueExpressionKind::FromInt;
            break;
          case LN_ValueType::Float:
            expression.kind = LN_ValueExpressionKind::FromFloat;
            break;
          case LN_ValueType::String:
            expression.kind = LN_ValueExpressionKind::FromString;
            break;
          case LN_ValueType::Vector:
            expression.kind = LN_ValueExpressionKind::FromVector;
            break;
          case LN_ValueType::Color:
            expression.kind = LN_ValueExpressionKind::FromColor;
            break;
          case LN_ValueType::Rotation:
            expression.kind = LN_ValueExpressionKind::FromRotation;
            break;
          default:
            wrap_typed_output = false;
            result = try_result;
            break;
        }
        if (wrap_typed_output) {
          expression.input0 = *try_result;
          result = program.AddValueExpression(expression);
        }
      }
      else {
        result = try_result;
      }
    }
    value_expression_cache[&output_socket] = result;
    return result;
  }

  switch (definition.kind) {
    case LN_NodeKind::Collision:
    case LN_NodeKind::OnCollision: {
      const bool collided_object_output = NamesMatch(
          output_socket.name, output_socket.identifier, "Collided Object");
      const bool collided_objects_output = NamesMatch(
          output_socket.name, output_socket.identifier, "Collided Objects");
      const bool points_output = NamesMatch(output_socket.name, output_socket.identifier, "Points");
      const bool normals_output = NamesMatch(output_socket.name, output_socket.identifier, "Normals");
      if (!collided_object_output && !collided_objects_output && !points_output && !normals_output) {
        break;
      }
      FloatExpressionCache float_expression_cache_local;
      BoolExpressionCache bool_expression_cache_local;
      IntExpressionCache int_expression_cache_local;
      StringExpressionCache string_expression_cache_local;
      VectorExpressionCache vector_expression_cache_local;
      ColorExpressionCache color_expression_cache_local;
      ValueExpressionCache value_expression_cache_local;
      const std::optional<uint32_t> object_expr = BuildInputValueExpression(
          program,
          node,
          "Object",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache_local,
          string_expression_cache_local,
          vector_expression_cache_local,
          color_expression_cache_local,
          value_expression_cache_local);
      const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                               node,
                                                                               "Property",
                                                                               node_definitions,
                                                                               input_links,
                                                                               value_cache,
                                                                               string_expression_cache_local);
      const std::optional<uint32_t> material_expr = BuildInputValueExpression(
          program,
          node,
          "Material",
          node_definitions,
          input_links,
          value_cache,
          bool_expression_cache_local,
          int_expression_cache_local,
          float_expression_cache_local,
          string_expression_cache_local,
          vector_expression_cache_local,
          color_expression_cache_local,
          value_expression_cache_local);
      LN_ValueExpression expression;
      if (collided_object_output) {
        expression.kind = LN_ValueExpressionKind::CollisionHitObject;
      }
      else if (collided_objects_output) {
        expression.kind = LN_ValueExpressionKind::CollisionHitObjects;
      }
      else if (points_output) {
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
      break;
    }
    case LN_NodeKind::CombineXYZW:
      result = BuildCombineVector4ValueExpression(program,
                                                  node,
                                                  node_definitions,
                                                  input_links,
                                                  value_cache,
                                                  bool_expression_cache,
                                                  float_expression_cache,
                                                  vector_expression_cache);
      break;
    case LN_NodeKind::ResizeVector:
      result = BuildResizeVectorValueExpression(program,
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
      break;
    case LN_NodeKind::XYZToMatrix:
      result = BuildEulerToMatrixValueExpression(program,
                                                node,
                                                node_definitions,
                                                input_links,
                                                value_cache,
                                                float_expression_cache,
                                                vector_expression_cache);
      break;
    case LN_NodeKind::MatrixToXYZ:
      result = BuildMatrixToEulerValueExpression(program,
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
      break;
    case LN_NodeKind::Loop:
    case LN_NodeKind::LoopFromList:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Value") &&
          g_active_loop_frame_cache != nullptr)
      {
        if (const auto iter = g_active_loop_frame_cache->value_expressions.find(&node);
            iter != g_active_loop_frame_cache->value_expressions.end())
        {
          result = iter->second;
        }
      }
      break;
    case LN_NodeKind::GetBoneAttribute:
      result = BuildBoneAttributeValueExpression(program,
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
      break;
    case LN_NodeKind::GetBonePoseRotation:
    case LN_NodeKind::GetBonePoseTransform:
      if (definition.kind == LN_NodeKind::GetBonePoseRotation ||
          NamesMatch(output_socket.name, output_socket.identifier, "Rotation"))
      {
        result = BuildBonePoseRotationValueExpression(program,
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
      }
      break;
    case LN_NodeKind::GetMaterialFromSlot:
      if (NamesMatch(output_socket.name, output_socket.identifier, "Material")) {
        result = BuildMaterialSlotValueExpression(program,
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
      }
      break;
    case LN_NodeKind::GetMaterialParameter: {
      if (NamesMatch(output_socket.name, output_socket.identifier, "Value")) {
        result = BuildMaterialNodeValueExpression(program,
                                                  node,
                                                  definition.kind ==
                                                      LN_NodeKind::GetMaterialParameter,
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
      break;
    }
    case LN_NodeKind::GetEditorNodeValue: {
      if (NamesMatch(output_socket.name, output_socket.identifier, "Value")) {
        result = node.custom1 == 1 ?
                     BuildMaterialNodeValueExpression(program,
                                                      node,
                                                      false,
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
                     BuildEditorNodeValueExpression(program,
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
      }
      break;
    }
    case LN_NodeKind::GetSound:
    case LN_NodeKind::GetImage:
    case LN_NodeKind::GetFont: {
      const char *input_name = definition.kind == LN_NodeKind::GetSound ? "Sound File" :
                               definition.kind == LN_NodeKind::GetImage ? "Image" : "Font";
      result = BuildInputValueExpression(program,
                                         node,
                                         input_name,
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
      break;
    }
    default: {
      const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, output_socket);
      if (pin != nullptr) {
        switch (pin->value_type) {
          case LN_ValueType::Bool: {
            const std::optional<uint32_t> bool_expr = BuildOutputBoolExpression(program,
                                                                                node,
                                                                                output_socket,
                                                                                node_definitions,
                                                                                input_links,
                                                                                value_cache,
                                                                                float_expression_cache,
                                                                                bool_expression_cache);
            if (bool_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromBool;
              expression.input0 = *bool_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Int: {
            const std::optional<uint32_t> int_expr = BuildOutputIntExpression(program,
                                                                              node,
                                                                              output_socket,
                                                                              node_definitions,
                                                                              input_links,
                                                                              value_cache,
                                                                              int_expression_cache);
            if (int_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromInt;
              expression.input0 = *int_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Float: {
            const std::optional<uint32_t> float_expr = BuildOutputFloatExpression(program,
                                                                                  node,
                                                                                  output_socket,
                                                                                  node_definitions,
                                                                                  input_links,
                                                                                  value_cache,
                                                                                  float_expression_cache,
                                                                                  &bool_expression_cache,
                                                                                  &vector_expression_cache);
            if (float_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromFloat;
              expression.input0 = *float_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::String: {
            const std::optional<uint32_t> string_expr = BuildOutputStringExpression(program,
                                                                                    node,
                                                                                    output_socket,
                                                                                    node_definitions,
                                                                                    input_links,
                                                                                    value_cache,
                                                                                    string_expression_cache);
            if (string_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromString;
              expression.input0 = *string_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Vector: {
            const std::optional<uint32_t> vector_expr = BuildOutputVectorExpression(program,
                                                                                    node,
                                                                                    output_socket,
                                                                                    node_definitions,
                                                                                    input_links,
                                                                                    value_cache,
                                                                                    float_expression_cache,
                                                                                    vector_expression_cache);
            if (vector_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromVector;
              expression.input0 = *vector_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Vector4:
          case LN_ValueType::Matrix:
            break;
          case LN_ValueType::Color: {
            const std::optional<uint32_t> color_expr = BuildOutputColorExpression(program,
                                                                                  node,
                                                                                  output_socket,
                                                                                  node_definitions,
                                                                                  input_links,
                                                                                  value_cache,
                                                                                  float_expression_cache,
                                                                                  color_expression_cache);
            if (color_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromColor;
              expression.input0 = *color_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Rotation: {
            const std::optional<uint32_t> vector_expr = BuildOutputVectorExpression(program,
                                                                                    node,
                                                                                    output_socket,
                                                                                    node_definitions,
                                                                                    input_links,
                                                                                    value_cache,
                                                                                    float_expression_cache,
                                                                                    vector_expression_cache);
            if (vector_expr) {
              LN_ValueExpression expression;
              expression.kind = LN_ValueExpressionKind::FromRotation;
              expression.input0 = *vector_expr;
              result = program.AddValueExpression(expression);
            }
            break;
          }
          case LN_ValueType::Generic:
          case LN_ValueType::ObjectRef:
          case LN_ValueType::SceneRef:
          case LN_ValueType::CollectionRef:
          case LN_ValueType::DatablockRef:
          case LN_ValueType::List:
          case LN_ValueType::Dict:
          case LN_ValueType::None:
            break;
        }
      }

      if (!result) {
        const std::optional<LN_Value> value = EvaluateOutputValue(node,
                                                                  output_socket,
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache);
        if (value) {
          result = AddConstantValueExpression(program, *value);
        }
      }
      break;
    }
  }

  value_expression_cache[&output_socket] = result;
  return result;
}

std::optional<LN_Value> EvaluateOutputValue(
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache)
{
  const auto cache_iter = value_cache.find(&output_socket);
  if (cache_iter != value_cache.end()) {
    return cache_iter->second;
  }

  std::optional<LN_Value> result;
  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    value_cache.emplace(&output_socket, result);
    return result;
  }

  const LN_NodeDefinition &definition = *definition_iter->second;
  switch (definition.kind) {
    case LN_NodeKind::ValueBool:
      result = ReadSocketDefault(output_socket, LN_ValueType::Bool);
      break;
    case LN_NodeKind::ValueInt:
      result = ReadSocketDefault(output_socket, LN_ValueType::Int);
      break;
    case LN_NodeKind::ValueFloat:
      result = ReadSocketDefault(output_socket, LN_ValueType::Float);
      break;
    case LN_NodeKind::ValueString:
      result = ReadSocketDefault(output_socket, LN_ValueType::String);
      break;
    case LN_NodeKind::GetBonePoseRotation:
    case LN_NodeKind::GetBonePoseTransform:
    case LN_NodeKind::GetRigidBodyAttribute:
      break;
    case LN_NodeKind::StringOperation: {
      const std::optional<LN_Value> input0 = ReadInputValue(node,
                                                            "String",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> input1 = ReadInputValue(node,
                                                            "Substring",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      if (!input0 || input0->type != LN_ValueType::String) {
        break;
      }

      if (NamesMatch(output_socket.name, output_socket.identifier, "String")) {
        LN_Value value;
        value.type = LN_ValueType::String;
        switch (node.custom1) {
          case LN_STRING_OP_JOIN:
            if (input1 && input1->type == LN_ValueType::String) {
              value.string_value = input0->string_value + input1->string_value;
              result = value;
            }
            break;
          case LN_STRING_OP_REPLACE: {
            const std::optional<LN_Value> input2 = ReadInputValue(node,
                                                                  "Replacement",
                                                                  LN_ValueType::String,
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache);
            if (input1 && input1->type == LN_ValueType::String && input2 &&
                input2->type == LN_ValueType::String)
            {
              value.string_value = ReplaceStringOccurrences(input0->string_value,
                                                            input1->string_value,
                                                            input2->string_value);
              result = value;
            }
            break;
          }
          case LN_STRING_OP_UPPER:
            value.string_value = ToCaseString(input0->string_value, true);
            result = value;
            break;
          case LN_STRING_OP_LOWER:
            value.string_value = ToCaseString(input0->string_value, false);
            result = value;
            break;
          case LN_STRING_OP_ZFILL: {
            const std::optional<LN_Value> width = ReadInputValue(node,
                                                                 "Length",
                                                                 LN_ValueType::Int,
                                                                 node_definitions,
                                                                 input_links,
                                                                 value_cache);
            if (width && width->type == LN_ValueType::Int) {
              value.string_value = ZeroFillString(input0->string_value, width->int_value);
              result = value;
            }
            break;
          }
        }
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Result")) {
        if (!input1 || input1->type != LN_ValueType::String) {
          break;
        }
        LN_Value value;
        value.type = LN_ValueType::Bool;
        switch (node.custom1) {
          case LN_STRING_OP_CONTAINS:
            value.bool_value = input0->string_value.find(input1->string_value) !=
                               std::string::npos;
            result = value;
            break;
          case LN_STRING_OP_STARTS_WITH:
            value.bool_value = StringStartsWith(input0->string_value, input1->string_value);
            result = value;
            break;
          case LN_STRING_OP_ENDS_WITH:
            value.bool_value = StringEndsWith(input0->string_value, input1->string_value);
            result = value;
            break;
        }
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Count") &&
               node.custom1 == LN_STRING_OP_COUNT)
      {
        if (input1 && input1->type == LN_ValueType::String) {
          LN_Value value;
          value.type = LN_ValueType::Int;
          value.int_value = CountStringOccurrences(input0->string_value, input1->string_value);
          result = value;
        }
      }
      break;
    }
    case LN_NodeKind::FormattedString: {
      if (!NamesMatch(output_socket.name, output_socket.identifier, "String")) {
        break;
      }

      const std::optional<LN_Value> format = ReadInputValue(node,
                                                            "Format String",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> input1 = ReadInputValue(node,
                                                            "A",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> input2 = ReadInputValue(node,
                                                            "B",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> input3 = ReadInputValue(node,
                                                            "C",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> input4 = ReadInputValue(node,
                                                            "D",
                                                            LN_ValueType::String,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      if (format && format->type == LN_ValueType::String && input1 &&
          input1->type == LN_ValueType::String && input2 &&
          input2->type == LN_ValueType::String && input3 &&
          input3->type == LN_ValueType::String && input4 &&
          input4->type == LN_ValueType::String)
      {
        LN_Value value;
        value.type = LN_ValueType::String;
        value.string_value = FormatStringSlots(format->string_value,
                                               input1->string_value,
                                               input2->string_value,
                                               input3->string_value,
                                               input4->string_value);
        result = value;
      }
      break;
    }
    case LN_NodeKind::ValueColor:
      result = ReadSocketDefault(output_socket, LN_ValueType::Color);
      break;
    case LN_NodeKind::ColorRGB:
    case LN_NodeKind::ColorRGBA: {
      const std::optional<LN_Value> r = ReadInputValue(node,
                                                       "R",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> g = ReadInputValue(node,
                                                       "G",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> b = ReadInputValue(node,
                                                       "B",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      std::optional<LN_Value> a;
      if (definition.kind == LN_NodeKind::ColorRGBA) {
        a = ReadInputValue(node,
                           "A",
                           LN_ValueType::Float,
                           node_definitions,
                           input_links,
                           value_cache);
      }
      if (r && r->type == LN_ValueType::Float && g && g->type == LN_ValueType::Float && b &&
          b->type == LN_ValueType::Float &&
          (definition.kind == LN_NodeKind::ColorRGB || (a && a->type == LN_ValueType::Float)))
      {
        LN_Value color_value;
        color_value.type = LN_ValueType::Color;
        color_value.color_value = MT_Vector4(r->float_value,
                                             g->float_value,
                                             b->float_value,
                                             a ? a->float_value : 1.0f);
        result = color_value;
      }
      break;
    }
    case LN_NodeKind::ValueVector:
      result = ReadSocketDefault(output_socket, LN_ValueType::Vector);
      break;
    case LN_NodeKind::CombineXYZW: {
      const std::optional<LN_Value> x = ReadInputValue(node,
                                                       "X",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> y = ReadInputValue(node,
                                                       "Y",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> z = ReadInputValue(node,
                                                       "Z",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> w = ReadInputValue(node,
                                                       "W",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      if (x && x->type == LN_ValueType::Float && y && y->type == LN_ValueType::Float && z &&
          z->type == LN_ValueType::Float && w && w->type == LN_ValueType::Float)
      {
        LN_Value vector_value;
        vector_value.type = LN_ValueType::Vector4;
        vector_value.exists = true;
        vector_value.vector4_value = MT_Vector4(x->float_value,
                                                y->float_value,
                                                z->float_value,
                                                w->float_value);
        result = vector_value;
      }
      break;
    }
    case LN_NodeKind::CombineXY:
    case LN_NodeKind::CombineXYZ:
    case LN_NodeKind::Euler: {
      const std::optional<LN_Value> x = ReadInputValue(node,
                                                       "X",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> y = ReadInputValue(node,
                                                       "Y",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      std::optional<LN_Value> z;
      if (definition.kind == LN_NodeKind::CombineXYZ || definition.kind == LN_NodeKind::Euler) {
        z = ReadInputValue(node,
                           "Z",
                           LN_ValueType::Float,
                           node_definitions,
                           input_links,
                           value_cache);
      }
      if (x && x->type == LN_ValueType::Float && y && y->type == LN_ValueType::Float &&
          (definition.kind == LN_NodeKind::CombineXY ||
           (z && z->type == LN_ValueType::Float)))
      {
        LN_Value vector_value;
        if (definition.kind == LN_NodeKind::Euler) {
          vector_value.type = LN_ValueType::Rotation;
          vector_value.rotation_euler_value = MT_Vector3(x->float_value,
                                                         y->float_value,
                                                         z->float_value);
        }
        else {
          vector_value.type = LN_ValueType::Vector;
          vector_value.vector_value = MT_Vector3(x->float_value,
                                                 y->float_value,
                                                 z ? z->float_value : 0.0f);
        }
        result = vector_value;
      }
      break;
    }
    case LN_NodeKind::ResizeVector: {
      const std::optional<LN_Value> input = ReadInputValue(node,
                                                           "Vector",
                                                           LN_ValueType::Vector,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      if (!input) {
        break;
      }
      const int target_size = std::clamp(int(node.custom1) + 2, 2, 4);
      MT_Vector4 components(0.0f, 0.0f, 0.0f, 0.0f);
      if (input->type == LN_ValueType::Vector4) {
        components = input->vector4_value;
      }
      else if (input->type == LN_ValueType::Color) {
        components = input->color_value;
      }
      else if (input->type == LN_ValueType::Vector) {
        components = MT_Vector4(input->vector_value.x(),
                                input->vector_value.y(),
                                input->vector_value.z(),
                                0.0f);
      }
      else if (input->type == LN_ValueType::Rotation) {
        components = MT_Vector4(input->rotation_euler_value.x(),
                                input->rotation_euler_value.y(),
                                input->rotation_euler_value.z(),
                                0.0f);
      }
      else {
        break;
      }

      LN_Value vector_value;
      vector_value.exists = true;
      if (target_size == 4) {
        vector_value.type = LN_ValueType::Vector4;
        vector_value.vector4_value = components;
      }
      else {
        vector_value.type = LN_ValueType::Vector;
        vector_value.vector_value = MT_Vector3(components.x(),
                                               components.y(),
                                               target_size >= 3 ? components.z() : 0.0f);
      }
      result = vector_value;
      break;
    }
    case LN_NodeKind::XYZToMatrix: {
      const std::optional<LN_Value> xyz = ReadInputValue(node,
                                                         "XYZ",
                                                         LN_ValueType::Vector,
                                                         node_definitions,
                                                         input_links,
                                                         value_cache);
      if (xyz && xyz->type == LN_ValueType::Vector) {
        LN_Value matrix_value;
        matrix_value.type = LN_ValueType::Matrix;
        matrix_value.exists = true;
        matrix_value.matrix_value = MT_Matrix3x3(xyz->vector_value);
        result = matrix_value;
      }
      break;
    }
    case LN_NodeKind::MatrixToXYZ: {
      const std::optional<LN_Value> matrix = ReadInputValue(node,
                                                            "Matrix",
                                                            LN_ValueType::Generic,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      if (matrix && matrix->type == LN_ValueType::Matrix) {
        MT_Scalar x = 0.0f;
        MT_Scalar y = 0.0f;
        MT_Scalar z = 0.0f;
        matrix->matrix_value.getEuler(x, y, z);
        LN_Value vector_value;
        vector_value.type = node.custom1 == 1 ? LN_ValueType::Rotation : LN_ValueType::Vector;
        vector_value.exists = true;
        if (vector_value.type == LN_ValueType::Rotation) {
          vector_value.rotation_euler_value = MT_Vector3(x, y, z);
        }
        else {
          vector_value.vector_value = MT_Vector3(x, y, z);
        }
        result = vector_value;
      }
      break;
    }
    case LN_NodeKind::SeparateXY:
    case LN_NodeKind::SeparateXYZ:
    case LN_NodeKind::SeparateEuler: {
      const std::optional<LN_Value> value =
          definition.kind == LN_NodeKind::SeparateEuler ?
              ReadInputValue(node,
                             "Rotation",
                             LN_ValueType::Rotation,
                             node_definitions,
                             input_links,
                             value_cache) :
              ReadInputValue(node,
                             "Vector",
                             LN_ValueType::Vector,
                             node_definitions,
                             input_links,
                             value_cache);
      if (!value || (definition.kind == LN_NodeKind::SeparateEuler ?
                         value->type != LN_ValueType::Rotation :
                         value->type != LN_ValueType::Vector))
      {
        break;
      }

      const MT_Vector3 vector = definition.kind == LN_NodeKind::SeparateEuler ?
                                    value->rotation_euler_value :
                                    value->vector_value;
      LN_Value float_value;
      float_value.type = LN_ValueType::Float;
      if (NamesMatch(output_socket.name, output_socket.identifier, "X")) {
        float_value.float_value = vector.x();
        result = float_value;
      }
      else if (NamesMatch(output_socket.name, output_socket.identifier, "Y")) {
        float_value.float_value = vector.y();
        result = float_value;
      }
      else if ((definition.kind == LN_NodeKind::SeparateXYZ ||
                definition.kind == LN_NodeKind::SeparateEuler) &&
               NamesMatch(output_socket.name, output_socket.identifier, "Z"))
      {
        float_value.float_value = vector.z();
        result = float_value;
      }
      break;
    }
    case LN_NodeKind::InvertValue: {
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      if (value && value->type == LN_ValueType::Float) {
        LN_Value inverted_value;
        inverted_value.type = LN_ValueType::Float;
        inverted_value.float_value = -value->float_value;
        result = inverted_value;
      }
      break;
    }
    case LN_NodeKind::ClampValue: {
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      const std::optional<LN_Value> min_value = ReadInputValue(node,
                                                               "Min",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_Value> max_value = ReadInputValue(node,
                                                               "Max",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      if (value && value->type == LN_ValueType::Float && min_value &&
          min_value->type == LN_ValueType::Float && max_value &&
          max_value->type == LN_ValueType::Float) {
        const float lower = std::min(min_value->float_value, max_value->float_value);
        const float upper = std::max(min_value->float_value, max_value->float_value);
        LN_Value clamped_value;
        clamped_value.type = LN_ValueType::Float;
        clamped_value.float_value = std::min(std::max(value->float_value, lower), upper);
        result = clamped_value;
      }
      break;
    }
    case LN_NodeKind::MapRange: {
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      const std::optional<LN_Value> from_min = ReadInputValue(node,
                                                              "From Min",
                                                              LN_ValueType::Float,
                                                              node_definitions,
                                                              input_links,
                                                              value_cache);
      const std::optional<LN_Value> from_max = ReadInputValue(node,
                                                              "From Max",
                                                              LN_ValueType::Float,
                                                              node_definitions,
                                                              input_links,
                                                              value_cache);
      const std::optional<LN_Value> to_min = ReadInputValue(node,
                                                            "To Min",
                                                            LN_ValueType::Float,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      const std::optional<LN_Value> to_max = ReadInputValue(node,
                                                            "To Max",
                                                            LN_ValueType::Float,
                                                            node_definitions,
                                                            input_links,
                                                            value_cache);
      if (value && value->type == LN_ValueType::Float && from_min &&
          from_min->type == LN_ValueType::Float && from_max &&
          from_max->type == LN_ValueType::Float && to_min &&
          to_min->type == LN_ValueType::Float && to_max &&
          to_max->type == LN_ValueType::Float) {
        const float source_range = from_max->float_value - from_min->float_value;
        const float normalized_value = (std::fabs(source_range) <= 1.0e-20f) ?
                                           0.0f :
                                           (value->float_value - from_min->float_value) /
                                               source_range;
        float mapped_value = to_min->float_value +
                             normalized_value * (to_max->float_value - to_min->float_value);
        if (node.custom1 != 0) {
          const float lower = std::min(to_min->float_value, to_max->float_value);
          const float upper = std::max(to_min->float_value, to_max->float_value);
          mapped_value = std::min(std::max(mapped_value, lower), upper);
        }

        LN_Value result_value;
        result_value.type = LN_ValueType::Float;
        result_value.float_value = mapped_value;
        result = result_value;
      }
      break;
    }
    case LN_NodeKind::Compare: {
      const std::optional<LN_Value> a = ReadInputValue(node,
                                                       "A",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> b = ReadInputValue(node,
                                                       "B",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_FloatCompareOperation> operation =
          FloatCompareOperationFromCustom1(node.custom1);
      if (a && a->type == LN_ValueType::Float && b && b->type == LN_ValueType::Float &&
          operation) {
        LN_Value value;
        value.type = LN_ValueType::Bool;
        value.bool_value = EvaluateFloatCompare(*operation, a->float_value, b->float_value);
        result = value;
      }
      break;
    }
    case LN_NodeKind::Threshold: {
      const std::optional<LN_Value> else_zero = ReadInputValue(node,
                                                               "Else 0",
                                                               LN_ValueType::Bool,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      const std::optional<LN_Value> threshold = ReadInputValue(node,
                                                               "Threshold",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_ThresholdOperation> operation = ThresholdOperationFromCustom1(
          node.custom1);
      if (else_zero && else_zero->type == LN_ValueType::Bool && value &&
          value->type == LN_ValueType::Float && threshold &&
          threshold->type == LN_ValueType::Float && operation) {
        LN_Value result_value;
        result_value.type = LN_ValueType::Float;
        result_value.float_value = EvaluateThreshold(*operation,
                                                     value->float_value,
                                                     threshold->float_value,
                                                     else_zero->bool_value);
        result = result_value;
      }
      break;
    }
    case LN_NodeKind::RangedThreshold: {
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      const std::optional<LN_Value> min_value = ReadInputValue(node,
                                                               "Min",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_Value> max_value = ReadInputValue(node,
                                                               "Max",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_RangeOperation> operation = RangeOperationFromCustom1(node.custom1);
      if (value && value->type == LN_ValueType::Float && min_value &&
          min_value->type == LN_ValueType::Float && max_value &&
          max_value->type == LN_ValueType::Float && operation) {
        LN_Value result_value;
        result_value.type = LN_ValueType::Float;
        result_value.float_value = EvaluateRange(*operation,
                                                 value->float_value,
                                                 min_value->float_value,
                                                 max_value->float_value) ?
                                       value->float_value :
                                       0.0f;
        result = result_value;
      }
      break;
    }
    case LN_NodeKind::WithinRange: {
      const std::optional<LN_Value> value = ReadInputValue(node,
                                                           "Value",
                                                           LN_ValueType::Float,
                                                           node_definitions,
                                                           input_links,
                                                           value_cache);
      const std::optional<LN_Value> min_value = ReadInputValue(node,
                                                               "Min",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      const std::optional<LN_Value> max_value = ReadInputValue(node,
                                                               "Max",
                                                               LN_ValueType::Float,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache);
      if (value && value->type == LN_ValueType::Float && min_value &&
          min_value->type == LN_ValueType::Float && max_value &&
          max_value->type == LN_ValueType::Float) {
        const std::optional<LN_RangeOperation> operation = RangeOperationFromCustom1(node.custom1);
        if (!operation) {
          break;
        }

        LN_Value result_value;
        result_value.type = LN_ValueType::Bool;
        result_value.bool_value = EvaluateRange(*operation,
                                                value->float_value,
                                                min_value->float_value,
                                                max_value->float_value);
        result = result_value;
      }
      break;
    }
    case LN_NodeKind::Math: {
      const std::optional<LN_Value> a = ReadInputValue(node,
                                                       "A",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      const std::optional<LN_Value> b = ReadInputValue(node,
                                                       "B",
                                                       LN_ValueType::Float,
                                                       node_definitions,
                                                       input_links,
                                                       value_cache);
      if (a && b && a->type == LN_ValueType::Float && b->type == LN_ValueType::Float) {
        const std::optional<float> math_value = EvaluateMath(node.custom1,
                                                             a->float_value,
                                                             b->float_value);
        if (math_value) {
          LN_Value value;
          value.type = LN_ValueType::Float;
          value.float_value = *math_value;
          result = value;
        }
      }
      break;
    }
    case LN_NodeKind::EventOnInit:
    case LN_NodeKind::EventOnFixedUpdate:
    case LN_NodeKind::Branch:
    case LN_NodeKind::ToggleProperty:
    case LN_NodeKind::ModifyProperty:
    case LN_NodeKind::ModifyPropertyClamped:
    case LN_NodeKind::SetTreeProperty:
    case LN_NodeKind::RemoveObject:
    case LN_NodeKind::AddObject:
    case LN_NodeKind::SetParent:
    case LN_NodeKind::RemoveParent:
    case LN_NodeKind::SetGravity:
    case LN_NodeKind::SetTimescale:
    case LN_NodeKind::SetCamera:
    case LN_NodeKind::SetCameraFov:
    case LN_NodeKind::SetCameraOrthoScale:
    case LN_NodeKind::SetFullscreen:
    case LN_NodeKind::SetResolution:
    case LN_NodeKind::SetVSync:
    case LN_NodeKind::ShowFramerate:
    case LN_NodeKind::ShowProfile:
    case LN_NodeKind::SetCollisionGroup:
    case LN_NodeKind::SetPhysics:
    case LN_NodeKind::SetDynamics:
    case LN_NodeKind::RebuildCollisionShape:
    case LN_NodeKind::SetRigidBodyAttribute:
    case LN_NodeKind::CharacterJump:
    case LN_NodeKind::SetCharacterGravity:
    case LN_NodeKind::SetCharacterJumpSpeed:
    case LN_NodeKind::SetCharacterMaxJumps:
    case LN_NodeKind::SetCharacterWalkDirection:
    case LN_NodeKind::SetCharacterVelocity:
    case LN_NodeKind::VehicleControl:
    case LN_NodeKind::VehicleAccelerate:
    case LN_NodeKind::VehicleBrake:
    case LN_NodeKind::VehicleSteer:
    case LN_NodeKind::VehicleSetAttributes:
    case LN_NodeKind::SetCursorVisibility:
    case LN_NodeKind::SetCursorPosition:
    case LN_NodeKind::GamepadVibration:
    case LN_NodeKind::GamepadLook:
    case LN_NodeKind::ApplyImpulse:
    case LN_NodeKind::MakeLightUnique:
    case LN_NodeKind::SetLightColor:
    case LN_NodeKind::SetLightPower:
    case LN_NodeKind::SetLightShadow:
    case LN_NodeKind::ApplyMovement:
    case LN_NodeKind::ApplyRotation:
    case LN_NodeKind::ApplyForce:
    case LN_NodeKind::ApplyTorque:
    case LN_NodeKind::SetGamePropertyInt:
    case LN_NodeKind::SetGamePropertyFloat:
    case LN_NodeKind::SetGamePropertyBool:
    case LN_NodeKind::SetGamePropertyString:
    case LN_NodeKind::StartLogicTree:
    case LN_NodeKind::StopLogicTree:
    case LN_NodeKind::RunLogicTree:
    case LN_NodeKind::InstallLogicTree:
    case LN_NodeKind::PlayAction:
    case LN_NodeKind::StopAction:
    case LN_NodeKind::SetActionFrame:
    case LN_NodeKind::StopAllSounds:
    case LN_NodeKind::PlaySound:
    case LN_NodeKind::StopSound:
    case LN_NodeKind::SetBonePoseLocation:
    case LN_NodeKind::SetBonePoseRotation:
    case LN_NodeKind::SetBonePoseScale:
    case LN_NodeKind::SetBonePoseTransform:
    case LN_NodeKind::Print:
    case LN_NodeKind::QuitGame:
    case LN_NodeKind::RestartGame:
    case LN_NodeKind::LoadBlendFile:
    case LN_NodeKind::SendEvent:
      break;
  }

  value_cache.emplace(&output_socket, result);
  return result;
}

}  // namespace ln_compiler
